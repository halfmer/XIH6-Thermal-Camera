#include "lepton_stream.h"
#include "usart.h"
#include "spi.h"
#include <stdio.h>
#include <string.h>

#define LEPTON_STREAM_BUF_COUNT  2U
/* 32-byte-aligned stride so both ping-pong buffers start on a cache line;
   HAL_UART_Transmit_DMA still sends exactly LEPTON_STREAM_FRAME_LEN bytes. */
#define LEPTON_STREAM_BUF_STRIDE (((LEPTON_STREAM_FRAME_LEN + 31U) / 32U) * 32U)

/* stream_active: 0 = idle for host commands, 1 = binary frames are sent.
   Debug logs are on USART1; UART4/CH340 stays binary-clean for the host. */
DMA_HandleTypeDef           hdma_uart4_tx;
DMA_HandleTypeDef           hdma_spi5_tx;
/* Default ON: the WiFi bridge has no wire to send 'S'; the Qt serial modes
   send 'P'/'S' themselves on port open, so both hosts get what they need. */
static volatile uint8_t     stream_active = 1;
/* Runtime transport selector. Both paths stay initialized; switching aborts
   any in-flight DMA of the old path from thread context (Poll), never from
   the RX interrupt. Power-on default = UART4/CH340; when the ESP32 bridge
   reports ready (PG2 rising edge) we auto-switch to SPI5/TCP. */
static volatile uint8_t     stream_link = LEPTON_STREAM_LINK_UART4;
static volatile uint8_t     stream_link_pending = 0xFFU;   /* 0xFF = none */
static volatile uint32_t    stream_busy_since = 0;
static volatile uint32_t    stream_spi_timeout_count = 0;
static uint8_t              esp32_ready_last = 0;          /* PG2 level memory */
static volatile uint8_t     stream_ok_pending = 1;         /* report first delivered frame */
static uint32_t             ok_last_cplt = 0;
static volatile uint8_t     stream_rx_byte = 0;
static UART_HandleTypeDef  *stream_huart = NULL;
static uint16_t             stream_frame_id = 0;
static char                 stream_cmd_buf[20];
static uint8_t              stream_cmd_len = 0;
static uint8_t              stream_buf[LEPTON_STREAM_BUF_COUNT][LEPTON_STREAM_BUF_STRIDE] __attribute__((aligned(32)));
static uint8_t              stream_build_index = 0;
static volatile uint8_t     stream_tx_busy = 0;
static volatile uint8_t     stream_tx_index = 0xFFU;
static volatile uint32_t    stream_tx_ok_count = 0;
static volatile uint32_t    stream_tx_fail_count = 0;
static volatile uint32_t    stream_dma_cplt_count = 0;
static volatile uint32_t    stream_dma_busy_drop_count = 0;
static volatile uint32_t    stream_dma_error_count = 0;
static volatile uint32_t    stream_cmd_s_count = 0;
static volatile uint32_t    stream_cmd_p_count = 0;
static volatile uint32_t    stream_uart_error_count = 0;
/* ESP32 IO0 (PJ7) sequencing: 0 = auto (low 30 s, then release high),
   1 = a manual 'B'/'R' took over for the rest of the session. */
static volatile uint8_t     esp32_io0_manual = 0;
static uint8_t              esp32_io0_released = 0;

#define LEPTON_STREAM_DRDY_HIGH() HAL_GPIO_WritePin(ESP_IO39_GPIO_Port, ESP_IO39_Pin, GPIO_PIN_SET)
#define LEPTON_STREAM_DRDY_LOW()  HAL_GPIO_WritePin(ESP_IO39_GPIO_Port, ESP_IO39_Pin, GPIO_PIN_RESET)

/* Reconfigure the CubeMX SPI5 (unused touch-panel master) as a TX-only SLAVE
   on the ESP32 header pins. Runtime re-config on top of MX_SPI5_Init, same
   pattern as Lepton_SPI_Config, so a CubeMX regen cannot silently undo it. */
static void Lepton_Stream_SPI5_Config(void)
{
    GPIO_InitTypeDef gi = {0};

    __HAL_RCC_GPIOK_CLK_ENABLE();
    __HAL_RCC_GPIOJ_CLK_ENABLE();
    gi.Mode      = GPIO_MODE_AF_PP;
    gi.Pull      = GPIO_NOPULL;
    gi.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gi.Alternate = GPIO_AF5_SPI5;
    gi.Pin = GPIO_PIN_0;                       /* PK0  = SPI5_SCK  <- IO37 */
    HAL_GPIO_Init(GPIOK, &gi);
    gi.Pin = GPIO_PIN_11;                      /* PJ11 = SPI5_MISO -> IO36 */
    HAL_GPIO_Init(GPIOJ, &gi);
    __HAL_RCC_GPIOH_CLK_ENABLE();
    gi.Pin  = GPIO_PIN_5;                      /* PH5  = SPI5_NSS  <- IO47 */
    gi.Pull = GPIO_PULLUP;                     /* deselected if ESP32 absent */
    HAL_GPIO_Init(GPIOH, &gi);

    if (HAL_SPI_DeInit(&hspi5) != HAL_OK)
        Error_Handler();
    hspi5.Init.Mode          = SPI_MODE_SLAVE;
    hspi5.Init.Direction     = SPI_DIRECTION_2LINES_TXONLY;
    hspi5.Init.DataSize      = SPI_DATASIZE_8BIT;
    hspi5.Init.CLKPolarity   = SPI_POLARITY_LOW;   /* mode 0 = ESP32 default */
    hspi5.Init.CLKPhase      = SPI_PHASE_1EDGE;
    /* Hardware NSS (PH5, ESP32 CS): while high the slave ignores SCK
       entirely, so inter-frame clock glitches cannot shift the bit counter.
       Intra-frame alignment is already per-transaction (TSIZE/EOT). */
    hspi5.Init.NSS           = SPI_NSS_HARD_INPUT;
    hspi5.Init.FirstBit      = SPI_FIRSTBIT_MSB;
    hspi5.Init.FifoThreshold = SPI_FIFO_THRESHOLD_01DATA;
    if (HAL_SPI_Init(&hspi5) != HAL_OK)
        Error_Handler();

    __HAL_RCC_DMA1_CLK_ENABLE();
    hdma_spi5_tx.Instance                 = DMA1_Stream2;
    hdma_spi5_tx.Init.Request             = DMA_REQUEST_SPI5_TX;
    hdma_spi5_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    hdma_spi5_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_spi5_tx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_spi5_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_spi5_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    hdma_spi5_tx.Init.Mode                = DMA_NORMAL;
    hdma_spi5_tx.Init.Priority            = DMA_PRIORITY_LOW;
    hdma_spi5_tx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_spi5_tx) != HAL_OK)
        Error_Handler();
    __HAL_LINKDMA(&hspi5, hdmatx, hdma_spi5_tx);

    HAL_NVIC_SetPriority(DMA1_Stream2_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream2_IRQn);
    /* H7 SPI DMA completion is signalled through the SPI EOT interrupt */
    HAL_NVIC_SetPriority(SPI5_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(SPI5_IRQn);

    LEPTON_STREAM_DRDY_LOW();
}

static void Lepton_Stream_Usart1Print(const char *s)
{
    (void)HAL_UART_Transmit(&huart1, (const uint8_t *)s,
                            (uint16_t)strlen(s), 30);
}

static void Lepton_Stream_RequestLink(uint8_t target)
{
    if ((target == LEPTON_STREAM_LINK_UART4) || (target == LEPTON_STREAM_LINK_SPI5))
        stream_link_pending = target;
}

static void Lepton_Stream_HandleCommandLine(const char *cmd)
{
    if ((strcmp(cmd, "MODE_TOGGLE") == 0) || (strcmp(cmd, "T") == 0))
    {
        Lepton_Stream_RequestLink((stream_link == LEPTON_STREAM_LINK_SPI5)
                                  ? LEPTON_STREAM_LINK_UART4
                                  : LEPTON_STREAM_LINK_SPI5);
    }
    else if ((strcmp(cmd, "MODE_UART4") == 0) || (strcmp(cmd, "U") == 0))
    {
        Lepton_Stream_RequestLink(LEPTON_STREAM_LINK_UART4);
    }
    else if ((strcmp(cmd, "MODE_TCP") == 0) || (strcmp(cmd, "W") == 0))
    {
        Lepton_Stream_RequestLink(LEPTON_STREAM_LINK_SPI5);
    }
}

static void Lepton_Stream_HandleCommandByte(uint8_t b)
{
    if (stream_cmd_len == 0U)
    {
        if (b == 'S')
        {
            stream_active = 1;
            stream_cmd_s_count++;
            return;
        }
        if (b == 'P')
        {
            stream_active = 0;
            stream_cmd_p_count++;
            return;
        }
        if (b == 'T')
        {
            Lepton_Stream_RequestLink((stream_link == LEPTON_STREAM_LINK_SPI5)
                                      ? LEPTON_STREAM_LINK_UART4
                                      : LEPTON_STREAM_LINK_SPI5);
            return;
        }
        if (b == 'U')
        {
            Lepton_Stream_RequestLink(LEPTON_STREAM_LINK_UART4);
            return;
        }
        if (b == 'W')
        {
            Lepton_Stream_RequestLink(LEPTON_STREAM_LINK_SPI5);
            return;
        }
        if (b == 'B')
        {
            esp32_io0_manual = 1U;
            HAL_GPIO_WritePin(ESP_IO0_GPIO_Port, ESP_IO0_Pin, GPIO_PIN_RESET);
            return;
        }
        if (b == 'R')
        {
            esp32_io0_manual = 1U;
            HAL_GPIO_WritePin(ESP_IO0_GPIO_Port, ESP_IO0_Pin, GPIO_PIN_SET);
            return;
        }
    }

    if ((b == '\r') || (b == '\n'))
    {
        if (stream_cmd_len > 0U)
        {
            stream_cmd_buf[stream_cmd_len] = '\0';
            Lepton_Stream_HandleCommandLine(stream_cmd_buf);
            stream_cmd_len = 0U;
        }
        return;
    }

    if (stream_cmd_len < (uint8_t)(sizeof(stream_cmd_buf) - 1U))
    {
        if ((b >= 'a') && (b <= 'z'))
            b = (uint8_t)(b - 'a' + 'A');
        stream_cmd_buf[stream_cmd_len++] = (char)b;
    }
    else
    {
        stream_cmd_len = 0U;
    }
}

static void Lepton_Stream_UART4_GPIO_Config(void)
{
    GPIO_InitTypeDef gi = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    gi.Pin       = GPIO_PIN_0 | GPIO_PIN_1;
    gi.Mode      = GPIO_MODE_AF_PP;
    gi.Pull      = GPIO_NOPULL;
    gi.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gi.Alternate = GPIO_AF8_UART4;
    HAL_GPIO_Init(GPIOA, &gi);
}

static void Lepton_Stream_UART4_DMA_Config(UART_HandleTypeDef *huart)
{
    __HAL_RCC_DMA1_CLK_ENABLE();

    hdma_uart4_tx.Instance                 = DMA1_Stream1;
    hdma_uart4_tx.Init.Request             = DMA_REQUEST_UART4_TX;
    hdma_uart4_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    hdma_uart4_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_uart4_tx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_uart4_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_uart4_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    hdma_uart4_tx.Init.Mode                = DMA_NORMAL;
    hdma_uart4_tx.Init.Priority            = DMA_PRIORITY_LOW;
    hdma_uart4_tx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_uart4_tx) != HAL_OK)
        Error_Handler();

    __HAL_LINKDMA(huart, hdmatx, hdma_uart4_tx);

    HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream1_IRQn);
}

static void Lepton_Stream_CleanDCache(uint8_t *buf, uint32_t len)
{
#if (__DCACHE_PRESENT == 1U)
    if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U)
    {
        uint32_t start = ((uint32_t)buf) & ~31UL;
        uint32_t end = (((uint32_t)buf) + len + 31UL) & ~31UL;

        SCB_CleanDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
    }
#else
    (void)buf;
    (void)len;
#endif
}

/* README_9 Step9A verdict: a blocking USART1 print right after starting the
   UART4 TX DMA corrupted every 16th frame (checksum bytes read as 0x0000).
   So this prints ONLY on a failed DMA start - never on the success path. */
static void Lepton_Stream_DebugPrint(uint16_t fid, uint16_t sum,
                                     HAL_StatusTypeDef status)
{
    char msg[220];
    int n;

    if (status == HAL_OK)
        return;

    n = snprintf(msg, sizeof(msg),
                 "[STRM] fid=%u len=%lu sum=%u st=%d ok=%lu fail=%lu cplt=%lu drop=%lu dmaErr=%lu busy=%u txi=%u S=%lu P=%lu uartErr=%lu\r\n",
                 (unsigned)fid,
                 (unsigned long)LEPTON_STREAM_FRAME_LEN,
                 (unsigned)sum,
                 (int)status,
                 (unsigned long)stream_tx_ok_count,
                 (unsigned long)stream_tx_fail_count,
                 (unsigned long)stream_dma_cplt_count,
                 (unsigned long)stream_dma_busy_drop_count,
                 (unsigned long)stream_dma_error_count,
                 (unsigned)stream_tx_busy,
                 (unsigned)stream_tx_index,
                 (unsigned long)stream_cmd_s_count,
                 (unsigned long)stream_cmd_p_count,
                 (unsigned long)stream_uart_error_count);
    if (n > 0)
        (void)HAL_UART_Transmit(&huart1, (uint8_t *)msg, (uint16_t)n, 30);
}

/* ---------------------------------------------------------------------------
 * Re-init the CH340C UART for streaming and arm the 1-byte command receiver.
 * Runtime re-config on top of MX_UART4_Init (same pattern as Lepton_SPI_Config)
 * so a CubeMX regen cannot silently undo baud rate or pin swap.
 * ------------------------------------------------------------------------- */
void Lepton_Stream_Init(UART_HandleTypeDef *huart)
{
    stream_huart = huart;

    huart->Init.BaudRate               = LEPTON_STREAM_BAUD;
    huart->AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_SWAP_INIT;
    huart->AdvancedInit.Swap           = UART_ADVFEATURE_SWAP_ENABLE;
    if (HAL_UART_Init(huart) != HAL_OK)
        Error_Handler();

    if (huart->Instance == UART4)
    {
        Lepton_Stream_UART4_GPIO_Config();
        Lepton_Stream_UART4_DMA_Config(huart);
        HAL_NVIC_SetPriority(UART4_IRQn, 5, 0);
        HAL_NVIC_EnableIRQ(UART4_IRQn);
    }

    HAL_UART_Receive_IT(huart, (uint8_t *)&stream_rx_byte, 1);

    /* Both transports live side by side; stream_link picks at runtime. */
    Lepton_Stream_SPI5_Config();

    /* PG2 <- ESP32 IO38 = bridge-ready line ("1-wire" status): the sketch
       drives it high once booted. MX_GPIO made PG2 an output (low) - flip
       it to input w/ pulldown so both sides can't fight and an absent or
       un-flashed ESP32 reads as "not ready". */
    {
        GPIO_InitTypeDef gi = {0};
        __HAL_RCC_GPIOG_CLK_ENABLE();
        gi.Pin  = ESP_IO38_Pin;
        gi.Mode = GPIO_MODE_INPUT;
        gi.Pull = GPIO_PULLDOWN;
        HAL_GPIO_Init(ESP_IO38_GPIO_Port, &gi);
    }

    /* ESP32 IO0 (PJ7), user-mandated sequence: POWER-ON = LOW for the first
       30 s (MX_GPIO_Init already drives it low -> ESP32 waits in download
       mode, flash window), then Lepton_Stream_Poll() releases it for good.
       Reconfigure to OPEN-DRAIN so "high" merely releases the line (ESP32
       pull-up makes it high) and the Arduino auto-download circuit can
       always yank it low - a push-pull HIGH is what blocked esptool. */
    {
        GPIO_InitTypeDef gi = {0};
        HAL_GPIO_WritePin(ESP_IO0_GPIO_Port, ESP_IO0_Pin, GPIO_PIN_RESET);
        gi.Pin   = ESP_IO0_Pin;
        gi.Mode  = GPIO_MODE_OUTPUT_OD;
        gi.Pull  = GPIO_NOPULL;
        gi.Speed = GPIO_SPEED_FREQ_LOW;
        HAL_GPIO_Init(ESP_IO0_GPIO_Port, &gi);
    }
}

uint8_t Lepton_Stream_Active(void)
{
    return stream_active;
}

/* Execute a pending transport switch from THREAD context (never in the RX
   interrupt). Switching waits for the current complete frame to finish; no
   in-flight DMA is aborted, so UART/TCP mode changes cannot cut a half-frame. */
static void Lepton_Stream_ApplyLinkSwitch(void)
{
    uint8_t target = stream_link_pending;

    if ((target == 0xFFU) || (target == stream_link))
    {
        stream_link_pending = 0xFFU;
        return;
    }

    if (stream_tx_busy != 0U)
        return;

    if (stream_link == LEPTON_STREAM_LINK_SPI5)
        LEPTON_STREAM_DRDY_LOW();

    stream_tx_busy = 0U;
    stream_tx_index = 0xFFU;
    stream_link = target;
    stream_link_pending = 0xFFU;
    stream_ok_pending = 1U;

    Lepton_Stream_Usart1Print("change over\r\n");
}

/* Called every super-loop iteration. Four cooperative duties, all cheap:
   1) ESP32 IO0 (PJ7) timed release: LOW from power-on (flash window) until
      LEPTON_STREAM_ESP32_BOOT_LOW_MS, then released permanently.
   2) Pending transport switch requested by a UART4 command.
   3) ESP32 bridge-ready line (PG2): rising edge = the sketch just booted ->
      auto-switch the video to WiFi/TCP, per system requirement.
   4) "ok" on USART1 after the first complete frame is delivered in the current
      mode - printed only when no TX DMA is in flight (README_9 hot-path rule). */
void Lepton_Stream_Poll(void)
{
    uint8_t ready;
    uint32_t now = HAL_GetTick();

    if ((esp32_io0_manual == 0U) && (esp32_io0_released == 0U) &&
        (now >= LEPTON_STREAM_ESP32_BOOT_LOW_MS))
    {
        HAL_GPIO_WritePin(ESP_IO0_GPIO_Port, ESP_IO0_Pin, GPIO_PIN_SET);
        esp32_io0_released = 1U;
    }

    if (stream_link_pending != 0xFFU)
        Lepton_Stream_ApplyLinkSwitch();

    ready = (HAL_GPIO_ReadPin(ESP_IO38_GPIO_Port, ESP_IO38_Pin) == GPIO_PIN_SET) ? 1U : 0U;
#if LEPTON_STREAM_AUTO_TCP
    if ((ready != 0U) && (esp32_ready_last == 0U) &&
        (stream_link != LEPTON_STREAM_LINK_SPI5))
    {
        stream_link_pending = LEPTON_STREAM_LINK_SPI5;
        Lepton_Stream_ApplyLinkSwitch();
    }
#endif
    esp32_ready_last = ready;

    if ((stream_ok_pending != 0U) && (stream_tx_busy == 0U))
    {
        uint32_t cplt = stream_dma_cplt_count;
        if (cplt != ok_last_cplt)
        {
            Lepton_Stream_Usart1Print("ok\r\n");
            ok_last_cplt = cplt;
            stream_ok_pending = 0U;
        }
    }
}

uint16_t Lepton_Stream_FrameCount(void)
{
    return stream_frame_id;
}

/* ---------------------------------------------------------------------------
 * Pack lepton_raw_frame into the host frame format and start UART4 TX DMA.
 * If the previous RAW16 frame is still on the wire, drop this frame instead of
 * blocking the VoSPI acquisition loop (~256 ms per frame at 1.5 Mbps).
 * ------------------------------------------------------------------------- */
HAL_StatusTypeDef Lepton_Stream_SendFrame(void)
{
    uint8_t        *buf;
    uint8_t        *p;
    const uint16_t *src = &lepton_raw_frame[0][0];
    uint32_t        i;
    uint16_t        fid = stream_frame_id;
    uint16_t        sum = 0;
    HAL_StatusTypeDef status;

    if (stream_huart == NULL)
        return HAL_ERROR;

    /* SPI5 slave: master stalled mid-frame (ESP32 reboot / WiFi hang) -
       abort so the pipeline self-heals instead of staying busy forever. */
    if ((stream_link == LEPTON_STREAM_LINK_SPI5) && (stream_tx_busy != 0U) &&
        ((HAL_GetTick() - stream_busy_since) > LEPTON_STREAM_SPI_TIMEOUT_MS))
    {
        (void)HAL_SPI_Abort(&hspi5);
        LEPTON_STREAM_DRDY_LOW();
        stream_tx_busy = 0U;
        stream_tx_index = 0xFFU;
        stream_spi_timeout_count++;
    }

    if (stream_tx_busy != 0U)
    {
        stream_tx_fail_count++;
        stream_dma_busy_drop_count++;
        /* UART4 DMA is active here; do not block on USART1 diagnostics. */
        return HAL_BUSY;
    }

    buf = stream_buf[stream_build_index];
    p = buf;

    *p++ = 0xAA;
    *p++ = 0x55;
    *p++ = 0x01;                                   /* type: raw 16-bit thermal */
    *p++ = (uint8_t)(fid >> 8);
    *p++ = (uint8_t)(fid & 0xFFU);
    *p++ = 0x00;
    *p++ = (uint8_t)LEPTON_IMG_WIDTH;              /* 160 */
    *p++ = 0x00;
    *p++ = (uint8_t)LEPTON_IMG_HEIGHT;             /* 120 */
    *p++ = (uint8_t)(LEPTON_STREAM_PAYLOAD >> 24);
    *p++ = (uint8_t)(LEPTON_STREAM_PAYLOAD >> 16);
    *p++ = (uint8_t)(LEPTON_STREAM_PAYLOAD >> 8);
    *p++ = (uint8_t)(LEPTON_STREAM_PAYLOAD & 0xFFU);

    /* Pixels big-endian on the wire; lepton_raw_frame holds native-LE values
       (VoSPI bytes already re-assembled in Lepton_VoSPI_CommitSegment). */
    for (i = 0; i < (uint32_t)LEPTON_IMG_WIDTH * LEPTON_IMG_HEIGHT; i++)
    {
        uint16_t v = src[i];
        *p++ = (uint8_t)(v >> 8);
        *p++ = (uint8_t)(v & 0xFFU);
    }

    /* Byte-wise sum over sync + header + payload, appended big-endian. */
    for (i = 0; i < (LEPTON_STREAM_HDR_LEN + LEPTON_STREAM_PAYLOAD); i++)
        sum += buf[i];
    *p++ = (uint8_t)(sum >> 8);
    *p   = (uint8_t)(sum & 0xFFU);

    Lepton_Stream_CleanDCache(buf, LEPTON_STREAM_FRAME_LEN);

    stream_tx_busy = 1U;
    stream_tx_index = stream_build_index;
    if (stream_link == LEPTON_STREAM_LINK_SPI5)
    {
        status = HAL_SPI_Transmit_DMA(&hspi5, buf, (uint16_t)LEPTON_STREAM_FRAME_LEN);
        if (status == HAL_OK)
        {
            stream_busy_since = HAL_GetTick();
            LEPTON_STREAM_DRDY_HIGH();      /* tell ESP32: frame armed, clock it out */
        }
    }
    else
    {
        status = HAL_UART_Transmit_DMA(stream_huart, buf,
                                       (uint16_t)LEPTON_STREAM_FRAME_LEN);
    }
    if (status == HAL_OK)
    {
        stream_frame_id++;
        stream_build_index ^= 1U;
        stream_tx_ok_count++;
    }
    else
    {
        if (stream_link == LEPTON_STREAM_LINK_SPI5)
            LEPTON_STREAM_DRDY_LOW();
        stream_tx_busy = 0U;
        stream_tx_index = 0xFFU;
        stream_tx_fail_count++;
        if (status == HAL_ERROR)
            stream_dma_error_count++;
        Lepton_Stream_DebugPrint(fid, sum, status);
    }

    return status;
}

/* ---------------------------------------------------------------------------
 * HAL callbacks (sole definitions project-wide).
 * ------------------------------------------------------------------------- */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if ((stream_huart != NULL) && (huart->Instance == stream_huart->Instance))
    {
        Lepton_Stream_HandleCommandByte(stream_rx_byte);
        HAL_UART_Receive_IT(stream_huart, (uint8_t *)&stream_rx_byte, 1);
    }
}

/* Frame fully clocked out by the ESP32: drop DRDY, release the ping-pong.
   Zero prints here - same hot-path rule as the UART DMA (README_9 Step9A). */
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI5)
    {
        LEPTON_STREAM_DRDY_LOW();
        stream_tx_busy = 0U;
        stream_tx_index = 0xFFU;
        stream_dma_cplt_count++;
    }
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI5)
    {
        LEPTON_STREAM_DRDY_LOW();
        stream_tx_busy = 0U;
        stream_tx_index = 0xFFU;
        stream_dma_error_count++;
    }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if ((stream_huart != NULL) && (huart->Instance == stream_huart->Instance))
    {
        stream_tx_busy = 0U;
        stream_tx_index = 0xFFU;
        stream_dma_cplt_count++;
    }
}

/* Noise at high baud can raise ORE/FE and kill the RX interrupt chain; re-arm
   it so 'S'/'P' keep working no matter what hit the line. If the error also
   aborted an in-flight TX DMA (gState back to READY), release the ping-pong. */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if ((stream_huart != NULL) && (huart->Instance == stream_huart->Instance))
    {
        stream_uart_error_count++;
        if (huart->gState == HAL_UART_STATE_READY)
        {
            stream_tx_busy = 0U;
            stream_tx_index = 0xFFU;
            stream_dma_error_count++;
        }
        __HAL_UART_CLEAR_FLAG(huart, UART_CLEAR_OREF | UART_CLEAR_FEF |
                                     UART_CLEAR_NEF  | UART_CLEAR_PEF);
        HAL_UART_Receive_IT(stream_huart, (uint8_t *)&stream_rx_byte, 1);
    }
}

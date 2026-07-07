#include "lepton_stream.h"
#include "usart.h"
#include <stdio.h>

#define LEPTON_STREAM_BUF_COUNT 2U
#define LEPTON_STREAM_BUF_STRIDE (((LEPTON_STREAM_FRAME_LEN + 31U) / 32U) * 32U)
#define LEPTON_STREAM_FLAT_SPAN_DROP 16U

extern void SD_UART_Print(const char *s);
extern void SD_UART_TxCpltCallback(UART_HandleTypeDef *huart);
extern void SD_UART_ErrorCallback(UART_HandleTypeDef *huart);

/* stream_active: 0 = UART4 idle for host commands, 1 = binary frames are sent.
   Debug logs are on USART1; UART4/CH340 stays binary-clean for the host. */
DMA_HandleTypeDef           hdma_uart4_tx;
static volatile uint8_t     stream_active = 0;
static volatile uint8_t     stream_rx_byte = 0;
static UART_HandleTypeDef  *stream_huart = NULL;
static uint16_t             stream_frame_id = 0;
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
static volatile uint16_t    stream_raw_min = 0U;
static volatile uint16_t    stream_raw_max = 0U;
static volatile uint16_t    stream_raw_span = 0U;
static volatile uint32_t    stream_raw_flat_drop_count = 0U;
static volatile uint32_t    stream_raw_frame_count = 0U;

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
        SD_UART_Print(msg);
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
}

uint8_t Lepton_Stream_Active(void)
{
    return stream_active;
}

uint16_t Lepton_Stream_FrameCount(void)
{
    return stream_frame_id;
}

void Lepton_Stream_GetContentDiag(uint16_t *raw_min, uint16_t *raw_max,
                                  uint16_t *raw_span, uint32_t *flat_drop,
                                  uint32_t *flat_total)
{
    if (raw_min != NULL)
        *raw_min = stream_raw_min;
    if (raw_max != NULL)
        *raw_max = stream_raw_max;
    if (raw_span != NULL)
        *raw_span = stream_raw_span;
    if (flat_drop != NULL)
        *flat_drop = stream_raw_flat_drop_count;
    if (flat_total != NULL)
        *flat_total = stream_raw_frame_count;
}

/* ---------------------------------------------------------------------------
 * Pack lepton_raw_frame into the host frame format and start UART4 TX DMA.
 * If the previous RAW16 frame is still on the wire, drop this frame instead of
 * blocking the VoSPI acquisition loop.
 * ------------------------------------------------------------------------- */
HAL_StatusTypeDef Lepton_Stream_SendFrame(void)
{
    uint8_t        *buf;
    uint8_t        *p;
    const uint16_t *src = &lepton_raw_frame[0][0];
    uint32_t        i;
    uint16_t        fid = stream_frame_id;
    uint16_t        sum = 0;
    uint16_t        raw_min = 0xFFFFU;
    uint16_t        raw_max = 0U;
    uint16_t        raw_span;
    HAL_StatusTypeDef status;

    if (stream_huart == NULL)
        return HAL_ERROR;

    if (stream_tx_busy != 0U)
    {
        stream_tx_fail_count++;
        stream_dma_busy_drop_count++;
        /* UART4 DMA is active here; do not block on USART1 diagnostics. */
        return HAL_BUSY;
    }

    for (i = 0; i < (uint32_t)LEPTON_IMG_WIDTH * LEPTON_IMG_HEIGHT; i++)
    {
        uint16_t v = src[i];

        if (v < raw_min)
            raw_min = v;
        if (v > raw_max)
            raw_max = v;
    }
    raw_span = (uint16_t)(raw_max - raw_min);
    stream_raw_min = raw_min;
    stream_raw_max = raw_max;
    stream_raw_span = raw_span;
    stream_raw_frame_count++;

    if (raw_span < LEPTON_STREAM_FLAT_SPAN_DROP)
    {
        stream_raw_flat_drop_count++;
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
    status = HAL_UART_Transmit_DMA(stream_huart, buf,
                                   (uint16_t)LEPTON_STREAM_FRAME_LEN);
    if (status == HAL_OK)
    {
        stream_frame_id++;
        stream_build_index ^= 1U;
        stream_tx_ok_count++;
    }
    else
    {
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
        if (stream_rx_byte == 'S')
        {
            stream_active = 1;
            stream_cmd_s_count++;
        }
        else if (stream_rx_byte == 'P')
        {
            stream_active = 0;
            stream_cmd_p_count++;
        }
        HAL_UART_Receive_IT(stream_huart, (uint8_t *)&stream_rx_byte, 1);
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
    else if (huart->Instance == USART1)
    {
        SD_UART_TxCpltCallback(huart);
    }
}

/* Noise at 2Mbps can raise ORE/FE and kill the RX interrupt chain; re-arm it
   so 'S'/'P' keep working no matter what hit the line. */
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
    else if (huart->Instance == USART1)
    {
        SD_UART_ErrorCallback(huart);
    }
}

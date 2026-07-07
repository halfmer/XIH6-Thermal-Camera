#include "lepton_stream.h"
#include "usart.h"
#include <stdio.h>

/* stream_active: 0 = UART4 idle for host commands, 1 = binary frames are sent.
   Debug logs are on USART1; UART4/CH340 stays binary-clean for the host. */
static volatile uint8_t     stream_active = 0;
static volatile uint8_t     stream_rx_byte = 0;
static UART_HandleTypeDef  *stream_huart = NULL;
static uint16_t             stream_frame_id = 0;
static uint8_t              stream_buf[LEPTON_STREAM_FRAME_LEN];
static volatile uint32_t    stream_tx_ok_count = 0;
static volatile uint32_t    stream_tx_fail_count = 0;
static volatile uint32_t    stream_cmd_s_count = 0;
static volatile uint32_t    stream_cmd_p_count = 0;
static volatile uint32_t    stream_uart_error_count = 0;

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

static void Lepton_Stream_DebugPrint(uint16_t fid, uint16_t sum,
                                     HAL_StatusTypeDef status)
{
    char msg[160];
    int n;
    uint32_t total = stream_tx_ok_count + stream_tx_fail_count;

    if ((status == HAL_OK) && ((total & 0x0FU) != 0U))
        return;

    n = snprintf(msg, sizeof(msg),
                 "[STRM] fid=%u len=%lu sum=%u st=%d ok=%lu fail=%lu S=%lu P=%lu uartErr=%lu\r\n",
                 (unsigned)fid,
                 (unsigned long)LEPTON_STREAM_FRAME_LEN,
                 (unsigned)sum,
                 (int)status,
                 (unsigned long)stream_tx_ok_count,
                 (unsigned long)stream_tx_fail_count,
                 (unsigned long)stream_cmd_s_count,
                 (unsigned long)stream_cmd_p_count,
                 (unsigned long)stream_uart_error_count);
    if (n > 0)
        (void)HAL_UART_Transmit(&huart1, (uint8_t *)msg, (uint16_t)n, 10);
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

/* ---------------------------------------------------------------------------
 * Pack lepton_raw_frame into the host frame format and push it out (blocking).
 * ~256 ms per frame at 1.5 Mbps; VoSPI frames missed meanwhile are recovered by
 * the normal resync path on the next Lepton_Capture_Frame().
 * ------------------------------------------------------------------------- */
HAL_StatusTypeDef Lepton_Stream_SendFrame(void)
{
    uint8_t        *p   = stream_buf;
    const uint16_t *src = &lepton_raw_frame[0][0];
    uint32_t        i;
    uint16_t        fid = stream_frame_id;
    uint16_t        sum = 0;
    HAL_StatusTypeDef status;

    if (stream_huart == NULL)
        return HAL_ERROR;

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
        sum += stream_buf[i];
    *p++ = (uint8_t)(sum >> 8);
    *p   = (uint8_t)(sum & 0xFFU);

    stream_frame_id++;

    status = HAL_UART_Transmit(stream_huart, stream_buf,
                               (uint16_t)LEPTON_STREAM_FRAME_LEN, 400);
    if (status == HAL_OK)
        stream_tx_ok_count++;
    else
        stream_tx_fail_count++;

    Lepton_Stream_DebugPrint(fid, sum, status);
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

/* Noise at 2Mbps can raise ORE/FE and kill the RX interrupt chain; re-arm it
   so 'S'/'P' keep working no matter what hit the line. */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if ((stream_huart != NULL) && (huart->Instance == stream_huart->Instance))
    {
        stream_uart_error_count++;
        __HAL_UART_CLEAR_FLAG(huart, UART_CLEAR_OREF | UART_CLEAR_FEF |
                                     UART_CLEAR_NEF  | UART_CLEAR_PEF);
        HAL_UART_Receive_IT(stream_huart, (uint8_t *)&stream_rx_byte, 1);
    }
}

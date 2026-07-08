#include "lepton.h"
#include "DELAY.h"          /* delay_us for the software bit-bang I2C timing */
#include <string.h>

/* driver handles (bound in Lepton_Init) */
static SPI_HandleTypeDef *lepton_hspi = NULL;
static I2C_HandleTypeDef *lepton_hi2c = NULL;
static uint8_t lepton_cci_use_bitbang = 0U;   /* 0=HAL I2C4, 1=software CCI */

static void Lepton_I2C4_ResetAndReinit(void);
static HAL_StatusTypeDef Lepton_BB_Write_Mem(uint16_t reg, const uint8_t *buf, uint16_t len);
static HAL_StatusTypeDef Lepton_BB_Read_Mem(uint16_t reg, uint8_t *buf, uint16_t len);
static void Lepton_I2C_BusRecover(void);

#define VOSPI_PKT_VALID    0U
#define VOSPI_PKT_DISCARD  1U
#define VOSPI_PKT_INVALID  2U

#define LEPTON_VOSPI_PACKET_GUARD          3000U
#define LEPTON_VOSPI_FIRST_DISCARD_WAIT_MS 1U
#define LEPTON_VOSPI_RETRY_WAIT_MS         1U

/* 1 = publish only when all four shelf segments were committed in the same
 * acquisition generation (segment-number wrap = new generation). Blocks the
 * time-skewed stitches the persistent shelf otherwise ships whenever a segment
 * was dropped mid-round: checksum-valid frames whose 30-row bands come from
 * different Lepton frames. Those were invisible on a static scene but showed
 * as hard seams on motion, and the Qt tear gate then rejected nearly every
 * frame (2 fps -> ~0.3 fps on motion). 0 = legacy publish-on-seg4 behavior. */
#define LEPTON_VOSPI_FRESH_PUBLISH         1U

/* Mid-segment discard packets usually mean we out-ran the Lepton's packet
 * generator (~440us/packet at 8.7fps), NOT lost sync: re-read the same
 * expected packet up to this many times (1ms apart) before declaring a real
 * desync. Raises the per-segment success rate the fresh gate depends on. */
#define LEPTON_VOSPI_INTRA_DISCARD_MAX     8U

/* Fresh-gate starvation guard: after this many same-generation rejections in
 * ONE capture call, publish the stitched shelf anyway (legacy behavior). A
 * time-skewed frame beats no frame; the Qt tear gate still filters it. */
#define LEPTON_VOSPI_STALE_FORCE_PUBLISH   4U

/* global image buffer + scratch packet */
uint16_t lepton_raw_frame[LEPTON_IMG_HEIGHT][LEPTON_IMG_WIDTH] = {0};
uint8_t  lepton_spi_pkt[LEPTON_PACK_SIZE] = {0};

/* Complete-frame staging buffer. lepton_raw_frame is only updated after a fresh
   1->2->3->4 segment sequence is complete, so the streamer never publishes a
   half-new/half-stale image. */
static uint16_t lepton_assembly_frame[LEPTON_IMG_HEIGHT][LEPTON_IMG_WIDTH] = {0};

/* one segment's payload (60 packets * 160 bytes), staged before commit */
static uint8_t seg_payload[LEPTON_PKT_PER_SEG][160];
static uint8_t vospi_cached_mask = 0U;  /* bit0..3: segments already committed to the assembly frame */

/* Acquisition-generation tracking for the fresh-publish gate. The generation
   bumps whenever the committed segment number wraps (seg <= previous commit),
   i.e. the stream moved on to a new Lepton frame, and on every VoSPI resync. */
static uint8_t vospi_seg_gen[LEPTON_SEG_CNT + 1U] = {0};
static uint8_t vospi_gen = 0U;
static uint8_t vospi_last_commit_seg = 0U;
/* Starvation guard for the fresh-publish gate: counts consecutive stale
   blocks ACROSS captures (lepton_diag.vospi_stale_block is diag-reset every
   capture, and seg-4 completes at most ~once per capture, so a per-capture
   counter can never reach the force-publish threshold). */
static uint16_t vospi_stale_streak = 0U;

/* bring-up diagnostics (printed by main) */
Lepton_Diag_t lepton_diag = {0};

static void Lepton_VoSPI_DiagReset(void)
{
    lepton_diag.vospi_reads       = 0;
    lepton_diag.vospi_valid       = 0;
    lepton_diag.vospi_discard     = 0;
    lepton_diag.vospi_invalid     = 0;
    lepton_diag.vospi_pkt0        = 0;
    lepton_diag.vospi_desync      = 0;
    lepton_diag.vospi_bad_seg     = 0;
    lepton_diag.vospi_spi_err     = 0;
    lepton_diag.vospi_fail_reason = LEPTON_VOSPI_FAIL_NONE;
    lepton_diag.vospi_last_expected = 0xFF;
    lepton_diag.vospi_last_id0    = 0;
    lepton_diag.vospi_last_id1    = 0;
    lepton_diag.vospi_last_crc0   = 0;
    lepton_diag.vospi_last_crc1   = 0;
    lepton_diag.vospi_last_seg    = 0xFF;
    lepton_diag.vospi_got_mask    = vospi_cached_mask;
    lepton_diag.vospi_dup_seg     = 0;
    lepton_diag.vospi_seg_bad0    = 0;
    lepton_diag.vospi_seg_badx    = 0;
    lepton_diag.vospi_sync_waits  = 0;
    lepton_diag.vospi_stale_block = 0;
    for (uint8_t i = 0; i <= LEPTON_SEG_CNT; i++)
    {
        lepton_diag.vospi_seg_seen[i] = 0;
        lepton_diag.vospi_seg_ok[i] = 0;
    }
}

static uint8_t Lepton_VoSPI_DiagPacket(void)
{
    uint8_t id0_low;

    lepton_diag.vospi_reads++;
    lepton_diag.vospi_last_id0  = lepton_spi_pkt[0];
    lepton_diag.vospi_last_id1  = lepton_spi_pkt[1];
    lepton_diag.vospi_last_crc0 = lepton_spi_pkt[2];
    lepton_diag.vospi_last_crc1 = lepton_spi_pkt[3];

    id0_low = (uint8_t)(lepton_spi_pkt[0] & 0x0FU);
    if (id0_low == 0x0FU)
    {
        lepton_diag.vospi_discard++;
        return VOSPI_PKT_DISCARD;
    }

    if ((id0_low != 0U) || (lepton_spi_pkt[1] >= LEPTON_PKT_PER_SEG))
    {
        lepton_diag.vospi_invalid++;
        return VOSPI_PKT_INVALID;
    }

    lepton_diag.vospi_valid++;
    return VOSPI_PKT_VALID;
}

/* ===========================================================================
 * Low-level pin / clock / bus setup
 * ======================================================================== */

/* ---------------------------------------------------------------------------
 * PE4 is wired to the Lepton CS. CubeMX configures it as SPI4_NSS (alt-func);
 * reconfigure it as a plain push-pull GPIO so CS is driven in software
 * (VoSPI needs CS held low for a whole 164-byte packet).
 * ------------------------------------------------------------------------- */
static void Lepton_CS_GPIO_Config(void)
{
    GPIO_InitTypeDef gi = {0};
    __HAL_RCC_GPIOE_CLK_ENABLE();
    gi.Pin   = LEPTON_CS_GPIO_PIN;
    gi.Mode  = GPIO_MODE_OUTPUT_PP;
    gi.Pull  = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(LEPTON_CS_GPIO_PORT, &gi);
    LEPTON_CS_HIGH();
}

/* SPI4 pins were generated by CubeMX with GPIO_SPEED_FREQ_LOW. That is marginal
   for VoSPI, even at conservative SPI clocks. Keep the same AF mapping but drive
   the pads at very-high speed so edge shape is no longer the first suspect. */
static void Lepton_SPI_GPIO_Config(void)
{
    GPIO_InitTypeDef gi = {0};

    __HAL_RCC_GPIOE_CLK_ENABLE();
    gi.Pin       = GPIO_PIN_2 | GPIO_PIN_5 | GPIO_PIN_6; /* SCK, MISO, MOSI */
    gi.Mode      = GPIO_MODE_AF_PP;
    gi.Pull      = GPIO_NOPULL;
    gi.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gi.Alternate = GPIO_AF5_SPI4;
    HAL_GPIO_Init(GPIOE, &gi);
}

/* ---------------------------------------------------------------------------
 * RESET_L (PJ3) and PWR_DWN_L (PH6) are active-low. CubeMX already inits them
 * as push-pull outputs but drives them LOW (= reset asserted, powered down).
 * We keep them as outputs and take control here. VSYNC (PB2) is a Lepton
 * output, so switch it to input to avoid drive contention.
 * ------------------------------------------------------------------------- */
static void Lepton_CtrlPins_Config(void)
{
    GPIO_InitTypeDef gi = {0};

    __HAL_RCC_GPIOJ_CLK_ENABLE();   /* RESET_L */
    __HAL_RCC_GPIOH_CLK_ENABLE();   /* PWR_DWN_L */
    __HAL_RCC_GPIOB_CLK_ENABLE();   /* VSYNC */

    /* start from a known "off" state: both active-low lines asserted (LOW) */
    HAL_GPIO_WritePin(LEPTON_RST_GPIO_Port,  LEPTON_RST_Pin,  GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LEPTON_PWDN_GPIO_Port, LEPTON_PWDN_Pin, GPIO_PIN_RESET);

    gi.Mode  = GPIO_MODE_OUTPUT_PP;
    gi.Pull  = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_LOW;
    gi.Pin   = LEPTON_RST_Pin;
    HAL_GPIO_Init(LEPTON_RST_GPIO_Port, &gi);
    gi.Pin   = LEPTON_PWDN_Pin;
    HAL_GPIO_Init(LEPTON_PWDN_GPIO_Port, &gi);

    /* VSYNC as input (Lepton drives it when GPIO3=VSYNC is enabled via CCI) */
    gi.Mode  = GPIO_MODE_INPUT;
    gi.Pull  = GPIO_NOPULL;
    gi.Pin   = LEPTON_VSYNC_Pin;
    HAL_GPIO_Init(LEPTON_VSYNC_GPIO_Port, &gi);
}

/* ---------------------------------------------------------------------------
 * Reconfigure SPI4 for Lepton VoSPI, overriding CubeMX's MX_SPI4_Init():
 *   SPI mode 3 (CPOL=1, CPHA=1), 8-bit, MSB first, software NSS, ~7.5MHz.
 *   SPI4 kernel is D2PCLK1/APB1 = 120MHz; /16 = 7.5MHz, below Lepton's
 *   20MHz max and easier on jumper-wire bring-up.
 * ------------------------------------------------------------------------- */
static void Lepton_SPI_Config(SPI_HandleTypeDef *hspi)
{
    hspi->Instance               = SPI4;
    hspi->Init.Mode              = SPI_MODE_MASTER;
    hspi->Init.Direction         = SPI_DIRECTION_2LINES;     /* RX used; MOSI idle */
    hspi->Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi->Init.CLKPolarity       = SPI_POLARITY_HIGH;        /* CPOL = 1 */
    hspi->Init.CLKPhase          = SPI_PHASE_2EDGE;          /* CPHA = 1 -> mode 3 */
    hspi->Init.NSS               = SPI_NSS_SOFT;             /* CS handled in SW */
    hspi->Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16; /* 120MHz/16 = 7.5MHz */
    hspi->Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi->Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi->Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi->Init.CRCPolynomial     = 0x0;
    hspi->Init.NSSPMode          = SPI_NSS_PULSE_DISABLE;
    hspi->Init.NSSPolarity       = SPI_NSS_POLARITY_LOW;
    hspi->Init.FifoThreshold     = SPI_FIFO_THRESHOLD_01DATA;
    hspi->Init.TxCRCInitializationPattern = SPI_CRC_INITIALIZATION_ALL_ZERO_PATTERN;
    hspi->Init.RxCRCInitializationPattern = SPI_CRC_INITIALIZATION_ALL_ZERO_PATTERN;
    hspi->Init.MasterSSIdleness          = SPI_MASTER_SS_IDLENESS_00CYCLE;
    hspi->Init.MasterInterDataIdleness   = SPI_MASTER_INTERDATA_IDLENESS_00CYCLE;
    hspi->Init.MasterReceiverAutoSusp    = SPI_MASTER_RX_AUTOSUSP_DISABLE;
    hspi->Init.MasterKeepIOState         = SPI_MASTER_KEEP_IO_STATE_ENABLE;
    hspi->Init.IOSwap                    = SPI_IO_SWAP_DISABLE;
    if (HAL_SPI_Init(hspi) != HAL_OK)
    {
        Error_Handler();
    }
    Lepton_SPI_GPIO_Config();
}

/* ---------------------------------------------------------------------------
 * Lepton 3.5 needs an external master clock (24~27MHz, nominal 25MHz) on its
 * MST_CLK pin. This project returns to the last-known intermittent-OK baseline:
 * PA8 as TIM1_CH1 PWM at 24MHz. The exact-25MHz MCO1 test was worse on hardware.
 * ------------------------------------------------------------------------- */
static void Lepton_MCLK_Config(void)
{
    GPIO_InitTypeDef gi = {0};

    /* PA8 -> TIM1_CH1 (AF1), push-pull, very-high speed */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    gi.Pin       = GPIO_PIN_8;
    gi.Mode      = GPIO_MODE_AF_PP;
    gi.Pull      = GPIO_NOPULL;
    gi.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gi.Alternate = GPIO_AF1_TIM1;
    HAL_GPIO_Init(GPIOA, &gi);

    /* TIM1 PWM: 240MHz / (ARR+1=10) = 24MHz, 50% duty on CH1 */
    __HAL_RCC_TIM1_CLK_ENABLE();
    TIM1->CR1   = 0;
    TIM1->PSC   = 0;
    TIM1->ARR   = 9;
    TIM1->CCR1  = 5;
    TIM1->CCMR1 = TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1PE;
    TIM1->CCER  = TIM_CCER_CC1E;
    TIM1->BDTR  = TIM_BDTR_MOE;
    TIM1->EGR   = TIM_EGR_UG;
    TIM1->CR1  |= TIM_CR1_CEN;

    lepton_diag.mclk_on = (TIM1->CR1 & TIM_CR1_CEN) ? 1U : 0U;
    lepton_diag.mclk_hz = 24000000U;
    lepton_diag.sysclk_hz = HAL_RCC_GetSysClockFreq();
    lepton_diag.pclk2_hz = HAL_RCC_GetPCLK2Freq();
}

/* ---------------------------------------------------------------------------
 * Lepton power-on / reset sequence (FLIR Lepton Engineering Datasheet):
 *   1. Master clock present (done before this call).
 *   2. Hold RESET_L low, PWR_DWN_L low.
 *   3. Release PWR_DWN_L (high) -> power up core.
 *   4. Wait >5000 MCLK cycles (~1ms). We use a generous 50ms.
 *   5. Release RESET_L (high).
 *   6. Wait ~950ms for the camera to boot, then CCI is ready.
 * ------------------------------------------------------------------------- */
static void Lepton_PowerOn_Sequence(void)
{
    /* known-off state */
    HAL_GPIO_WritePin(LEPTON_RST_GPIO_Port,  LEPTON_RST_Pin,  GPIO_PIN_RESET); /* in reset */
    HAL_GPIO_WritePin(LEPTON_PWDN_GPIO_Port, LEPTON_PWDN_Pin, GPIO_PIN_RESET); /* powered down */
    HAL_Delay(10);

    /* power up (release PWR_DWN_L) with clock already running */
    HAL_GPIO_WritePin(LEPTON_PWDN_GPIO_Port, LEPTON_PWDN_Pin, GPIO_PIN_SET);
    lepton_diag.powered = 1;
    HAL_Delay(50);                       /* >> 5000 MCLK cycles */

    /* release reset */
    HAL_GPIO_WritePin(LEPTON_RST_GPIO_Port, LEPTON_RST_Pin, GPIO_PIN_SET);
    lepton_diag.reset_released = 1;

    HAL_Delay(1200);                     /* datasheet boot ~950ms; +margin, and
                                            never touch CCI before this or the
                                            TWI slave can latch dead until a COLD
                                            power-cycle (survives MCU reset). */

    /* Read the actual pad levels back (IDR reflects the real pin voltage even
       for an output). If ODR was set high but IDR reads low, the net is held low
       externally (short / strong pulldown / the Lepton driving it) -> hardware. */
    lepton_diag.rst_readback  =
        (HAL_GPIO_ReadPin(LEPTON_RST_GPIO_Port,  LEPTON_RST_Pin)  == GPIO_PIN_SET) ? 1U : 0U;
    lepton_diag.pwdn_readback =
        (HAL_GPIO_ReadPin(LEPTON_PWDN_GPIO_Port, LEPTON_PWDN_Pin) == GPIO_PIN_SET) ? 1U : 0U;
}

/* ===========================================================================
 * CCI transport (raw 16-bit register read/write, big-endian)
 * ======================================================================== */

HAL_StatusTypeDef Lepton_I2C_Write_Reg(uint16_t reg, uint16_t data)
{
    uint8_t buf[2];
    buf[0] = (uint8_t)(data >> 8);
    buf[1] = (uint8_t)(data & 0xFF);

    if (lepton_cci_use_bitbang)
        return Lepton_BB_Write_Mem(reg, buf, 2);

    return HAL_I2C_Mem_Write(lepton_hi2c, (uint16_t)(LEPTON_I2C_ADDR << 1),
                             reg, I2C_MEMADD_SIZE_16BIT, buf, 2, 100);
}

HAL_StatusTypeDef Lepton_I2C_Read_Reg(uint16_t reg, uint16_t *p_data)
{
    uint8_t buf[2] = {0};
    HAL_StatusTypeDef ret;

    if (lepton_cci_use_bitbang)
    {
        ret = Lepton_BB_Read_Mem(reg, buf, 2);
    }
    else
    {
        ret = HAL_I2C_Mem_Read(lepton_hi2c, (uint16_t)(LEPTON_I2C_ADDR << 1),
                               reg, I2C_MEMADD_SIZE_16BIT, buf, 2, 100);
    }

    *p_data = (uint16_t)((buf[0] << 8) | buf[1]);
    return ret;
}

/* Write N 16-bit words to the consecutive DATA0.. registers (big-endian). */
static HAL_StatusTypeDef Lepton_I2C_Write_Data(const uint16_t *data, uint16_t nwords)
{
    uint8_t buf[32];                         /* DATA0..DATA15 = 16 words max */
    if (nwords > 16U) return HAL_ERROR;
    for (uint16_t i = 0; i < nwords; i++)
    {
        buf[i * 2]     = (uint8_t)(data[i] >> 8);
        buf[i * 2 + 1] = (uint8_t)(data[i] & 0xFF);
    }
    if (lepton_cci_use_bitbang)
        return Lepton_BB_Write_Mem(LEP_REG_DATA0, buf, (uint16_t)(nwords * 2));

    return HAL_I2C_Mem_Write(lepton_hi2c, (uint16_t)(LEPTON_I2C_ADDR << 1),
                             LEP_REG_DATA0, I2C_MEMADD_SIZE_16BIT,
                             buf, (uint16_t)(nwords * 2), 200);
}

/* Read N 16-bit words from the consecutive DATA0.. registers (big-endian). */
static HAL_StatusTypeDef Lepton_I2C_Read_Data(uint16_t *data, uint16_t nwords)
{
    uint8_t buf[32];
    if (nwords > 16U) return HAL_ERROR;
    HAL_StatusTypeDef ret;

    if (lepton_cci_use_bitbang)
    {
        ret = Lepton_BB_Read_Mem(LEP_REG_DATA0, buf, (uint16_t)(nwords * 2));
    }
    else
    {
        ret = HAL_I2C_Mem_Read(lepton_hi2c, (uint16_t)(LEPTON_I2C_ADDR << 1),
                               LEP_REG_DATA0, I2C_MEMADD_SIZE_16BIT,
                               buf, (uint16_t)(nwords * 2), 200);
    }

    for (uint16_t i = 0; i < nwords; i++)
        data[i] = (uint16_t)((buf[i * 2] << 8) | buf[i * 2 + 1]);
    return ret;
}

/* ===========================================================================
 * CCI command layer
 * ======================================================================== */

/* Poll STATUS until BUSY clears; returns the command error code (high byte). */
Lepton_Status_t Lepton_CCI_WaitBusy(uint32_t timeout_ms, int8_t *p_err)
{
    uint16_t status = 0;
    uint32_t t0 = HAL_GetTick();

    do {
        if (Lepton_I2C_Read_Reg(LEP_REG_STATUS, &status) != HAL_OK)
            return LEP_ERR_I2C;
        if ((status & LEP_STATUS_BUSY) == 0U)
        {
            if (p_err) *p_err = (int8_t)(status >> 8);   /* signed error code */
            return LEP_OK;
        }
    } while ((HAL_GetTick() - t0) < timeout_ms);

    return LEP_ERR_BUSY;
}

/* SET: load DATA regs, set DATA LENGTH, issue COMMAND, wait, check error. */
Lepton_Status_t Lepton_CCI_SetAttr(uint16_t cmd, const uint16_t *data, uint16_t nwords)
{
    int8_t err = 0;
    Lepton_Status_t st;

    if ((st = Lepton_CCI_WaitBusy(500, NULL)) != LEP_OK) return st;
    if (nwords)
    {
        if (Lepton_I2C_Write_Data(data, nwords) != HAL_OK)            return LEP_ERR_I2C;
        if (Lepton_I2C_Write_Reg(LEP_REG_DATA_LENGTH, nwords) != HAL_OK) return LEP_ERR_I2C;
    }
    if (Lepton_I2C_Write_Reg(LEP_REG_COMMAND, cmd) != HAL_OK)         return LEP_ERR_I2C;
    if ((st = Lepton_CCI_WaitBusy(500, &err)) != LEP_OK)             return st;
    return (err == 0) ? LEP_OK : LEP_ERR_CMD;
}

/* GET: set DATA LENGTH, issue COMMAND, wait, check error, read DATA regs. */
Lepton_Status_t Lepton_CCI_GetAttr(uint16_t cmd, uint16_t *data, uint16_t nwords)
{
    int8_t err = 0;
    Lepton_Status_t st;

    if ((st = Lepton_CCI_WaitBusy(500, NULL)) != LEP_OK) return st;
    if (Lepton_I2C_Write_Reg(LEP_REG_DATA_LENGTH, nwords) != HAL_OK)  return LEP_ERR_I2C;
    if (Lepton_I2C_Write_Reg(LEP_REG_COMMAND, cmd) != HAL_OK)         return LEP_ERR_I2C;
    if ((st = Lepton_CCI_WaitBusy(500, &err)) != LEP_OK)             return st;
    if (err != 0)                                                    return LEP_ERR_CMD;
    if (data && nwords)
        if (Lepton_I2C_Read_Data(data, nwords) != HAL_OK)            return LEP_ERR_I2C;
    return LEP_OK;
}

/* RUN: issue COMMAND with no data, wait, check error. */
Lepton_Status_t Lepton_CCI_RunCmd(uint16_t cmd)
{
    int8_t err = 0;
    Lepton_Status_t st;

    if ((st = Lepton_CCI_WaitBusy(500, NULL)) != LEP_OK) return st;
    if (Lepton_I2C_Write_Reg(LEP_REG_COMMAND, cmd) != HAL_OK)         return LEP_ERR_I2C;
    if ((st = Lepton_CCI_WaitBusy(1000, &err)) != LEP_OK)            return st;
    return (err == 0) ? LEP_OK : LEP_ERR_CMD;
}

/* ===========================================================================
 * High-level control
 * ======================================================================== */

/* Poll STATUS until boot-complete bit is set (and not busy). */
Lepton_Status_t Lepton_WaitBoot(uint32_t timeout_ms)
{
    uint16_t status = 0;
    uint32_t t0 = HAL_GetTick();

    do {
        if (Lepton_I2C_Read_Reg(LEP_REG_STATUS, &status) == HAL_OK)
        {
            lepton_diag.status_reg  = status;
            lepton_diag.last_i2c_ok = 1;      /* the 0x0000 (if any) is a real read */
            if ((status & LEP_STATUS_BOOT_STATUS) &&
                ((status & LEP_STATUS_BUSY) == 0U))
            {
                lepton_diag.booted       = 1;
                lepton_diag.boot_wait_ms = HAL_GetTick() - t0;
                return LEP_OK;
            }
        }
        else
        {
            lepton_diag.last_i2c_ok = 0;      /* NACK/bus error -> status_reg is stale */
        }
        HAL_Delay(10);
    } while ((HAL_GetTick() - t0) < timeout_ms);

    lepton_diag.boot_wait_ms = HAL_GetTick() - t0;
    return LEP_ERR_BOOT;
}

/* Enable/disable radiometric TLinear output (raw16 = temperature in cK). */
Lepton_Status_t Lepton_EnableTLinear(uint8_t enable)
{
    uint16_t d[2];
    d[0] = enable ? 1U : 0U;   /* LEP_RAD_ENABLE_E is 32-bit: low word */
    d[1] = 0U;
    return Lepton_CCI_SetAttr(LEP_CID_RAD_TLINEAR_EN_SET, d, 2);
}

/* Enable/disable AGC (turn OFF to keep radiometric linear data). */
Lepton_Status_t Lepton_SetAGC(uint8_t enable)
{
    uint16_t d[2];
    d[0] = enable ? 1U : 0U;
    d[1] = 0U;
    return Lepton_CCI_SetAttr(LEP_CID_AGC_ENABLE_SET, d, 2);
}

/* Trigger a Flat-Field Correction (shutter). */
Lepton_Status_t Lepton_RunFFC(void)
{
    return Lepton_CCI_RunCmd(LEP_CID_SYS_FFC_RUN);
}

/* Scan the whole 7-bit I2C address space (0x08..0x77) and record which devices
   ACK. Distinguishes "nothing on the bus" (count=0) from "device answers at a
   different address" (e.g. SDA/SCL swapped, or wrong slave) -> fills diag. */
void Lepton_I2C_BusScan(void)
{
    lepton_diag.i2c_scan_first = 0xFF;
    lepton_diag.i2c_scan_count = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++)
    {
        if (HAL_I2C_IsDeviceReady(lepton_hi2c, (uint16_t)(a << 1), 2, 20) == HAL_OK)
        {
            if (lepton_diag.i2c_scan_first == 0xFF) lepton_diag.i2c_scan_first = a;
            lepton_diag.i2c_scan_count++;
        }
    }
}

/* Read the true idle voltage on SCL(PD12)/SDA(PD13). Both lines are open-drain
   with external pull-ups, so at idle they MUST float high. Reading LOW means the
   line is physically stuck (no pull-up / short to GND / VDDIO off / device
   holding it) -> the exact cause of an I2C TIMEOUT (err=0x20). GPIO input mode
   is switched only momentarily, then restored to I2C AF. */
static void Lepton_I2C_LineCheck(void)
{
    GPIO_InitTypeDef gi = {0};

    /* sample as plain inputs (no pull, so we see the external pull-up's true level) */
    gi.Mode  = GPIO_MODE_INPUT;
    gi.Pull  = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_LOW;
    gi.Pin   = GPIO_PIN_12 | GPIO_PIN_13;
    HAL_GPIO_Init(GPIOD, &gi);

    lepton_diag.scl_idle = (HAL_GPIO_ReadPin(GPIOD, GPIO_PIN_12) == GPIO_PIN_SET) ? 1U : 0U;
    lepton_diag.sda_idle = (HAL_GPIO_ReadPin(GPIOD, GPIO_PIN_13) == GPIO_PIN_SET) ? 1U : 0U;

    /* restore I2C4 alternate function (AF4, open-drain) exactly as MspInit set it */
    gi.Mode      = GPIO_MODE_AF_OD;
    gi.Pull      = GPIO_NOPULL;
    gi.Speed     = GPIO_SPEED_FREQ_LOW;
    gi.Alternate = GPIO_AF4_I2C4;
    gi.Pin       = GPIO_PIN_12 | GPIO_PIN_13;
    HAL_GPIO_Init(GPIOD, &gi);
}

static void Lepton_I2C4_ResetAndReinit(void)
{
    __HAL_RCC_I2C4_FORCE_RESET();
    __HAL_RCC_I2C4_RELEASE_RESET();
    (void)HAL_I2C_DeInit(lepton_hi2c);
    MX_I2C4_Init();   /* includes HAL_I2C_Init + analog/digital filter config */
}

/* ===========================================================================
 * Software bit-bang I2C on PD12(SCL)/PD13(SDA) -- bypasses the STM32 I2C4
 * peripheral entirely. This is the DECISIVE config-vs-hardware test:
 *   - if the Lepton ACKs here but not via HAL  -> I2C4 peripheral setup is the
 *     culprit (and we have a working CCI path as a bonus);
 *   - if it does NOT ACK here either          -> hardware/level (2.8V rail /
 *     3V3 pull-up / wiring), stop chasing config.
 * Open-drain is driven like Drivers/soft/SW_I2C: both pins stay in OD output
 * mode; writing SET releases the line, writing RESET pulls it low. ~50kHz via
 * delay_us(10).
 * ------------------------------------------------------------------------- */
#define BB_PIN_12    GPIO_PIN_12   /* PD12 = I2C4_SCL per CubeMX */
#define BB_PIN_13    GPIO_PIN_13   /* PD13 = I2C4_SDA per CubeMX */
#define BB_PORT      GPIOD
#define BB_HALF_US   10U       /* half bit period -> ~50kHz, tolerant bring-up timing */

/* Runtime-selectable role assignment so we can test BOTH the normal wiring
   (SCL=PD12, SDA=PD13) and the swapped wiring (SCL=PD13, SDA=PD12) in one
   power-up. If the swapped pass ACKs at 0x2A, SDA/SCL are physically swapped. */
static uint16_t bb_scl_pin = BB_PIN_12;
static uint16_t bb_sda_pin = BB_PIN_13;

static void bb_bus_od_config(void)
{
    GPIO_InitTypeDef gi = {0};

    __HAL_RCC_GPIOD_CLK_ENABLE();
    HAL_GPIO_WritePin(BB_PORT, BB_PIN_12 | BB_PIN_13, GPIO_PIN_SET);

    gi.Mode  = GPIO_MODE_OUTPUT_OD;
    gi.Pull  = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_LOW;
    gi.Pin   = BB_PIN_12 | BB_PIN_13;
    HAL_GPIO_Init(BB_PORT, &gi);

    HAL_GPIO_WritePin(BB_PORT, BB_PIN_12 | BB_PIN_13, GPIO_PIN_SET);
}

static void bb_pin_input(uint16_t pin)   /* release (open-drain high) */
{
    HAL_GPIO_WritePin(BB_PORT, pin, GPIO_PIN_SET);
}

static void bb_pin_output_low(uint16_t pin)   /* drive low */
{
    HAL_GPIO_WritePin(BB_PORT, pin, GPIO_PIN_RESET);
}

static void     bb_scl_high(void){ bb_pin_input(bb_scl_pin); }
static void     bb_scl_low (void){ bb_pin_output_low(bb_scl_pin); }
static void     bb_sda_high(void){ bb_pin_input(bb_sda_pin); }
static void     bb_sda_low (void){ bb_pin_output_low(bb_sda_pin); }
static uint8_t  bb_sda_read(void){ return (HAL_GPIO_ReadPin(BB_PORT, bb_sda_pin) == GPIO_PIN_SET) ? 1U : 0U; }

/* SCL clock stretch aware high: release SCL, wait for it to actually rise */
static void bb_scl_release_wait(void)
{
    bb_scl_high();
    delay_us(BB_HALF_US);
    /* honour clock-stretching: spin (bounded) until SCL actually reads high */
    for (uint32_t g = 0; g < 10000U; g++)
    {
        if (HAL_GPIO_ReadPin(BB_PORT, bb_scl_pin) == GPIO_PIN_SET) break;
        delay_us(1);
    }
}

static void bb_start(void)
{
    bb_sda_high(); bb_scl_high(); delay_us(BB_HALF_US);
    bb_sda_low();  delay_us(BB_HALF_US);          /* SDA falls while SCL high */
    bb_scl_low();  delay_us(BB_HALF_US);
}

static void bb_stop(void)
{
    bb_sda_low();  delay_us(BB_HALF_US);
    bb_scl_release_wait();
    bb_sda_high(); delay_us(BB_HALF_US);          /* SDA rises while SCL high */
}

/* write one byte, return ACK bit: 0 = ACK (device pulled SDA low), 1 = NACK */
static uint8_t bb_write_byte(uint8_t b)
{
    uint8_t ack;
    for (uint8_t i = 0; i < 8; i++)
    {
        if (b & 0x80U) bb_sda_high(); else bb_sda_low();
        b <<= 1;
        delay_us(BB_HALF_US);
        bb_scl_release_wait();
        bb_scl_low();
        delay_us(BB_HALF_US);
    }
    /* 9th clock: release SDA, sample ACK */
    bb_sda_high();
    delay_us(BB_HALF_US);
    bb_scl_release_wait();
    ack = bb_sda_read();          /* 0 = ACK */
    bb_scl_low();
    delay_us(BB_HALF_US);
    return ack;
}

static uint8_t bb_read_byte(uint8_t ack_after)
{
    uint8_t b = 0;

    bb_sda_high();   /* release SDA so the slave can drive data */
    for (uint8_t i = 0; i < 8; i++)
    {
        b <<= 1;
        delay_us(BB_HALF_US);
        bb_scl_release_wait();
        if (bb_sda_read()) b |= 1U;
        bb_scl_low();
        delay_us(BB_HALF_US);
    }

    /* 9th clock: ACK=drive SDA low, NACK=release SDA high */
    if (ack_after) bb_sda_low(); else bb_sda_high();
    delay_us(BB_HALF_US);
    bb_scl_release_wait();
    bb_scl_low();
    delay_us(BB_HALF_US);
    bb_sda_high();
    return b;
}

/* Probe one 7-bit address with a bit-bang START + addr(write) + STOP.
   Returns 1 if the device ACKed (SDA pulled low on the 9th clock). */
static uint8_t bb_probe_addr(uint8_t addr7)
{
    uint8_t ack;
    bb_start();
    ack = bb_write_byte((uint8_t)((addr7 << 1) | 0U));   /* write bit */
    bb_stop();
    return (ack == 0U) ? 1U : 0U;                        /* 1 = present */
}

static void bb_restore_af(void)   /* put PD12/PD13 back to I2C4 AF (OD) */
{
    GPIO_InitTypeDef gi = {0};
    gi.Mode      = GPIO_MODE_AF_OD;
    gi.Pull      = GPIO_NOPULL;
    gi.Speed     = GPIO_SPEED_FREQ_LOW;
    gi.Alternate = GPIO_AF4_I2C4;
    gi.Pin       = BB_PIN_12 | BB_PIN_13;
    HAL_GPIO_Init(BB_PORT, &gi);
}

static HAL_StatusTypeDef Lepton_BB_Write_Mem(uint16_t reg, const uint8_t *buf, uint16_t len)
{
    if ((buf == NULL) && (len != 0U)) return HAL_ERROR;

    bb_scl_pin = BB_PIN_12;
    bb_sda_pin = BB_PIN_13;
    bb_bus_od_config();

    bb_start();
    if (bb_write_byte((uint8_t)(LEPTON_I2C_ADDR << 1))) goto fail;
    if (bb_write_byte((uint8_t)(reg >> 8)))             goto fail;
    if (bb_write_byte((uint8_t)(reg & 0xFFU)))          goto fail;
    for (uint16_t i = 0; i < len; i++)
    {
        if (bb_write_byte(buf[i])) goto fail;
    }
    bb_stop();
    return HAL_OK;

fail:
    bb_stop();
    return HAL_ERROR;
}

static HAL_StatusTypeDef Lepton_BB_Read_Mem(uint16_t reg, uint8_t *buf, uint16_t len)
{
    if ((buf == NULL) || (len == 0U)) return HAL_ERROR;

    bb_scl_pin = BB_PIN_12;
    bb_sda_pin = BB_PIN_13;
    bb_bus_od_config();

    bb_start();
    if (bb_write_byte((uint8_t)(LEPTON_I2C_ADDR << 1))) goto fail;
    if (bb_write_byte((uint8_t)(reg >> 8)))             goto fail;
    if (bb_write_byte((uint8_t)(reg & 0xFFU)))          goto fail;

    bb_start();   /* repeated START before read phase */
    if (bb_write_byte((uint8_t)((LEPTON_I2C_ADDR << 1) | 1U))) goto fail;
    for (uint16_t i = 0; i < len; i++)
    {
        buf[i] = bb_read_byte((i + 1U) < len);  /* ACK all but the last byte */
    }
    bb_stop();
    return HAL_OK;

fail:
    bb_stop();
    return HAL_ERROR;
}

static void Lepton_I2C_BusRecover(void)
{
    __HAL_RCC_GPIOD_CLK_ENABLE();

    bb_scl_pin = BB_PIN_12;
    bb_sda_pin = BB_PIN_13;
    bb_bus_od_config();

    bb_sda_high();
    bb_scl_high();
    delay_us(50);

    /* Standard I2C recovery: 9 SCL pulses then STOP. This releases a slave that
       is stuck mid-byte after a reset or aborted transaction. */
    for (uint8_t i = 0; i < 9; i++)
    {
        bb_scl_low();
        delay_us(BB_HALF_US);
        bb_scl_release_wait();
        delay_us(BB_HALF_US);
    }
    bb_stop();

    bb_restore_af();
    Lepton_I2C4_ResetAndReinit();
}

/* Run a full 0x08..0x77 scan with the CURRENT (bb_scl_pin, bb_sda_pin) role
   assignment; return the number of devices that ACKed and the first address. */
static uint8_t bb_scan_pass(uint8_t *p_first)
{
    uint8_t count = 0;
    *p_first = 0xFF;

    bb_bus_od_config();
    bb_scl_high(); bb_sda_high();     /* idle both high before starting */
    delay_us(50);

    for (uint8_t a = 0x08; a <= 0x77; a++)
    {
        if (bb_probe_addr(a))
        {
            if (*p_first == 0xFF) *p_first = a;
            count++;
        }
        delay_us(20);
    }
    return count;
}

/* Public: bit-bang probe of the Lepton at 0x2A + a full 0x08..0x77 scan, run
   BOTH with the normal and the swapped SCL/SDA role. Fills lepton_diag.bb_* and
   restores the I2C4 alternate-function afterwards. */
void Lepton_I2C_BitBang_Probe(void)
{
    uint8_t first;

    __HAL_RCC_GPIOD_CLK_ENABLE();

    /* ---- Pass 1: NORMAL wiring (SCL=PD12, SDA=PD13) ---- */
    bb_scl_pin = BB_PIN_12;
    bb_sda_pin = BB_PIN_13;
    lepton_diag.bb_scan_count = bb_scan_pass(&first);
    lepton_diag.bb_scan_first = first;
    lepton_diag.bb_ack_2a     = bb_probe_addr(LEPTON_I2C_ADDR);

    /* ---- Pass 2: SWAPPED wiring (SCL=PD13, SDA=PD12) ---- */
    bb_scl_pin = BB_PIN_13;
    bb_sda_pin = BB_PIN_12;
    lepton_diag.bb_swap_count = bb_scan_pass(&first);
    lepton_diag.bb_swap_first = first;
    lepton_diag.bb_swap_ack   = bb_probe_addr(LEPTON_I2C_ADDR);

    /* restore default role for any later use */
    bb_scl_pin = BB_PIN_12;
    bb_sda_pin = BB_PIN_13;

    bb_restore_af();   /* hand PD12/PD13 back to the I2C4 peripheral */

    /* The HAL probe that ran before us left I2C4 with a latched error, and we
       just toggled its SCL/SDA pads as plain GPIO. Fully reset + re-init the
       peripheral so any later HAL transfer starts from a clean state. */
    Lepton_I2C4_ResetAndReinit();
}

/* ===========================================================================
 * Bring-up entry point
 * ======================================================================== */
void Lepton_Init(SPI_HandleTypeDef *hspi, I2C_HandleTypeDef *hi2c)
{
    lepton_hspi = hspi;
    lepton_hi2c = hi2c;
    lepton_cci_use_bitbang = 0U;
    lepton_diag.cci_transport = 0U;

    /* clock FIRST, then take the control pins, then release reset */
    Lepton_MCLK_Config();          /* ~24MHz on PA8/TIM1_CH1 (MUST be wired to MST_CLK) */
    Lepton_CtrlPins_Config();      /* RST/PWDN outputs (asserted low), VSYNC in */
    Lepton_CS_GPIO_Config();       /* PE4 -> GPIO output, idle high */
    Lepton_SPI_Config(hspi);       /* SPI4 -> VoSPI mode 3, 8-bit, 7.5MHz, soft NSS */

    Lepton_PowerOn_Sequence();     /* release PWDN, then RESET, wait ~950ms boot */

    /* Before touching the bus, read the raw idle level of SCL/SDA. If either is
       LOW here, the bus is physically stuck (no pull-up / short / VDDIO off) and
       every transfer will TIMEOUT (err=0x20) -> hardware, not firmware. */
    Lepton_I2C_LineCheck();

    /* If the lines are idle-high, issue a conservative bus recovery before the
       first HAL address probe. This costs <1ms and clears a slave that was left
       holding SDA after a previous reset or aborted transaction. */
    if (lepton_diag.scl_idle && lepton_diag.sda_idle)
        Lepton_I2C_BusRecover();

    /* Probe the CCI slave: does anything ACK at 0x2A on I2C4? This separates
       "I2C bus/wiring/power dead" (i2c_ready=0) from "device present but STATUS
       reads 0" (i2c_ready=1, status_reg=0). Capture the HAL error code + ISR so
       we can tell an Acknowledge-Failure (AF=0x04: bus OK, nobody answered) from
       a bus error/timeout (BERR/ARLO/TIMEOUT: stuck line / no pull-up / dead
       peripheral clock). */
    lepton_diag.i2c_ready =
        (HAL_I2C_IsDeviceReady(lepton_hi2c, (uint16_t)(LEPTON_I2C_ADDR << 1), 3, 100) == HAL_OK)
        ? 1U : 0U;
    lepton_diag.i2c_err = lepton_hi2c->ErrorCode;
    lepton_diag.i2c_isr = lepton_hi2c->Instance->ISR;

    /* If 0x2A did not answer, scan the whole bus: an ACK at some OTHER address
       means the wiring is alive but SDA/SCL are swapped or the module uses a
       different address; zero ACKs means dead bus / no pull-up / wrong pins. */
    if (!lepton_diag.i2c_ready)
        Lepton_I2C_BusScan();

    /* DECISIVE config-vs-hardware test: if the HAL peripheral got no ACK, retry
       the exact same probe with a pure software (bit-bang) I2C that bypasses the
       STM32 I2C4 peripheral entirely (same PD12/PD13 pins, same 3V3 pull-ups).
         bb_ack=1  -> hardware/wiring/level is FINE, the I2C4 peripheral config is
                      the culprit -> and we now have a working CCI path in SW.
         bb_ack=0  -> the failure is hardware/analog (level / rail / Lepton),
                      not firmware config -> stop chasing the peripheral setup.  */
    if (!lepton_diag.i2c_ready)
        Lepton_I2C_BitBang_Probe();

    if (!lepton_diag.i2c_ready && lepton_diag.bb_ack_2a)
    {
        lepton_cci_use_bitbang = 1U;
        lepton_diag.cci_transport = 1U;
    }

    lepton_diag.tlinear_err = -1;  /* sentinel: TLinear not attempted */

    /* CCI: wait for boot-complete, then set radiometric mode.
       Failures are recorded in lepton_diag and printed by main; we do not
       hang here so the rest of the super-loop keeps running. */
    if (Lepton_WaitBoot(2000) == LEP_OK)
    {
        Lepton_Status_t st = Lepton_EnableTLinear(1);
        lepton_diag.tlinear_err = (st == LEP_OK) ? 0 : (int8_t)st;
        /* TLinear implies radiometric; AGC stays off by default. FFC will run
           automatically on the first frames, so no explicit RunFFC here. */
    }

    LEPTON_CS_HIGH();
}

/* ===========================================================================
 * VoSPI capture
 * ======================================================================== */

/* Read one 164-byte VoSPI packet (CS low for the whole transfer). */
HAL_StatusTypeDef Lepton_SPI_Read_Packet(uint8_t *p_buf)
{
    HAL_StatusTypeDef ret;
    if (p_buf == NULL || lepton_hspi == NULL)
        return HAL_ERROR;

    LEPTON_CS_LOW();
    delay_us(2);
    ret = HAL_SPI_Receive(lepton_hspi, p_buf, LEPTON_PACK_SIZE, 100);
    delay_us(2);
    LEPTON_CS_HIGH();
    delay_us(2);
    return ret;
}

/* Force a VoSPI resync: deassert CS and idle for >5 frame periods (~185ms),
   after which the Lepton restarts segment output cleanly. */
void Lepton_VoSPI_Resync(void)
{
    LEPTON_CS_HIGH();
    HAL_Delay(185);
    /* The stream restarts from scratch after a resync: nothing committed
       before it may pair with what comes after. */
    vospi_gen++;
    vospi_last_commit_seg = 0U;
}

static void Lepton_VoSPI_CommitSegment(uint8_t seg)
{
    uint8_t base_row;

    if (seg < 1U || seg > LEPTON_SEG_CNT)
        return;

    base_row = (uint8_t)((seg - 1U) * 30U);
    for (uint8_t p = 0; p < LEPTON_PKT_PER_SEG; p++)
    {
        uint8_t  row = (uint8_t)(base_row + (p >> 1));
        uint16_t col = (p & 1U) ? 80U : 0U;
        for (uint8_t i = 0; i < 80U; i++)
        {
            lepton_assembly_frame[row][col + i] =
                (uint16_t)((seg_payload[p][i * 2U] << 8) | seg_payload[p][i * 2U + 1U]);
        }
    }

    /* Segment number wrapped (or repeated) -> the stream has moved on to a
       new Lepton frame; everything committed from here on is a new generation. */
    if (seg <= vospi_last_commit_seg)
        vospi_gen++;
    vospi_seg_gen[seg] = vospi_gen;
    vospi_last_commit_seg = seg;

    vospi_cached_mask |= (uint8_t)(1U << (seg - 1U));
    lepton_diag.vospi_got_mask = vospi_cached_mask;
}

/* ---------------------------------------------------------------------------
 * Capture one full Lepton 3.5 frame (segmented VoSPI) into
 * lepton_raw_frame[120][160].
 * @retval 1 = segment cache is complete and segment 4 just arrived.
 *           0 = no full publishable frame yet / read error / garbled.
 * @note   Polling implementation per the VoSPI spec. On a board without a valid
 *         master clock (or no module) it returns 0 after the packet guard trips,
 *         so it never hangs the main loop.
 * ------------------------------------------------------------------------- */
uint8_t Lepton_Capture_Frame(void)
{
    uint32_t pkt_guard = 0;

    Lepton_VoSPI_DiagReset();

    while (pkt_guard < LEPTON_VOSPI_PACKET_GUARD)
    {
        /* ---- sync: read until packet 0 of a segment (skip discard packets) ---- */
        uint8_t sync_discard_seen = 0;
        for (;;)
        {
            uint8_t pkt_class;

            if (++pkt_guard > LEPTON_VOSPI_PACKET_GUARD)
            {
                lepton_diag.vospi_fail_reason = LEPTON_VOSPI_FAIL_GUARD;
                return 0;                                      /* give up */
            }
            if (Lepton_SPI_Read_Packet(lepton_spi_pkt) != HAL_OK)
            {
                lepton_diag.vospi_spi_err++;
                lepton_diag.vospi_fail_reason = LEPTON_VOSPI_FAIL_SPI;
                return 0;
            }
            pkt_class = Lepton_VoSPI_DiagPacket();
            if (pkt_class == VOSPI_PKT_DISCARD)
            {
                if (sync_discard_seen == 0U)
                {
                    lepton_diag.vospi_sync_waits++;
                    HAL_Delay(LEPTON_VOSPI_FIRST_DISCARD_WAIT_MS);
                    sync_discard_seen = 1U;
                }
                else
                {
                    HAL_Delay(LEPTON_VOSPI_RETRY_WAIT_MS);
                }
                continue;
            }
            if (pkt_class == VOSPI_PKT_INVALID)
            {
                lepton_diag.vospi_fail_reason = LEPTON_VOSPI_FAIL_INVALID;
                HAL_Delay(LEPTON_VOSPI_RETRY_WAIT_MS);
                continue;
            }
            sync_discard_seen = 0;
            if (lepton_spi_pkt[1] == 0)
            {
                lepton_diag.vospi_pkt0++;
                break;                                         /* packet 0 */
            }
        }

        /* ---- read packets 0..59 of this segment into seg_payload ---- */
        uint8_t seg  = 0xFF;
        uint8_t fail = 0;
        for (uint8_t p = 0; p < LEPTON_PKT_PER_SEG; p++)
        {
            if (p != 0)   /* packet 0 already in lepton_spi_pkt */
            {
                uint8_t pkt_class;
                uint8_t intra_retry = 0;

                for (;;)
                {
                    if (++pkt_guard > LEPTON_VOSPI_PACKET_GUARD)
                    {
                        lepton_diag.vospi_fail_reason = LEPTON_VOSPI_FAIL_GUARD;
                        return 0;
                    }
                    if (Lepton_SPI_Read_Packet(lepton_spi_pkt) != HAL_OK)
                    {
                        lepton_diag.vospi_spi_err++;
                        lepton_diag.vospi_fail_reason = LEPTON_VOSPI_FAIL_SPI;
                        return 0;
                    }
                    pkt_class = Lepton_VoSPI_DiagPacket();
                    if (pkt_class != VOSPI_PKT_DISCARD)
                        break;
                    /* discard mid-segment: data not ready yet, wait and
                       re-read; only give up after the retry budget. */
                    if (++intra_retry > LEPTON_VOSPI_INTRA_DISCARD_MAX)
                        break;
                    HAL_Delay(LEPTON_VOSPI_RETRY_WAIT_MS);
                }
                if (pkt_class == VOSPI_PKT_DISCARD)   /* retry budget exhausted */
                {
                    lepton_diag.vospi_desync++;
                    lepton_diag.vospi_last_expected = p;
                    lepton_diag.vospi_fail_reason = LEPTON_VOSPI_FAIL_DESYNC;
                    HAL_Delay(LEPTON_VOSPI_RETRY_WAIT_MS);
                    fail = 1;
                    break;
                }
                if (pkt_class == VOSPI_PKT_INVALID)
                {
                    lepton_diag.vospi_last_expected = p;
                    lepton_diag.vospi_fail_reason = LEPTON_VOSPI_FAIL_INVALID;
                    HAL_Delay(LEPTON_VOSPI_RETRY_WAIT_MS);
                    fail = 1;
                    break;
                }
                if (lepton_spi_pkt[1] != p)
                {
                    lepton_diag.vospi_desync++;
                    lepton_diag.vospi_last_expected = p;
                    lepton_diag.vospi_fail_reason = LEPTON_VOSPI_FAIL_DESYNC;
                    HAL_Delay(LEPTON_VOSPI_RETRY_WAIT_MS);
                    fail = 1;
                    break;
                }
            }

            /* Lepton 3.x segment number is only valid in packet 20, high nibble of ID[0]. */
            if (p == 20)
            {
                seg = (uint8_t)((lepton_spi_pkt[0] >> 4) & 0x0F);
                lepton_diag.vospi_last_seg = seg;
                if (seg >= 1U && seg <= LEPTON_SEG_CNT)
                {
                    if (lepton_diag.vospi_seg_seen[seg] < 0xFFFFU)
                        lepton_diag.vospi_seg_seen[seg]++;
                }
                if (seg == 0U)
                {
                    /* Segment id 0 = non-exported dummy frame (Lepton 3.5
                       runs 26Hz internally but exports 8.7Hz: 2 of every 3
                       frame slots stream with seg=0). Per spec the host
                       reads the remaining packets silently and drops the
                       segment — bailing out here with a delay breaks
                       packet-level sync and kills the NEXT (real) segment. */
                    lepton_diag.vospi_bad_seg++;
                    lepton_diag.vospi_seg_bad0++;
                    /* keep reading packets 21..59; seg stays 0 -> no commit */
                }
                else if (seg > LEPTON_SEG_CNT)
                {
                    lepton_diag.vospi_bad_seg++;
                    lepton_diag.vospi_seg_badx++;
                    lepton_diag.vospi_last_expected = p;
                    lepton_diag.vospi_fail_reason = LEPTON_VOSPI_FAIL_BAD_SEG;
                    HAL_Delay(LEPTON_VOSPI_RETRY_WAIT_MS);
                    fail = 1;
                    break;
                }
            }

            for (uint16_t b = 0; b < 160; b++)
                seg_payload[p][b] = lepton_spi_pkt[4 + b];
        }
        if (fail) continue;   /* lost sync mid-segment -> retry from packet 0 */

        /* ---- commit a complete segment into the persistent frame buffer ---- */
        if (seg >= 1 && seg <= LEPTON_SEG_CNT)
        {
            if (lepton_diag.vospi_seg_ok[seg] < 0xFFFFU)
                lepton_diag.vospi_seg_ok[seg]++;

            if (vospi_cached_mask & (uint8_t)(1U << (seg - 1U)))
            {
                if (lepton_diag.vospi_dup_seg < 0xFFU)
                    lepton_diag.vospi_dup_seg++;
            }

            Lepton_VoSPI_CommitSegment(seg);

            /* Match the best observed board behavior: collect valid segments
               1..4 on a persistent shelf and publish when segment 4 arrives.
               Do not clear the shelf after publish; later segments refresh it. */
            if ((seg == LEPTON_SEG_CNT) &&
                ((vospi_cached_mask & 0x0FU) == 0x0FU))
            {
                uint8_t fresh_ok = 1U;
#if LEPTON_VOSPI_FRESH_PUBLISH
                /* Fresh gate: all four shelf slots must carry the SAME
                   acquisition generation, i.e. this Lepton frame's own
                   segments. A stale mix (a segment dropped this round, slot
                   still holding the previous round) is NOT published — keep
                   collecting; the next seg-1 opens a new generation. */
                if ((vospi_seg_gen[1] != vospi_seg_gen[LEPTON_SEG_CNT]) ||
                    (vospi_seg_gen[2] != vospi_seg_gen[LEPTON_SEG_CNT]) ||
                    (vospi_seg_gen[3] != vospi_seg_gen[LEPTON_SEG_CNT]))
                {
                    fresh_ok = 0U;
                    if (lepton_diag.vospi_stale_block < 0xFFFFU)
                        lepton_diag.vospi_stale_block++;
                    /* Starvation guard: if the same-generation test keeps
                       failing across captures, fall back to the stitched
                       shelf rather than returning "no frame" forever. */
                    if (++vospi_stale_streak >= LEPTON_VOSPI_STALE_FORCE_PUBLISH)
                        fresh_ok = 1U;
                }
#endif
                if (fresh_ok)
                {
                    vospi_stale_streak = 0U;
                    memcpy(lepton_raw_frame, lepton_assembly_frame, sizeof(lepton_raw_frame));
                    lepton_diag.vospi_got_mask = 0x0FU;
                    lepton_diag.vospi_fail_reason = LEPTON_VOSPI_FAIL_NONE;
                    return 1;
                }
            }
        }
    }

    lepton_diag.vospi_fail_reason = LEPTON_VOSPI_FAIL_GUARD;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Convert a raw 16-bit value to Celsius.
 * @note   Valid only in Radiometric/TLinear mode (1 LSB = 0.01 K):
 *         T[C] = raw*0.01 - 273.15. In AGC mode the value is not a temperature.
 * ------------------------------------------------------------------------- */
float Lepton_Raw_To_Temp(uint16_t raw_val)
{
    return (float)raw_val * 0.01f - 273.15f;
}

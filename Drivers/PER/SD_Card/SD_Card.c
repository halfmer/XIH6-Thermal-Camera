#include "SD_Card.h"
#include "string.h"
#include "DELAY.h"

/* low-level SDMMC register access (dynamic clock change) */
#include "stm32h7xx_ll_sdmmc.h"

extern SD_HandleTypeDef hsd2;

/* async DMA completion flags, set from SDMMC2 IRQ callbacks */
volatile uint8_t g_sd_rx_cplt = 0;
volatile uint8_t g_sd_tx_cplt = 0;
volatile uint8_t g_sd_error   = 0;

/* init diagnostics (latched on failure, printed by SD_ReportStatus in main.c) */
uint32_t sd_dbg_kerclk    = 0;   /* SDMMC kernel clock HAL sees, Hz (0 = bad clk) */
uint32_t sd_dbg_errorcode = 0;   /* hsd2.ErrorCode captured at the failing step  */
uint8_t  sd_dbg_stage     = 0;   /* 0 none, 1 identify, 2 high-speed, 3 4-bit    */
uint8_t  sd_dbg_cardver   = 0xFF;/* hsd2.SdCard.CardVersion at fail: 0=V1.x 1=V2.x*/

/* 32-byte aligned bounce buffer for unsafe (DTCM / unaligned) user buffers */
ALIGN_32BYTES(static uint8_t g_sd_safe_buf[512]);

#define IS_DTCM_ADDRESS(addr)  (((uint32_t)(addr) >= 0x20000000UL) && ((uint32_t)(addr) <= 0x2001FFFFUL))
#define IS_ALIGNED_32B(addr)   (((uint32_t)(addr) & 0x1FUL) == 0)
#define IS_BUFFER_SAFE(addr)   (!IS_DTCM_ADDRESS(addr) && IS_ALIGNED_32B(addr))

/**
  * @brief  Card presence via SD_CD (PG10). LOW = inserted.
  * @retval SD_OK if a card is present, SD_NOT_PRESENT otherwise.
  */
uint8_t SD_IsPresent(void)
{
    if (HAL_GPIO_ReadPin(SD_CD_PORT, SD_CD_PIN) == GPIO_PIN_RESET) return SD_OK;
    return SD_NOT_PRESENT;
}

/**
  * @brief  Full SD card bring-up: presence -> identify (low clk) -> 25MHz -> HS -> 4-bit.
  * @note   Self-contained: configures the whole hsd2 handle here so we never
  *         depend on MX_SDMMC2_SD_Init() (which traps into Error_Handler() on
  *         failure). delay_ms() is DWT-based (see DELAY.c) and leaves SysTick
  *         intact, so HAL_GetTick()/HAL_Delay() used inside HAL_SD_InitCard()
  *         still work.
  * @retval SD_OK / SD_NOT_PRESENT / SD_ERROR
  */
uint8_t SD_Card_Init(void)
{
    HAL_StatusTypeDef status = HAL_ERROR;
    uint8_t retry = 0;

    /* 1. is a card in the slot? */
    if (SD_IsPresent() != SD_OK) return SD_NOT_PRESENT;

    /* 1b. Hot-plug clean slate (the fix for "[SD] init FAILED" on re-insertion).
       HAL_SD_Init() only runs HAL_SD_MspInit() + a fresh power-up when
       hsd2.State == RESET. After the first bring-up the state is left at READY
       and the peripheral at 25MHz / 4-bit / high-speed. Re-running HAL_SD_Init()
       on a newly inserted card then identifies it against that stale config (and
       any latched error flags) -> CMD0/ACMD41 fail. HAL_SD_DeInit() powers the
       SDMMC off, clears the error code, de-inits the pins and forces
       State = RESET, so the following HAL_SD_Init() starts completely clean.
       Safe on the very first call: Instance is set and State is already RESET,
       so the DeInit is skipped. */
    hsd2.Instance = SDMMC2;
    if (hsd2.State != HAL_SD_STATE_RESET)
    {
        HAL_SD_DeInit(&hsd2);
    }

    /* 1c. Let the socket contacts settle: on a push-push holder the CD switch
       can close a few ms before CMD/CK/DAT are fully seated, so initialising
       the instant insertion is detected can still race the mechanical contacts. */
    delay_ms(100);

    /* 2. configure the handle (independent of MX_SDMMC2_SD_Init).
       NOTE: ClockDiv below is only a fallback - HAL_SD_InitCard() overrides it
       with its own value (sdmmc_ker_ck / (2*400kHz)) for the identification
       phase. SDMMC kernel clock = PLL2R = 200MHz on this board
       (RCC_SDMMCCLKSOURCE_PLL2 selects PLL2R, not PLL2P, on STM32H7). */
    hsd2.Init.ClockEdge           = SDMMC_CLOCK_EDGE_RISING;
    hsd2.Init.ClockPowerSave      = SDMMC_CLOCK_POWER_SAVE_DISABLE;
    hsd2.Init.BusWide             = SDMMC_BUS_WIDE_4B;
    hsd2.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
    hsd2.Init.ClockDiv            = 253;

    /* record the SDMMC kernel clock HAL uses to derive the <=400kHz id divider.
       A reading of 0 here means the SDMMC clock source is misconfigured and
       HAL_SD_InitCard() aborts immediately with SDMMC_ERROR_INVALID_PARAMETER. */
    sd_dbg_kerclk = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SDMMC);

    /* 3. init with retries; de-init between attempts so every retry is clean */
    for (retry = 0; retry < 3; retry++)
    {
        status = HAL_SD_Init(&hsd2);   /* State==RESET -> runs HAL_SD_MspInit */
        if (status == HAL_OK) break;
        sd_dbg_errorcode = hsd2.ErrorCode;       /* capture BEFORE DeInit clears it */
        sd_dbg_stage     = 1;                    /* 1 = card identification         */
        sd_dbg_cardver   = hsd2.SdCard.CardVersion; /* 1=V2.x => CMD8 answered      */
        HAL_SD_DeInit(&hsd2);
        delay_ms(100);
    }
    if (status != HAL_OK) return SD_ERROR;

    /* 4. raise bus clock to ~25MHz now that the card is in transfer state.
       On STM32H7 the clock divider lives in SDMMC_CLKCR and can be rewritten
       directly via SDMMC_Init(); no power down/up is needed (and H7 has no
       __HAL_SD_ENABLE/DISABLE macros, unlike F1/F4). */
    hsd2.Init.ClockDiv = 4;           /* SDMMC_CK = kerclk/(2*4) = 200MHz/8 = 25MHz
                                         (H7 formula is ker/(2*ClockDiv); ClockDiv=2
                                         would be 50MHz - above default-speed spec) */
    SDMMC_Init(hsd2.Instance, hsd2.Init);

    /* 5. high-speed mode (best effort) */
    if (HAL_SD_ConfigSpeedBusOperation(&hsd2, SDMMC_SPEED_MODE_HIGH) != HAL_OK)
    {
        sd_dbg_errorcode = hsd2.ErrorCode;
        sd_dbg_stage     = 2;                /* 2 = high-speed switch (CMD6) */
        return SD_ERROR;
    }

    /* 6. 4-bit wide bus */
    if (HAL_SD_ConfigWideBusOperation(&hsd2, SDMMC_BUS_WIDE_4B) != HAL_OK)
    {
        sd_dbg_errorcode = hsd2.ErrorCode;
        sd_dbg_stage     = 3;                /* 3 = 4-bit bus switch (ACMD6) */
        return SD_ERROR;
    }

    return SD_OK;
}

/* async IRQ callbacks (called from HAL_SD_IRQHandler in SDMMC2_IRQHandler) */
void HAL_SD_RxCpltCallback(SD_HandleTypeDef *hsd) { if (hsd->Instance == SDMMC2) g_sd_rx_cplt = 1; }
void HAL_SD_TxCpltCallback(SD_HandleTypeDef *hsd) { if (hsd->Instance == SDMMC2) g_sd_tx_cplt = 1; }
void HAL_SD_ErrorCallback(SD_HandleTypeDef *hsd)  { if (hsd->Instance == SDMMC2) g_sd_error = 1; }

/* Wait helpers. DELAY.c is DWT-based and leaves the HAL SysTick tick intact, so
 * HAL_GetTick()/HAL_Delay() work normally. We still spin on delay_us(1) here (not
 * HAL_Delay) only to get a fine, ~1us polling granularity: the SDMMC2 IRQ preempts
 * this spin and sets the flag; the counter is a safety net against a stuck DMA. */
static uint8_t SD_Wait_RxCplt(uint32_t timeout_ms)
{
    uint32_t cnt = timeout_ms * 1000U;
    while (g_sd_rx_cplt == 0 && g_sd_error == 0)
    {
        if (cnt-- == 0U) return SD_TIMEOUT;
        delay_us(1);
    }
    if (g_sd_error) { g_sd_error = 0; return SD_ERROR; }
    g_sd_rx_cplt = 0;
    return SD_OK;
}

static uint8_t SD_Wait_TxCplt(uint32_t timeout_ms)
{
    uint32_t cnt = timeout_ms * 1000U;
    while (g_sd_tx_cplt == 0 && g_sd_error == 0)
    {
        if (cnt-- == 0U) return SD_TIMEOUT;
        delay_us(1);
    }
    if (g_sd_error) { g_sd_error = 0; return SD_ERROR; }
    g_sd_tx_cplt = 0;
    return SD_OK;
}

/**
  * @brief  Adaptive DMA multi-sector read (handles DTCM / unaligned buffers).
  */
uint8_t SD_ReadDisk_DMA(uint8_t *pBuffer, uint32_t SectorAddr, uint32_t NumberOfSectors)
{
    if (SD_IsPresent() != SD_OK) return SD_NOT_PRESENT;
    g_sd_rx_cplt = 0; g_sd_error = 0;

    if (IS_BUFFER_SAFE(pBuffer))
    {
        SCB_InvalidateDCache_by_Addr((uint32_t*)pBuffer, NumberOfSectors * 512);
        if (HAL_SD_ReadBlocks_DMA(&hsd2, pBuffer, SectorAddr, NumberOfSectors) != HAL_OK) return SD_ERROR;
        if (SD_Wait_RxCplt(5000) != SD_OK) return SD_ERROR;
    }
    else
    {
        for (uint32_t i = 0; i < NumberOfSectors; i++)
        {
            g_sd_rx_cplt = 0; g_sd_error = 0;
            SCB_InvalidateDCache_by_Addr((uint32_t*)g_sd_safe_buf, 512);
            if (HAL_SD_ReadBlocks_DMA(&hsd2, g_sd_safe_buf, SectorAddr + i, 1) != HAL_OK) return SD_ERROR;
            if (SD_Wait_RxCplt(2000) != SD_OK) return SD_ERROR;
            memcpy(pBuffer + (i * 512), g_sd_safe_buf, 512);
        }
    }
    {
        uint32_t t0 = HAL_GetTick();
        while (HAL_SD_GetCardState(&hsd2) != HAL_SD_CARD_TRANSFER)
        {
            if ((HAL_GetTick() - t0) > 1000U) return SD_TIMEOUT;
        }
    }
    return SD_OK;
}

/**
  * @brief  Adaptive DMA multi-sector write (handles DTCM / unaligned buffers).
  */
uint8_t SD_WriteDisk_DMA(const uint8_t *pBuffer, uint32_t SectorAddr, uint32_t NumberOfSectors)
{
    if (SD_IsPresent() != SD_OK) return SD_NOT_PRESENT;
    g_sd_tx_cplt = 0; g_sd_error = 0;

    if (IS_BUFFER_SAFE(pBuffer))
    {
        SCB_CleanDCache_by_Addr((uint32_t*)pBuffer, NumberOfSectors * 512);
        if (HAL_SD_WriteBlocks_DMA(&hsd2, (uint8_t *)pBuffer, SectorAddr, NumberOfSectors) != HAL_OK) return SD_ERROR;
        if (SD_Wait_TxCplt(5000) != SD_OK) return SD_ERROR;
    }
    else
    {
        for (uint32_t i = 0; i < NumberOfSectors; i++)
        {
            g_sd_tx_cplt = 0; g_sd_error = 0;
            memcpy(g_sd_safe_buf, pBuffer + (i * 512), 512);
            SCB_CleanDCache_by_Addr((uint32_t*)g_sd_safe_buf, 512);
            if (HAL_SD_WriteBlocks_DMA(&hsd2, g_sd_safe_buf, SectorAddr + i, 1) != HAL_OK) return SD_ERROR;
            if (SD_Wait_TxCplt(2000) != SD_OK) return SD_ERROR;
        }
    }
    {
        uint32_t t0 = HAL_GetTick();
        while (HAL_SD_GetCardState(&hsd2) != HAL_SD_CARD_TRANSFER)
        {
            if ((HAL_GetTick() - t0) > 1000U) return SD_TIMEOUT;
        }
    }
    return SD_OK;
}

void SD_GetCardInfo(HAL_SD_CardInfoTypeDef *CardInfo)
{
    HAL_SD_GetCardInfo(&hsd2, CardInfo);
}

#include "DELAY.h"

/**
  * @brief  One-time enable of the DWT cycle counter.
  */
static void DWT_Delay_Init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;   /* enable trace/debug blocks */
    DWT->CYCCNT       = 0U;
    DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;        /* start cycle counter */
}

/**
  * @brief  Microsecond delay based on the DWT cycle counter.
  * @note   IMPORTANT: this uses DWT, NOT SysTick. The previous SysTick-based
  *         implementation reconfigured SysTick->CTRL and disabled its interrupt
  *         (TICKINT), which permanently froze HAL_GetTick()/HAL_Delay() after the
  *         first call -- that made HAL_SD_Init() (which calls HAL_Delay inside)
  *         hang forever. Using DWT keeps the HAL SysTick tick fully intact.
  * @param  xus delay in microseconds. A single call must stay below the DWT
  *         wrap time (32-bit counter, about 9.5s at 450MHz); unsigned subtraction
  *         below tolerates a single wrap.
  */
void delay_us(uint32_t xus)
{
    uint32_t start, ticks;
    uint32_t cycles_per_us;

    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U)
    {
        DWT_Delay_Init();
    }

    cycles_per_us = SystemCoreClock / 1000000U;
    if (cycles_per_us == 0U)
    {
        cycles_per_us = 1U;
    }

    start = DWT->CYCCNT;
    ticks = xus * cycles_per_us;
    while ((DWT->CYCCNT - start) < ticks) { }
}

/**
  * @brief  Millisecond delay (wraps delay_us).
  */
void delay_ms(uint32_t xms)
{
    while (xms--)
    {
        delay_us(1000U);
    }
}

/**
  * @brief  Second delay (wraps delay_ms).
  */
void delay_s(uint32_t xs)
{
    while (xs--)
    {
        delay_ms(1000U);
    }
}

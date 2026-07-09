#include "fire_guard.h"
#include "adc.h"
#include "usart.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint16_t mq2_filt = 0, mq135_filt = 0;
static uint16_t mq2_ppm = 0,  mq135_ppm = 0;
static float    mq2_r0 = 0.0f, mq135_r0 = 0.0f;   /* clean-air baseline */
static float    mq2_rs_acc = 0.0f, mq135_rs_acc = 0.0f;
static uint8_t  cal_count = 0;
static uint8_t  alarm_on = 0, first_pass = 1;
static uint32_t last_tick = 0;
static FireGuard_State_t state = FIRE_STATE_WARMUP;

/* One polled conversion (~40µs incl. 810.5-cycle sampling), no IRQ/DMA.
   ADC1 -> MQ-2 (PA1_C / INP1), ADC2 -> MQ-135 (PA0_C / INP0): each sensor
   on its native ADC instance, both re-configured at runtime to the long
   sampling time the kΩ-level MQ divider needs (CubeMX leaves 1.5 cycles). */
static HAL_StatusTypeDef FireGuard_ConfigChannel(ADC_HandleTypeDef *hadc, uint32_t channel)
{
    ADC_ChannelConfTypeDef sConfig = {0};

    sConfig.Channel                = channel;
    sConfig.Rank                   = ADC_REGULAR_RANK_1;
    sConfig.SamplingTime           = ADC_SAMPLETIME_810CYCLES_5;
    sConfig.SingleDiff             = ADC_SINGLE_ENDED;
    sConfig.OffsetNumber           = ADC_OFFSET_NONE;
    sConfig.Offset                 = 0;
    sConfig.OffsetSignedSaturation = DISABLE;
    return HAL_ADC_ConfigChannel(hadc, &sConfig);
}

static uint16_t FireGuard_ReadAdc(ADC_HandleTypeDef *hadc, HAL_StatusTypeDef *st_out)
{
    uint16_t v = 0;
    HAL_StatusTypeDef st;

    st = HAL_ADC_Start(hadc);
    if (st == HAL_OK)
    {
        st = HAL_ADC_PollForConversion(hadc, 2);
        if (st == HAL_OK)
            v = (uint16_t)HAL_ADC_GetValue(hadc);
        (void)HAL_ADC_Stop(hadc);
    }
    if (st_out != NULL)
        *st_out = st;
    return v;
}

/* raw -> sensor resistance. Guards: raw==0 (open/unwired) returns -1. */
static float FireGuard_RawToRs(uint16_t raw, float rl_ohm)
{
    float vout = ((float)raw / 65535.0f) * FIRE_ADC_VREF * FIRE_ADC_DIVIDER;

    if (vout < 0.05f)                    /* unwired / shorted-low input */
        return -1.0f;
    if (vout > (FIRE_MQ_VC - 0.05f))
        vout = FIRE_MQ_VC - 0.05f;
    return rl_ohm * (FIRE_MQ_VC - vout) / vout;
}

static uint16_t FireGuard_RsToPpm(float rs, float r0, float a, float b)
{
    float ppm;

    if ((rs <= 0.0f) || (r0 <= 0.0f))
        return 0;
    ppm = a * powf(rs / r0, b);
    if (ppm < 0.0f)     ppm = 0.0f;
    if (ppm > 65535.0f) ppm = 65535.0f;
    return (uint16_t)ppm;
}

void FireGuard_Init(void)
{
    HAL_StatusTypeDef st1, st2;

    /* Alarm outputs off (pins already push-pull outputs from MX_GPIO_Init). */
    HAL_GPIO_WritePin(LED_CONTROL_GPIO_Port, LED_CONTROL_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(BEEP_GPIO_GPIO_Port,  BEEP_GPIO_Pin,   GPIO_PIN_RESET);

    /* Re-config both ADCs to long sampling (regen-proof). ADC1 -> MQ-2
       (PA1_C/INP1), ADC2 -> MQ-135 (PA0_C/INP0) - each on its native
       instance so the SYSCFG analog switch + GPIO setup from each ADC's
       own MspInit applies. */
    (void)FireGuard_ConfigChannel(&hadc1, ADC_CHANNEL_1);
    (void)FireGuard_ConfigChannel(&hadc2, ADC_CHANNEL_0);

    /* One-time linear calibration for both ADCs, boot path only. */
    (void)HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET_LINEARITY,
                                      ADC_SINGLE_ENDED);
    (void)HAL_ADCEx_Calibration_Start(&hadc2, ADC_CALIB_OFFSET_LINEARITY,
                                      ADC_SINGLE_ENDED);

    /* Boot-path self-test (USART1 print allowed here, no DMA in flight yet):
       one conversion per ADC with HAL status - next log splits "config
       problem" from "wiring problem" definitively. */
    {
        char sb[128];
        uint16_t v1 = FireGuard_ReadAdc(&hadc1, &st1);   /* MQ-2   */
        uint16_t v0 = FireGuard_ReadAdc(&hadc2, &st2);   /* MQ-135 */
        sprintf(sb, "[FIRE] selftest: MQ2(ADC1) raw=%u st=%d | MQ135(ADC2) raw=%u st=%d\r\n",
                (unsigned)v1, (int)st1, (unsigned)v0, (int)st2);
        (void)HAL_UART_Transmit(&huart1, (uint8_t *)sb, (uint16_t)strlen(sb), 30);
        if ((st2 == HAL_OK) && (v0 < 100U))
            (void)HAL_UART_Transmit(&huart1,
                (uint8_t *)"[FIRE] MQ135 conversion OK but pin ~0V -> check AO wiring / module 5V supply\r\n",
                79, 30);
    }

    state = FIRE_STATE_WARMUP;
}

void FireGuard_Poll(void)
{
    uint16_t mq2_raw, mq135_raw;
    uint32_t now = HAL_GetTick();

    if ((now - last_tick) < FIRE_GUARD_PERIOD_MS)
        return;
    last_tick = now;

    mq2_raw   = FireGuard_ReadAdc(&hadc1, NULL);   /* PA1_C, MQ-2   */
    mq135_raw = FireGuard_ReadAdc(&hadc2, NULL);   /* PA0_C, MQ-135 */

    if (first_pass)
    {
        mq2_filt = mq2_raw;  mq135_filt = mq135_raw;  first_pass = 0;
    }
    else
    {   /* light IIR: 7/8 old + 1/8 new */
        mq2_filt   = (uint16_t)(((uint32_t)mq2_filt   * 7U + mq2_raw)   >> 3);
        mq135_filt = (uint16_t)(((uint32_t)mq135_filt * 7U + mq135_raw) >> 3);
    }

    switch (state)
    {
    case FIRE_STATE_WARMUP:
        /* MQ heater stabilising: readings start HIGH and drift DOWN - that
           falling curve is normal and must not alarm or calibrate. */
        if (now >= FIRE_WARMUP_MS)
        {
            mq2_rs_acc = 0.0f; mq135_rs_acc = 0.0f; cal_count = 0;
            state = FIRE_STATE_CALIB;
        }
        break;

    case FIRE_STATE_CALIB:
    {   /* average clean-air Rs, derive R0 from the datasheet clean ratio */
        float rs2 = FireGuard_RawToRs(mq2_filt,   FIRE_MQ2_RL_OHM);
        float rs5 = FireGuard_RawToRs(mq135_filt, FIRE_MQ135_RL_OHM);
        if (rs2 > 0.0f) mq2_rs_acc   += rs2;
        if (rs5 > 0.0f) mq135_rs_acc += rs5;
        if (++cal_count >= FIRE_R0_CAL_SAMPLES)
        {
            mq2_r0   = (mq2_rs_acc   / (float)FIRE_R0_CAL_SAMPLES) / FIRE_MQ2_CLEAN_RATIO;
            mq135_r0 = (mq135_rs_acc / (float)FIRE_R0_CAL_SAMPLES) / FIRE_MQ135_CLEAN_RATIO;
            state = FIRE_STATE_RUN;
        }
        break;
    }

    case FIRE_STATE_RUN:
    default:
    {
        float rs2 = FireGuard_RawToRs(mq2_filt,   FIRE_MQ2_RL_OHM);
        float rs5 = FireGuard_RawToRs(mq135_filt, FIRE_MQ135_RL_OHM);
        mq2_ppm   = FireGuard_RsToPpm(rs2, mq2_r0,   FIRE_MQ2_PPM_A,   FIRE_MQ2_PPM_B);
        mq135_ppm = FireGuard_RsToPpm(rs5, mq135_r0, FIRE_MQ135_PPM_A, FIRE_MQ135_PPM_B);

        /* hysteresis in PPM: latch on either sensor, release on both */
        if (((float)mq2_ppm   >= FIRE_MQ2_ON_PPM) ||
            ((float)mq135_ppm >= FIRE_MQ135_ON_PPM))
            alarm_on = 1U;
        else if (((float)mq2_ppm   < FIRE_MQ2_OFF_PPM) &&
                 ((float)mq135_ppm < FIRE_MQ135_OFF_PPM))
            alarm_on = 0U;
        break;
    }
    }

    HAL_GPIO_WritePin(LED_CONTROL_GPIO_Port, LED_CONTROL_Pin,
                      alarm_on ? GPIO_PIN_SET : GPIO_PIN_RESET);

    /* Buzzer hook (PG9 HIGH = sound), enable as a later single-step change:
       HAL_GPIO_WritePin(BEEP_GPIO_GPIO_Port, BEEP_GPIO_Pin,
                         alarm_on ? GPIO_PIN_SET : GPIO_PIN_RESET); */
}

FireGuard_State_t FireGuard_State(void)  { return state;      }
uint16_t FireGuard_MQ2_Raw(void)         { return mq2_filt;   }
uint16_t FireGuard_MQ135_Raw(void)       { return mq135_filt; }
uint16_t FireGuard_MQ2_PPM(void)         { return mq2_ppm;    }
uint16_t FireGuard_MQ135_PPM(void)       { return mq135_ppm;  }
uint8_t  FireGuard_Alarm(void)           { return alarm_on;   }

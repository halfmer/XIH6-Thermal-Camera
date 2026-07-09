#include "fire_guard.h"
#include "adc.h"
#include "usart.h"
#include "lepton.h"
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
/* thermal over-heat channel: written by FireGuard_ThermalScan (video slice),
   read by FireGuard_Poll (time wheel) - same super-loop, no concurrency. */
static uint8_t  thermal_alarm = 0;
static uint16_t thermal_max_raw = 0;
static uint8_t  blink_phase = 0;

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

/* Scan the freshly assembled LEPTON frame for a >=100 C hot spot. Called
   from the video slice right after Lepton_Capture_Frame() succeeds, so
   lepton_raw_frame is a complete coherent frame (never mid-assembly).
   ~19200 compares = ~100 µs worst case at 480 MHz, and it runs while the
   TX DMA is already streaming the same frame - the video path never waits.
   Latch on FIRE_THERMAL_MIN_PIX hot pixels (dead-pixel immunity), release
   only when the valid-pixel maximum cools below the OFF level. */
void FireGuard_ThermalScan(void)
{
    const uint16_t *px = &lepton_raw_frame[0][0];
    uint16_t maxv = 0;
    uint32_t hot = 0;
    uint32_t i;

    for (i = 0; i < (uint32_t)LEPTON_IMG_WIDTH * LEPTON_IMG_HEIGHT; i++)
    {
        uint16_t v = px[i];
        if (v == 0xFFFFU)                /* dead/stuck pixel sentinel */
            continue;
        if (v > maxv)
            maxv = v;
        if (v >= FIRE_THERMAL_ON_RAW)
            hot++;
    }

    thermal_max_raw = maxv;
    if (hot >= FIRE_THERMAL_MIN_PIX)
        thermal_alarm = 1U;
    else if (maxv < FIRE_THERMAL_OFF_RAW)
        thermal_alarm = 0U;
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

    /* Output stage, priority order (user spec 2026-07-09):
       1) thermal >=100 C  : lamp and buzzer toggle together each 200 ms poll
                             (2.5 Hz blink + intermittent beep) - runs in ANY
                             MQ state, the thermal channel needs no warm-up;
       2) gas alarm only   : lamp steady on, buzzer silent (unchanged);
       3) no alarm         : both off. */
    if (thermal_alarm != 0U)
    {
        blink_phase ^= 1U;
        HAL_GPIO_WritePin(LED_CONTROL_GPIO_Port, LED_CONTROL_Pin,
                          blink_phase ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_GPIO_WritePin(BEEP_GPIO_GPIO_Port, BEEP_GPIO_Pin,
                          blink_phase ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
    else
    {
        blink_phase = 0U;
        HAL_GPIO_WritePin(LED_CONTROL_GPIO_Port, LED_CONTROL_Pin,
                          alarm_on ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_GPIO_WritePin(BEEP_GPIO_GPIO_Port, BEEP_GPIO_Pin, GPIO_PIN_RESET);
    }
}

FireGuard_State_t FireGuard_State(void)  { return state;      }
uint16_t FireGuard_MQ2_Raw(void)         { return mq2_filt;   }
uint16_t FireGuard_MQ135_Raw(void)       { return mq135_filt; }
uint16_t FireGuard_MQ2_PPM(void)         { return mq2_ppm;    }
uint16_t FireGuard_MQ135_PPM(void)       { return mq135_ppm;  }
uint8_t  FireGuard_Alarm(void)           { return alarm_on;   }
uint8_t  FireGuard_ThermalAlarm(void)    { return thermal_alarm;   }
uint16_t FireGuard_ThermalMaxRaw(void)   { return thermal_max_raw; }

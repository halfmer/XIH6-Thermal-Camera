#ifndef __FIRE_GUARD_H
#define __FIRE_GUARD_H

#include "main.h"

/* ----------------------------------------------------------------------------
 * Fire-guard: MQ-2 (smoke/LPG) + MQ-135 (air quality/CO2-eq) quantified in
 * PPM per the industry-standard MQ model, alarm on dedicated driver circuits.
 *
 * Wiring:
 *   MQ-2   AO -> PA1_C = ADC1_INP1   MQ-135 AO -> PA0_C = ADC2_INP0
 *   Each on its native ADC instance (SYSCFG analog switch + GPIO set up by
 *   that ADC's own MspInit), both re-configured to 810.5-cycle sampling.
 *   RGB lamp     : PJ15 (LED_CONTROL) -> AO3400 NMOS, HIGH = on
 *   Alarm buzzer : PG9  (BEEP_GPIO)   -> NPN low-side, HIGH = sound
 *                  (hook present, intentionally NOT driven yet)
 *   !! MQ modules run their divider from 5V: AO can exceed 3.3V. The H743
 *   analog pads are NOT 5V-tolerant - use a resistor divider on AO and set
 *   FIRE_ADC_DIVIDER accordingly (e.g. 2:1 divider -> 2.0f).
 *
 * Quantification (the standard MQ model, datasheet log-log curve fit):
 *   Vout   = raw/65535 * 3.3V * FIRE_ADC_DIVIDER
 *   Rs     = RL * (Vc - Vout) / Vout          (RL = module load resistor)
 *   ratio  = Rs / R0                          (R0 = Rs in CLEAN AIR)
 *   ppm    = A * ratio^B                      (A/B per gas curve)
 * Curves used (classic library constants, adjust after field calibration):
 *   MQ-2  LPG/smoke : A=574.25      B=-2.222   clean-air Rs/R0 = 9.83
 *   MQ-135 CO2-eq   : A=116.602069  B=-2.769   clean-air Rs/R0 = 3.60
 *
 * R0 calibration: after FIRE_WARMUP_MS the module samples clean air for
 * FIRE_R0_CAL_SAMPLES polls and derives R0 from the clean-air ratio - so
 * power the board in clean air. Alarm is suppressed during warm-up+cal
 * (this also kills the cold-heater false alarm: an MQ element reads HIGH
 * while the heater warms up, then settles - the falling curve is normal).
 *
 * Scheduling contract: FireGuard_Poll() is one time-wheel task, gated to
 * FIRE_GUARD_PERIOD_MS, costing two polled conversions (~40µs each after
 * the 810.5-cycle sampling fix) - it can never disturb the LEPTON path.
 * ------------------------------------------------------------------------- */

#define FIRE_GUARD_PERIOD_MS   200U
#define FIRE_WARMUP_MS         60000U   /* MQ heater stabilisation (datasheet: minutes) */
#define FIRE_R0_CAL_SAMPLES    16U      /* clean-air averaging polls for R0 */

#define FIRE_ADC_VREF          3.3f
#define FIRE_ADC_DIVIDER       1.0f     /* AO->ADC divider ratio (Vao/Vadc); 2.0f for 2:1 */
#define FIRE_MQ_VC             5.0f     /* MQ module divider supply */
#define FIRE_MQ2_RL_OHM        5100.0f  /* typical MQ-2 module load resistor */
#define FIRE_MQ135_RL_OHM      10000.0f /* typical MQ-135 module load resistor */

#define FIRE_MQ2_CLEAN_RATIO   9.83f
#define FIRE_MQ2_PPM_A         574.25f
#define FIRE_MQ2_PPM_B         -2.222f
#define FIRE_MQ135_CLEAN_RATIO 3.60f
#define FIRE_MQ135_PPM_A       116.602069f
#define FIRE_MQ135_PPM_B       -2.769f

/* Alarm thresholds in PPM with hysteresis. User spec 2026-07-09: the RGB
   lamp (PJ15, AO3400 NMOS - "RGB灯" from here on) lights when MQ-2 exceeds
   4 ppm (clean-air LPG baseline computes to ~3.6 ppm, so 4 ppm = very
   sensitive, trips on any real gas above ambient). */
#define FIRE_MQ2_ON_PPM        4.0f
#define FIRE_MQ2_OFF_PPM       3.0f
#define FIRE_MQ135_ON_PPM      2000.0f
#define FIRE_MQ135_OFF_PPM     1500.0f

/* ----------------------------------------------------------------------------
 * Thermal over-heat channel (user spec 2026-07-09): any hot spot >= 100 C in
 * the LEPTON frame -> RGB lamp BLINKS + buzzer BEEPS (both toggled together
 * at the FIRE_GUARD_PERIOD_MS cadence = 2.5 Hz), overriding the gas alarm
 * (gas alone keeps the lamp STEADY and the buzzer silent).
 *
 * Raw is TLinear centikelvin (0.01 K/LSB, README_2 §"TLinear"):
 *   raw = (T_C + 273.15) * 100  ->  100 C = 37315, 95 C release = 36815.
 * The sensor has known dead pixels that can read 0/0xFFFF (README_9..12 era):
 * 0xFFFF is skipped outright and the alarm additionally needs at least
 * FIRE_THERMAL_MIN_PIX pixels at/above the ON level - an isolated stuck
 * pixel can never trip it, while any real >=100 C source covers several
 * pixels of the 160x120 field. Latch releases only when the frame maximum
 * (dead pixels excluded) drops below the OFF level; no timeout - a fire
 * alarm stays loud until the scene itself reads cool (fail-loud).
 * This channel is independent of the MQ warm-up/calibration state machine:
 * the thermal camera needs no heater stabilisation.
 * ------------------------------------------------------------------------- */
#define FIRE_THERMAL_ON_RAW    37315U   /* (100.00 C + 273.15) * 100 */
#define FIRE_THERMAL_OFF_RAW   36815U   /* 95.00 C release (5 C hysteresis) */
#define FIRE_THERMAL_MIN_PIX   4U       /* pixels >= ON needed to latch */

typedef enum {
    FIRE_STATE_WARMUP = 0,   /* heater stabilising, alarm suppressed */
    FIRE_STATE_CALIB  = 1,   /* sampling clean air for R0            */
    FIRE_STATE_RUN    = 2    /* quantified ppm + alarm active        */
} FireGuard_State_t;

void              FireGuard_Init(void);
void              FireGuard_Poll(void);      /* time-wheel task */
void              FireGuard_ThermalScan(void); /* call after each complete LEPTON frame */
FireGuard_State_t FireGuard_State(void);
uint16_t          FireGuard_MQ2_Raw(void);   /* filtered 16-bit raw */
uint16_t          FireGuard_MQ135_Raw(void);
uint16_t          FireGuard_MQ2_PPM(void);   /* 0 until FIRE_STATE_RUN */
uint16_t          FireGuard_MQ135_PPM(void);
uint8_t           FireGuard_Alarm(void);
uint8_t           FireGuard_ThermalAlarm(void);  /* 1 = hot spot >= 100 C latched */
uint16_t          FireGuard_ThermalMaxRaw(void); /* frame max, dead px excluded */

#endif /* __FIRE_GUARD_H */

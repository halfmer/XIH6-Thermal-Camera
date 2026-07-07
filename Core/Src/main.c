/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "dma.h"
#include "i2c.h"
#include "ltdc.h"
#include "sdmmc.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "LED.h"
#include "DELAY.h"
#include "oled.h"
#include "SHT40.h"
#include "SD_Card.h"
#include <stdio.h>
#include "string.h"
#include "lepton.h"
#include "lepton_stream.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define SD_TEST_SECTOR   0x10000UL   /* scratch LBA used by the R/W self-test */
#define SD_UART_DMA_RING_SIZE 8192U
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
float temperature = 0.0f;
float humidity = 0.0f;
uint8_t sht40_status = 0;
char disp_buff[32]; //

/* SD card state */
uint8_t sd_init_status = SD_NOT_PRESENT;
uint8_t sd_present_last = 0xFF;          /* 0xFF forces a first-pass refresh */
HAL_SD_CardInfoTypeDef sd_info;

/* Thermal stream interval diagnostics. These counters are USART1-only and do
   not change the UART4 binary frame format. */
static uint32_t stream_diag_last_ms = 0U;
static uint32_t stream_diag_cap_ok = 0U;
static uint32_t stream_diag_cap_fail = 0U;
static uint32_t stream_diag_resync = 0U;
static uint32_t stream_diag_send_ok = 0U;
static uint32_t stream_diag_send_fail = 0U;
static uint32_t stream_diag_cap_ms_sum = 0U;
static uint32_t stream_diag_cap_ms_max = 0U;
static uint32_t stream_diag_send_ms_sum = 0U;
static uint32_t stream_diag_send_ms_max = 0U;

static uint8_t sd_uart_dma_ring[SD_UART_DMA_RING_SIZE] __attribute__((aligned(32)));
static volatile uint16_t sd_uart_dma_head = 0U;
static volatile uint16_t sd_uart_dma_tail = 0U;
static volatile uint16_t sd_uart_dma_active_len = 0U;
static volatile uint8_t  sd_uart_dma_active = 0U;
static volatile uint32_t sd_uart_dma_drop_count = 0U;
static volatile uint32_t sd_uart_dma_fail_count = 0U;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void PeriphCommonClock_Config(void);
static void MPU_Config(void);
/* USER CODE BEGIN PFP */
void SD_UART_Print(const char *s);
void SD_UART_TxCpltCallback(UART_HandleTypeDef *huart);
void SD_UART_ErrorCallback(UART_HandleTypeDef *huart);
static void SD_ReportStatus(void);
static uint8_t SD_SelfTest(uint32_t sector);
static void LEP_PrintVoSPIDiag(const char *tag);
static void LEP_StreamDiag_MaybePrint(uint32_t now_ms);
static void SD_UART_DMA_Kick(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
    unsigned char s_buf[]="It's mygo!!!!!\r\n";

static void LEP_PrintVoSPIDiag(const char *tag)
{
    char vd[448];

    sprintf(vd, "[LEP] VoSPI %s reason=%u reads=%lu valid=%lu discard=%lu invalid=%lu pkt0=%lu desync=%lu badseg=%lu spierr=%lu mask=0x%02X seen=%u/%u/%u/%u segs=%u/%u/%u/%u dup=%u bad0=%u badx=%u wait=%u exp=%u last=%02X %02X %02X %02X seg=0x%02X\r\n",
            tag,
            (unsigned)lepton_diag.vospi_fail_reason,
            (unsigned long)lepton_diag.vospi_reads,
            (unsigned long)lepton_diag.vospi_valid,
            (unsigned long)lepton_diag.vospi_discard,
            (unsigned long)lepton_diag.vospi_invalid,
            (unsigned long)lepton_diag.vospi_pkt0,
            (unsigned long)lepton_diag.vospi_desync,
            (unsigned long)lepton_diag.vospi_bad_seg,
            (unsigned long)lepton_diag.vospi_spi_err,
            (unsigned)lepton_diag.vospi_got_mask,
            (unsigned)lepton_diag.vospi_seg_seen[1],
            (unsigned)lepton_diag.vospi_seg_seen[2],
            (unsigned)lepton_diag.vospi_seg_seen[3],
            (unsigned)lepton_diag.vospi_seg_seen[4],
            (unsigned)lepton_diag.vospi_seg_ok[1],
            (unsigned)lepton_diag.vospi_seg_ok[2],
            (unsigned)lepton_diag.vospi_seg_ok[3],
            (unsigned)lepton_diag.vospi_seg_ok[4],
            (unsigned)lepton_diag.vospi_dup_seg,
            (unsigned)lepton_diag.vospi_seg_bad0,
            (unsigned)lepton_diag.vospi_seg_badx,
            (unsigned)lepton_diag.vospi_sync_waits,
            (unsigned)lepton_diag.vospi_last_expected,
            (unsigned)lepton_diag.vospi_last_id0,
            (unsigned)lepton_diag.vospi_last_id1,
            (unsigned)lepton_diag.vospi_last_crc0,
            (unsigned)lepton_diag.vospi_last_crc1,
            (unsigned)lepton_diag.vospi_last_seg);
    SD_UART_Print(vd);
}

static void LEP_StreamDiag_MaybePrint(uint32_t now_ms)
{
    uint32_t elapsed;
    uint32_t cap_total;
    uint32_t send_total;
    uint32_t cap_avg;
    uint32_t send_avg;
    uint16_t raw_min;
    uint16_t raw_max;
    uint16_t raw_span;
    uint32_t flat_drop;
    uint32_t flat_total;
    uint32_t uart1_drop;
    uint32_t uart1_fail;
    char msg[704];

    if (stream_diag_last_ms == 0U)
    {
        stream_diag_last_ms = now_ms;
        return;
    }

    elapsed = now_ms - stream_diag_last_ms;
    if (elapsed < 5000U)
        return;

    cap_total = stream_diag_cap_ok + stream_diag_cap_fail;
    send_total = stream_diag_send_ok + stream_diag_send_fail;
    cap_avg = (cap_total > 0U) ? (stream_diag_cap_ms_sum / cap_total) : 0U;
    send_avg = (send_total > 0U) ? (stream_diag_send_ms_sum / send_total) : 0U;
    Lepton_Stream_GetContentDiag(&raw_min, &raw_max, &raw_span, &flat_drop, &flat_total);
    uart1_drop = sd_uart_dma_drop_count;
    uart1_fail = sd_uart_dma_fail_count;

    snprintf(msg, sizeof(msg),
             "[STRM_DIAG] %lums cap_ok=%lu cap_fail=%lu resync=%lu send_ok=%lu send_fail=%lu cap_avg=%lums cap_max=%lums send_avg=%lums send_max=%lums fid=%u raw=%u/%u span=%u flat=%lu/%lu uart1Drop=%lu uart1Fail=%lu reason=%u mask=0x%02X ok=%u/%u/%u/%u seen=%u/%u/%u/%u reads=%lu discard=%lu desync=%lu badseg=%lu spierr=%lu\r\n",
             (unsigned long)elapsed,
             (unsigned long)stream_diag_cap_ok,
             (unsigned long)stream_diag_cap_fail,
             (unsigned long)stream_diag_resync,
             (unsigned long)stream_diag_send_ok,
             (unsigned long)stream_diag_send_fail,
             (unsigned long)cap_avg,
             (unsigned long)stream_diag_cap_ms_max,
             (unsigned long)send_avg,
             (unsigned long)stream_diag_send_ms_max,
             (unsigned)Lepton_Stream_FrameCount(),
             (unsigned)raw_min,
             (unsigned)raw_max,
             (unsigned)raw_span,
             (unsigned long)flat_drop,
             (unsigned long)flat_total,
             (unsigned long)uart1_drop,
             (unsigned long)uart1_fail,
             (unsigned)lepton_diag.vospi_fail_reason,
             (unsigned)lepton_diag.vospi_got_mask,
             (unsigned)lepton_diag.vospi_seg_ok[1],
             (unsigned)lepton_diag.vospi_seg_ok[2],
             (unsigned)lepton_diag.vospi_seg_ok[3],
             (unsigned)lepton_diag.vospi_seg_ok[4],
             (unsigned)lepton_diag.vospi_seg_seen[1],
             (unsigned)lepton_diag.vospi_seg_seen[2],
             (unsigned)lepton_diag.vospi_seg_seen[3],
             (unsigned)lepton_diag.vospi_seg_seen[4],
             (unsigned long)lepton_diag.vospi_reads,
             (unsigned long)lepton_diag.vospi_discard,
             (unsigned long)lepton_diag.vospi_desync,
             (unsigned long)lepton_diag.vospi_bad_seg,
             (unsigned long)lepton_diag.vospi_spi_err);
    SD_UART_Print(msg);

    stream_diag_last_ms = now_ms;
    stream_diag_cap_ok = 0U;
    stream_diag_cap_fail = 0U;
    stream_diag_resync = 0U;
    stream_diag_send_ok = 0U;
    stream_diag_send_fail = 0U;
    stream_diag_cap_ms_sum = 0U;
    stream_diag_cap_ms_max = 0U;
    stream_diag_send_ms_sum = 0U;
    stream_diag_send_ms_max = 0U;
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  uint32_t test_count = 0;
  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* Configure the peripherals common clocks */
  PeriphCommonClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_SDMMC2_SD_Init();
  MX_I2C4_Init();
  MX_SPI4_Init();
  MX_ADC1_Init();
  MX_ADC2_Init();
  MX_LTDC_Init();
  MX_SPI5_Init();
  MX_UART4_Init();
  MX_UART5_Init();
  /* USER CODE BEGIN 2 */
  /* CH340C link (UART4 @ PA0/PA1, TX/RX swapped in CR2) -> 1.5Mbps.
     Carries only the binary thermal stream; host sends 'S'/'P'. */
  Lepton_Stream_Init(&huart4);

  /* Buzzer silent on request. New HW: PG9 -> 1k -> NPN base (10k base-emitter),
     emitter=GND, collector=buzzer(-), buzzer(+)=VCC. Active-HIGH low-side drive:
     PG9 HIGH = transistor ON = sound; PG9 LOW = OFF = silent. */
  HAL_GPIO_WritePin(BEEP_GPIO_GPIO_Port, BEEP_GPIO_Pin, GPIO_PIN_RESET);

  SW_I2C_Init();
  OLED_Init();
  
  OLED_Clear(); 
  OLED_ShowString(0, 0, (uint8_t *)"--- SHT40 DATA ---", 12, 1);
  
  OLED_ShowString(0, 16, (uint8_t *)"Temp: ", 16, 1); 
  OLED_ShowString(0, 32, (uint8_t *)"Humi: ", 16, 1);


  SD_UART_Print((const char *)s_buf);

  /* ---- Reset-cause banner. If inserting the SD card makes THIS line re-print
     with POR/BOR/PIN set, the "freeze" is really a brownout/reset on insertion
     (a power-integrity hardware fault), NOT a software hang in HAL_SD_Init. ---- */
  {
      char rb[96];
      uint32_t rsr = RCC->RSR;
      sprintf(rb, "\r\n[RST] RSR=0x%08lX POR=%u BOR=%u PIN=%u SFT=%u IWDG=%u LPWR=%u\r\n",
              (unsigned long)rsr,
              (unsigned)((rsr >> 23) & 1U),   /* PORRSTF  */
              (unsigned)((rsr >> 21) & 1U),   /* BORRSTF  */
              (unsigned)((rsr >> 22) & 1U),   /* PINRSTF  */
              (unsigned)((rsr >> 24) & 1U),   /* SFTRSTF  */
              (unsigned)((rsr >> 26) & 1U),   /* IWDG1RSTF*/
              (unsigned)((rsr >> 30) & 1U));  /* LPWRRSTF */
      SD_UART_Print(rb);
      __HAL_RCC_CLEAR_RESET_FLAGS();
  }

  SD_UART_Print("[STREAM] UART4/CH340 1.5Mbps binary only: 'S' = start thermal stream, 'P' = stop; debug logs on USART1\r\n");

  /* ---- SD card bring-up + read/write self-test ---- */
  if (SD_IsPresent() != SD_OK)
  {
      sd_present_last = 0;
      sd_init_status  = SD_NOT_PRESENT;
      OLED_ShowString(32, 48, (uint8_t *)"No Card ", 16, 1);
      SD_UART_Print("\r\n[SD] no card in slot\r\n");
  }
  else
  {
      sd_present_last = 1;
      sd_init_status  = SD_Card_Init();
      SD_ReportStatus();
      if (sd_init_status == SD_OK)
      {
          if (SD_SelfTest(SD_TEST_SECTOR) == SD_OK)
              SD_UART_Print("[SD] R/W self-test PASS\r\n");
          else
              SD_UART_Print("[SD] R/W self-test FAIL\r\n");
      }
  }

  /* ---- Lepton 3.5 thermal camera bring-up (VoSPI@SPI4, CCI@I2C4) ----
     Reconfigures PA8 as TIM1_CH1 ~24MHz and SPI4 (mode 3/8-bit/7.5MHz/soft-CS on PE4).
     Releases RESET_L(PJ3)/PWR_DWN_L(PH6) (CubeMX holds them asserted at boot),
     waits ~950ms boot, then enables radiometric TLinear over CCI. */
  Lepton_Init(&hspi4, &hi2c4);
  {
      char lbuf[320];
      char cci_boot = lepton_diag.last_i2c_ok ? (lepton_diag.booted ? '1' : '0') : '?';
      sprintf(lbuf, "[LEP] mclk=%u mclk_hz=%lu sys=%lu pclk2=%lu pwr=%u rst=%u rstIDR=%u pwdnIDR=%u i2c_rdy=%u cci=%s cci_boot=%c(%lums) status_ok=%u status=0x%04X tlin=%d\r\n",
              (unsigned)lepton_diag.mclk_on,
              (unsigned long)lepton_diag.mclk_hz,
              (unsigned long)lepton_diag.sysclk_hz,
              (unsigned long)lepton_diag.pclk2_hz,
              (unsigned)lepton_diag.powered,
              (unsigned)lepton_diag.reset_released, (unsigned)lepton_diag.rst_readback,
              (unsigned)lepton_diag.pwdn_readback, (unsigned)lepton_diag.i2c_ready,
              lepton_diag.cci_transport ? "BB" : "HAL",
              cci_boot, (unsigned long)lepton_diag.boot_wait_ms,
              (unsigned)lepton_diag.last_i2c_ok, (unsigned)lepton_diag.status_reg,
              (int)lepton_diag.tlinear_err);
      SD_UART_Print(lbuf);
      if (!lepton_diag.rst_readback)
          SD_UART_Print("[LEP] RESET_L(PJ3) reads LOW after release -> net held low (short/pulldown/HW), Lepton stuck in reset\r\n");
      else if (!lepton_diag.i2c_ready)
      {
          char ib[200];
          sprintf(ib, "[LEP] HAL I2C4 no ACK @0x2A: err=0x%lX isr=0x%lX SCLidle=%u SDAidle=%u scan_first=0x%02X scan_cnt=%u\r\n",
                  (unsigned long)lepton_diag.i2c_err, (unsigned long)lepton_diag.i2c_isr,
                  (unsigned)lepton_diag.scl_idle, (unsigned)lepton_diag.sda_idle,
                  (unsigned)lepton_diag.i2c_scan_first, (unsigned)lepton_diag.i2c_scan_count);
          SD_UART_Print(ib);
          if (!lepton_diag.last_i2c_ok)
              SD_UART_Print("[LEP] CCI status unread -> cci_boot is UNKNOWN; shutter/FFC motion still means the Lepton core likely booted\r\n");
          if (!lepton_diag.scl_idle || !lepton_diag.sda_idle)
              SD_UART_Print("[LEP] I2C line STUCK LOW at idle -> no pull-up / short to GND / VDDIO off / wrong pin. Fix wiring first.\r\n");
          else if (lepton_diag.i2c_scan_count == 0)
              /* NOTE: HAL_I2C_IsDeviceReady returns err=0x20(TIMEOUT) for a plain
                 no-ACK too (it clears AF each trial and ORs TIMEOUT at the end), so
                 0x20 alone does NOT prove a clock/peripheral fault. Lines idle-high
                 + 0 devices = nobody answers on an electrically-OK bus. The bit-bang
                 result below is the decisive config-vs-hardware test. */
              SD_UART_Print("[LEP] lines high but 0 devices -> see bit-bang test below (Lepton VDDIO 2.8V? pull-ups? SDA/SCL swap?)\r\n");
          else if (lepton_diag.i2c_scan_first != 0x2A)
              SD_UART_Print("[LEP] I2C alive but Lepton not at 0x2A -> likely SDA/SCL swapped or wrong module pins\r\n");

          /* DECISIVE config-vs-hardware test: SW_I2C-style open-drain GPIO on
             PD12/PD13, bypassing the STM32 I2C4 peripheral entirely. */
          {
              char bb[192];
              sprintf(bb, "[LEP] SW-OD I2C (SW_I2C-style, bypasses I2C4): ack@0x2A=%u scan_first=0x%02X scan_cnt=%u\r\n",
                      (unsigned)lepton_diag.bb_ack_2a,
                      (unsigned)lepton_diag.bb_scan_first,
                      (unsigned)lepton_diag.bb_scan_count);
              SD_UART_Print(bb);

              /* Second bit-bang pass with SCL/SDA roles SWAPPED (SCL=PD13,
                 SDA=PD12). If this pass ACKs where the normal pass did not, the
                 two lines are physically swapped -> just re-wire, no scope. */
              {
                  char sw[160];
                  sprintf(sw, "[LEP] bit-bang SWAPPED (SCL=PD13,SDA=PD12): ack@0x2A=%u scan_first=0x%02X scan_cnt=%u\r\n",
                          (unsigned)lepton_diag.bb_swap_ack,
                          (unsigned)lepton_diag.bb_swap_first,
                          (unsigned)lepton_diag.bb_swap_count);
                  SD_UART_Print(sw);
              }

              if (lepton_diag.bb_ack_2a)
                  SD_UART_Print("[LEP] >>> bit-bang ACKs @0x2A but HAL did NOT -> using software CCI fallback; I2C4 timing/config remains suspect\r\n");
              else if (lepton_diag.bb_swap_ack)
                  SD_UART_Print("[LEP] >>> SWAPPED pass ACKs @0x2A -> SDA/SCL ARE PHYSICALLY SWAPPED. Swap PD12<->PD13 wiring.\r\n");
              else if (lepton_diag.bb_scan_count > 0 || lepton_diag.bb_swap_count > 0)
                  SD_UART_Print("[LEP] >>> bit-bang finds device NOT at 0x2A -> wrong address / partial swap (HW alive)\r\n");
              else
                  SD_UART_Print("[LEP] >>> normal+swapped bit-bang both 0 devices -> not an I2C4 peripheral issue; check CCI pad power/level/timing or module state\r\n");
          }
      }
      else if (!lepton_diag.last_i2c_ok)
          SD_UART_Print("[LEP] I2C ACKed once but STATUS read failed -> CCI register phase/timing still suspect\r\n");
      else if (!lepton_diag.booted)
          SD_UART_Print("[LEP] CCI STATUS readable but boot bit is 0 -> check MCLK ~24MHz clean / reset timing\r\n");
  }
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

    /* ---- thermal stream mode: CH340 link carries binary frames only ---- */
    if (Lepton_Stream_Active())
    {
        uint32_t t0;
        uint32_t dt;
        uint8_t cap_ok;

        t0 = HAL_GetTick();
        cap_ok = Lepton_Capture_Frame();
        dt = HAL_GetTick() - t0;
        stream_diag_cap_ms_sum += dt;
        if (dt > stream_diag_cap_ms_max)
            stream_diag_cap_ms_max = dt;

        if (cap_ok)
        {
            HAL_StatusTypeDef tx_status;

            stream_diag_cap_ok++;
            t0 = HAL_GetTick();
            tx_status = Lepton_Stream_SendFrame();
            dt = HAL_GetTick() - t0;
            stream_diag_send_ms_sum += dt;
            if (dt > stream_diag_send_ms_max)
                stream_diag_send_ms_max = dt;
            if (tx_status == HAL_OK)
                stream_diag_send_ok++;
            else
                stream_diag_send_fail++;
        }
        else
        {
            stream_diag_cap_fail++;
            stream_diag_resync++;
            Lepton_VoSPI_Resync();
        }
        LEP_StreamDiag_MaybePrint(HAL_GetTick());
        LED_TURN(1);              /* 2ms heartbeat vs ~256ms per frame */
        continue;
    }

    LED_TURN(250);

    /* SD hot-plug edge detection (PG10 / SD_CD) */
    {
        uint8_t present_now = (SD_IsPresent() == SD_OK) ? 1 : 0;
        if (present_now != sd_present_last)
        {
            sd_present_last = present_now;
            if (present_now)
            {
                SD_UART_Print("[SD] insert detected -> init...\r\n");
                sd_init_status = SD_Card_Init();
                SD_UART_Print("[SD] init call returned\r\n");
                SD_ReportStatus();
                SD_UART_Print("[SD] card inserted\r\n");
            }
            else
            {
                sd_init_status = SD_NOT_PRESENT;
                OLED_ShowString(32, 48, (uint8_t *)"No Card ", 16, 1);
                SD_UART_Print("[SD] card removed\r\n");
            }
        }
    }
    test_count++;

    /* Lepton: grab one frame every ~16 loops (~8s); fails fast if no data */
    if ((test_count % 16U) == 0U)
    {
        if (Lepton_Capture_Frame())
        {
            char lp[48];
            uint16_t craw = lepton_raw_frame[60][80];   /* centre pixel */
            float    ctmp = Lepton_Raw_To_Temp(craw);
            sprintf(lp, "[LEP] OK c_raw=%u c=%.2fC\r\n", (unsigned)craw, ctmp);
            SD_UART_Print(lp);
            LEP_PrintVoSPIDiag("okdiag");
        }
        else
        {
            SD_UART_Print("[LEP] no frame (check MCLK/VoSPI); resync\r\n");
            LEP_PrintVoSPIDiag("diag");
            Lepton_VoSPI_Resync();
        }
    }

    sht40_status = sht40_read_data(&temperature, &humidity);
    
    if (sht40_status == 0) 
    { 
        sprintf(disp_buff, "%.2f C   ", temperature);
        OLED_ShowString(48, 16, (uint8_t *)disp_buff, 16, 1);
        
        sprintf(disp_buff, "%.2f %%   ", humidity);
        OLED_ShowString(48, 32, (uint8_t *)disp_buff, 16, 1);

        /* Buzzer stays silent (paused on request). New HW is active-HIGH:
           to sound, WritePin(BEEP..., GPIO_PIN_SET); to silence, GPIO_PIN_RESET. */
        if (temperature >= 30.0f)        /* over-temp: would sound (PG9 HIGH) */
        {
            (void)0; /* BEEP paused */
        }
        else if (temperature < 29.5f)    /* back below threshold: would silence (PG9 LOW) */
        {
            (void)0; /* BEEP paused */
        }
    }

    
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 60;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief Peripherals Common Clock Configuration
  * @retval None
  */
void PeriphCommonClock_Config(void)
{
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

  /** Initializes the peripherals clock
  */
  PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_ADC|RCC_PERIPHCLK_SDMMC;
  PeriphClkInitStruct.PLL2.PLL2M = 32;
  PeriphClkInitStruct.PLL2.PLL2N = 200;
  PeriphClkInitStruct.PLL2.PLL2P = 4;
  PeriphClkInitStruct.PLL2.PLL2Q = 2;
  PeriphClkInitStruct.PLL2.PLL2R = 2;
  PeriphClkInitStruct.PLL2.PLL2RGE = RCC_PLL2VCIRANGE_1;
  PeriphClkInitStruct.PLL2.PLL2VCOSEL = RCC_PLL2VCOWIDE;
  PeriphClkInitStruct.PLL2.PLL2FRACN = 0;
  PeriphClkInitStruct.SdmmcClockSelection = RCC_SDMMCCLKSOURCE_PLL2;
  PeriphClkInitStruct.AdcClockSelection = RCC_ADCCLKSOURCE_PLL2;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

static uint16_t SD_UART_RingAdvance(uint16_t value, uint16_t delta)
{
    uint32_t next = (uint32_t)value + (uint32_t)delta;

    if (next >= SD_UART_DMA_RING_SIZE)
        next -= SD_UART_DMA_RING_SIZE;
    return (uint16_t)next;
}

static void SD_UART_RestoreIRQ(uint32_t primask)
{
    if ((primask & 1U) == 0U)
        __enable_irq();
}

static void SD_UART_CleanDCache(uint8_t *buf, uint32_t len)
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

static void SD_UART_DMA_Kick(void)
{
    uint32_t primask;
    uint16_t head;
    uint16_t tail;
    uint16_t len;
    HAL_StatusTypeDef status;

    primask = __get_PRIMASK();
    __disable_irq();
    if ((sd_uart_dma_active != 0U) || (sd_uart_dma_head == sd_uart_dma_tail))
    {
        SD_UART_RestoreIRQ(primask);
        return;
    }

    head = sd_uart_dma_head;
    tail = sd_uart_dma_tail;
    len = (head > tail) ? (uint16_t)(head - tail)
                        : (uint16_t)(SD_UART_DMA_RING_SIZE - tail);
    sd_uart_dma_active = 1U;
    sd_uart_dma_active_len = len;
    SD_UART_RestoreIRQ(primask);

    SD_UART_CleanDCache(&sd_uart_dma_ring[tail], len);
    status = HAL_UART_Transmit_DMA(&huart1, &sd_uart_dma_ring[tail], len);
    if (status != HAL_OK)
    {
        primask = __get_PRIMASK();
        __disable_irq();
        sd_uart_dma_active = 0U;
        sd_uart_dma_active_len = 0U;
        sd_uart_dma_fail_count++;
        SD_UART_RestoreIRQ(primask);
    }
}

void SD_UART_Print(const char *s)
{
    uint32_t primask;
    uint32_t len;
    uint32_t i;

    if (s == NULL)
        return;

    len = (uint32_t)strlen(s);
    if (len == 0U)
        return;

    primask = __get_PRIMASK();
    __disable_irq();
    for (i = 0U; i < len; i++)
    {
        uint16_t next = SD_UART_RingAdvance(sd_uart_dma_head, 1U);

        if (next == sd_uart_dma_tail)
        {
            sd_uart_dma_drop_count += (len - i);
            break;
        }
        sd_uart_dma_ring[sd_uart_dma_head] = (uint8_t)s[i];
        sd_uart_dma_head = next;
    }
    SD_UART_RestoreIRQ(primask);

    SD_UART_DMA_Kick();
}

void SD_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    uint32_t primask;

    if (huart->Instance != USART1)
        return;

    primask = __get_PRIMASK();
    __disable_irq();
    sd_uart_dma_tail = SD_UART_RingAdvance(sd_uart_dma_tail, sd_uart_dma_active_len);
    sd_uart_dma_active_len = 0U;
    sd_uart_dma_active = 0U;
    SD_UART_RestoreIRQ(primask);

    SD_UART_DMA_Kick();
}

void SD_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    uint32_t primask;

    if (huart->Instance != USART1)
        return;

    primask = __get_PRIMASK();
    __disable_irq();
    sd_uart_dma_tail = SD_UART_RingAdvance(sd_uart_dma_tail, sd_uart_dma_active_len);
    sd_uart_dma_active_len = 0U;
    sd_uart_dma_active = 0U;
    sd_uart_dma_fail_count++;
    SD_UART_RestoreIRQ(primask);

    SD_UART_DMA_Kick();
}

/* Show SD status on OLED + UART (capacity in MB when OK). */
static void SD_ReportStatus(void)
{
    if (sd_init_status == SD_OK)
    {
        uint32_t cap_mb;
        SD_GetCardInfo(&sd_info);
        cap_mb = (uint32_t)(((uint64_t)sd_info.BlockNbr * sd_info.BlockSize) >> 20);
        sprintf(disp_buff, "%luMB    ", (unsigned long)cap_mb);
        OLED_ShowString(32, 48, (uint8_t *)disp_buff, 16, 1);
        sprintf(disp_buff, "[SD] init OK, %lu MB\r\n", (unsigned long)cap_mb);
        SD_UART_Print(disp_buff);
    }
    else if (sd_init_status == SD_NOT_PRESENT)
    {
        OLED_ShowString(32, 48, (uint8_t *)"No Card ", 16, 1);
        SD_UART_Print("[SD] not present\r\n");
    }
    else
    {
        char dbg[96];
        OLED_ShowString(32, 48, (uint8_t *)"Init Err", 16, 1);
        sprintf(dbg, "[SD] init FAILED stage=%u err=0x%08lX kerclk=%lu Hz cardver=%u\r\n",
                (unsigned)sd_dbg_stage, (unsigned long)sd_dbg_errorcode,
                (unsigned long)sd_dbg_kerclk, (unsigned)sd_dbg_cardver);
        SD_UART_Print(dbg);
    }
}

/* Write a known pattern to one sector, read it back, compare. */
static uint8_t SD_SelfTest(uint32_t sector)
{
    static uint8_t wbuf[512];
    static uint8_t rbuf[512];
    uint32_t i;

    for (i = 0; i < 512; i++) wbuf[i] = (uint8_t)(i ^ 0xA5);

    if (SD_WriteDisk_DMA(wbuf, sector, 1) != SD_OK) return SD_ERROR;
    memset(rbuf, 0, sizeof(rbuf));
    if (SD_ReadDisk_DMA(rbuf, sector, 1) != SD_OK) return SD_ERROR;

    return (memcmp(wbuf, rbuf, 512) == 0) ? SD_OK : SD_ERROR;
}

/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

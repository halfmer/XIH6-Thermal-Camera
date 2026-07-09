#ifndef __LEPTON_H
#define __LEPTON_H

#include "main.h"
#include "spi.h"
#include "i2c.h"

/* ============================================================================
 * FLIR Lepton 3.5 driver for XIH6_V2 (STM32H743, bare-metal)
 *
 * Board wiring (LEPTON_IO_CONNET.txt; silk names SPI3/I2C1, real pins SPI4/I2C4):
 *   VoSPI video : SPI4   PE2=SCK  PE5=MISO  PE6=MOSI(idle)  PE4=CS(software GPIO)
 *   CCI control : I2C4   PD12=SCL  PD13=SDA          (7-bit addr 0x2A)
 *   MST_CLK     : PA8  = TIM1_CH1 PWM ~24MHz  (last-known intermittent-OK baseline)
 *   RESET_L     : PJ3  (active-low)  <-- WIRED (CubeMX: LEPTON_RST_Pin/GPIOJ)
 *   PWR_DWN_L   : PH6  (active-low)  <-- WIRED (CubeMX: LEPTON_PWDN_Pin/GPIOH)
 *   VSYNC       : PB2  (Lepton output, input to MCU; LEPTON_VSYNC_Pin/GPIOB)
 *
 *   NOTE: MX_GPIO_Init() drives PJ3 and PH6 LOW at boot, i.e. it holds the
 *   Lepton in RESET *and* POWER-DOWN. The driver MUST release them in the
 *   correct order/timing (see Lepton_PowerOn_Sequence) or no frame ever comes.
 *
 * Segmented VoSPI (Lepton 3.5, 160x120):
 *   packet = 164 bytes = 2(ID) + 2(CRC) + 160(payload = 80 px * 2 bytes)
 *   discard packet : (ID[0] & 0x0F) == 0x0F
 *   packet number  : ID[1]  (0..59 within a segment)
 *   segment number : valid ONLY in packet 20, = (ID[0] >> 4) & 0x0F, range 1..4
 *   frame = 4 segments * 60 packets = 240 packets; each packet = 80 px =
 *           half a row, so segment s, packet p -> row=(s-1)*30 + p/2,
 *           col = (p&1)?80:0.
 * ==========================================================================*/

/* ---- software chip-select (PE4, reused as plain GPIO output) ------------- */
#define LEPTON_CS_GPIO_PORT    SPI_SSEL_GPIO_Port   /* GPIOE */
#define LEPTON_CS_GPIO_PIN     SPI_SSEL_Pin         /* GPIO_PIN_4 */

/* ---- CCI 7-bit I2C address ----------------------------------------------- */
#define LEPTON_I2C_ADDR        0x2A

/* ---- CCI register map (16-bit register addresses, data is big-endian) ---- */
#define LEP_REG_STATUS         0x0002U   /* [0]=BUSY, [1]=BootMode, [2]=BootStatus, [15:8]=err */
#define LEP_REG_COMMAND        0x0004U
#define LEP_REG_DATA_LENGTH    0x0006U   /* number of 16-bit words in DATA block */
#define LEP_REG_DATA0          0x0008U   /* DATA0..DATA15 : 0x0008..0x0026 */

#define LEP_STATUS_BUSY        0x0001U
#define LEP_STATUS_BOOT_MODE   0x0002U
#define LEP_STATUS_BOOT_STATUS 0x0004U   /* 1 = boot complete */

/* ---- CCI command word = MODULE_BASE | (cmd_offset) | TYPE [| PROT] ------- */
#define LEP_CMD_TYPE_GET       0x0000U
#define LEP_CMD_TYPE_SET       0x0001U
#define LEP_CMD_TYPE_RUN       0x0002U
#define LEP_CMD_PROT_BIT       0x4000U   /* required by RAD & OEM modules */

/* module bases: AGC=0x0100 SYS=0x0200 VID=0x0300 OEM=0x0800 RAD=0x0E00       */
#define LEP_CID_SYS_PING           (0x0200U | LEP_CMD_TYPE_RUN)                 /* 0x0202 */
#define LEP_CID_SYS_FFC_RUN        (0x0200U | 0x0042U | LEP_CMD_TYPE_RUN)       /* 0x0242 */
#define LEP_CID_AGC_ENABLE_SET     (0x0100U | 0x0000U | LEP_CMD_TYPE_SET)       /* 0x0101 */
#define LEP_CID_AGC_ENABLE_GET     (0x0100U | 0x0000U | LEP_CMD_TYPE_GET)       /* 0x0100 */
#define LEP_CID_RAD_TLINEAR_EN_SET (LEP_CMD_PROT_BIT | 0x0E00U | 0x00C0U | LEP_CMD_TYPE_SET) /* 0x4EC1 */
#define LEP_CID_RAD_TLINEAR_EN_GET (LEP_CMD_PROT_BIT | 0x0E00U | 0x00C0U | LEP_CMD_TYPE_GET) /* 0x4EC0 */

/* ---- packet / frame geometry --------------------------------------------- */
#define LEPTON_PACK_SIZE       164U   /* bytes per VoSPI packet */
#define LEPTON_PKT_PER_SEG     60U    /* valid packets per segment */
#define LEPTON_SEG_CNT         4U     /* segments per frame */
#define LEPTON_IMG_WIDTH       160U
#define LEPTON_IMG_HEIGHT      120U

/* ---- VoSPI capture failure reason (for diagnostics only) ----------------- */
#define LEPTON_VOSPI_FAIL_NONE     0U
#define LEPTON_VOSPI_FAIL_GUARD    1U
#define LEPTON_VOSPI_FAIL_SPI      2U
#define LEPTON_VOSPI_FAIL_DESYNC   3U
#define LEPTON_VOSPI_FAIL_BAD_SEG  4U
#define LEPTON_VOSPI_FAIL_INVALID  5U

/* ---- driver return / boot state ------------------------------------------ */
typedef enum {
    LEP_OK          = 0,
    LEP_ERR_I2C     = 1,   /* CCI I2C transport error */
    LEP_ERR_BOOT    = 2,   /* STATUS never reported boot complete */
    LEP_ERR_BUSY    = 3,   /* command BUSY timeout */
    LEP_ERR_CMD     = 4,   /* command returned a non-zero error code */
    LEP_ERR_PARAM   = 5
} Lepton_Status_t;

/* one-shot bring-up diagnostics, printed by main after Lepton_Init() */
typedef struct {
    uint8_t  powered;        /* PWR_DWN_L released (ODR set high) */
    uint8_t  reset_released; /* RESET_L released (ODR set high) */
    uint8_t  rst_readback;   /* actual PJ3 pad level after release (IDR): 1=high */
    uint8_t  pwdn_readback;  /* actual PH6 pad level after release (IDR): 1=high */
    uint8_t  i2c_ready;      /* CCI slave 0x2A ACKs on I2C4 (HAL_I2C_IsDeviceReady) */
    uint32_t i2c_err;        /* hi2c4.ErrorCode after the 0x2A probe (AF vs BERR/timeout) */
    uint32_t i2c_isr;        /* I2C4->ISR after the probe (idle should be 0x1; 0/0xFFFFFFFF=dead) */
    uint8_t  scl_idle;       /* PD12(SCL) pad level at idle: 1=high(pull-up OK), 0=stuck low */
    uint8_t  sda_idle;       /* PD13(SDA) pad level at idle: 1=high(pull-up OK), 0=stuck low */
    uint8_t  i2c_scan_first; /* first 7-bit addr that ACKed on a full bus scan (0xFF=none) */
    uint8_t  i2c_scan_count; /* how many 7-bit addresses ACKed on the scan */
    uint8_t  bb_ack_2a;      /* 1 = 0x2A ACKed via SOFTWARE bit-bang (bypasses I2C4 HW) */
    uint8_t  bb_scan_first;  /* first addr that ACKed on the bit-bang scan (0xFF=none) */
    uint8_t  bb_scan_count;  /* how many addresses ACKed on the bit-bang scan */
    uint8_t  bb_swap_ack;    /* 1 = 0x2A ACKed with SDA/SCL roles SWAPPED (PD12=SDA,PD13=SCL) */
    uint8_t  bb_swap_first;  /* first addr that ACKed on the swapped-role scan (0xFF=none) */
    uint8_t  bb_swap_count;  /* how many addresses ACKed on the swapped-role scan */
    uint8_t  cci_transport;  /* 0=HAL I2C4, 1=software bit-bang CCI fallback */
    uint8_t  booted;         /* STATUS boot-complete seen */
    uint8_t  last_i2c_ok;    /* 1 = last STATUS read returned HAL_OK (0x0000 is real) */
    uint16_t status_reg;     /* last STATUS read after boot */
    uint32_t boot_wait_ms;   /* how long we waited for boot */
    int8_t   tlinear_err;    /* CCI error code from TLinear enable (0=OK, <0 not run) */
    uint8_t  mclk_on;        /* MCLK output configured */
    uint32_t mclk_hz;        /* configured MCLK frequency, 0 if unknown */
    uint32_t sysclk_hz;      /* HAL_RCC_GetSysClockFreq() at Lepton init */
    uint32_t pclk2_hz;       /* APB2 clock, useful for MCLK/SPI timing debug */

    uint32_t vospi_reads;      /* packets read in the last capture attempt */
    uint32_t vospi_valid;      /* non-discard packets seen */
    uint32_t vospi_discard;    /* discard packets seen */
    uint32_t vospi_invalid;    /* ID[0] low nibble not 0/0xF, or packet >=60 */
    uint32_t vospi_pkt0;       /* packet-0 sync hits seen */
    uint32_t vospi_desync;     /* packet number mismatch count */
    uint32_t vospi_bad_seg;    /* packet-20 segment id outside 1..4 */
    uint32_t vospi_spi_err;    /* HAL_SPI_Receive failures */
    uint8_t  vospi_fail_reason;/* LEPTON_VOSPI_FAIL_* */
    uint8_t  vospi_last_expected; /* expected packet number when failing */
    uint8_t  vospi_last_id0;   /* last packet header byte 0 */
    uint8_t  vospi_last_id1;   /* last packet header byte 1 */
    uint8_t  vospi_last_crc0;  /* last packet header byte 2 */
    uint8_t  vospi_last_crc1;  /* last packet header byte 3 */
    uint8_t  vospi_last_seg;   /* last decoded segment id, 0xFF if none */
    uint8_t  vospi_got_mask;   /* bit0..3 = segment 1..4 currently cached in the assembly frame */
    uint8_t  vospi_dup_seg;    /* valid segment repeated after it was already cached */
    uint16_t vospi_seg_seen[LEPTON_SEG_CNT + 1]; /* packet-20 saw segment id 1..4 */
    uint16_t vospi_seg_ok[LEPTON_SEG_CNT + 1]; /* completed valid segments by id, index 1..4 */
    uint16_t vospi_seg_bad0;   /* packet-20 segment id decoded as 0 */
    uint16_t vospi_seg_badx;   /* packet-20 segment id decoded as >4 */
    uint16_t vospi_sync_waits; /* long waits after first discard while seeking packet 0 */
    uint16_t vospi_stale_block;/* same-frame assembly drops: segment-1 arrived mid-frame (lost segments -> frame discarded, not stitched) */
} Lepton_Diag_t;

extern Lepton_Diag_t lepton_diag;

/* ---- global frame buffer (raw 16-bit pixels) + scratch packet buffer ----- */
extern uint16_t lepton_raw_frame[LEPTON_IMG_HEIGHT][LEPTON_IMG_WIDTH];
extern uint8_t  lepton_spi_pkt[LEPTON_PACK_SIZE];

/* ---- API ----------------------------------------------------------------- */
void              Lepton_Init(SPI_HandleTypeDef *hspi, I2C_HandleTypeDef *hi2c);

/* CCI raw register access (transport layer) */
HAL_StatusTypeDef Lepton_I2C_Write_Reg(uint16_t reg, uint16_t data);
HAL_StatusTypeDef Lepton_I2C_Read_Reg (uint16_t reg, uint16_t *p_data);

/* CCI command layer */
Lepton_Status_t   Lepton_CCI_WaitBusy(uint32_t timeout_ms, int8_t *p_err);
Lepton_Status_t   Lepton_CCI_SetAttr (uint16_t cmd, const uint16_t *data, uint16_t nwords);
Lepton_Status_t   Lepton_CCI_GetAttr (uint16_t cmd, uint16_t *data, uint16_t nwords);
Lepton_Status_t   Lepton_CCI_RunCmd  (uint16_t cmd);

/* high-level control */
Lepton_Status_t   Lepton_WaitBoot(uint32_t timeout_ms);   /* poll STATUS boot-complete */
Lepton_Status_t   Lepton_EnableTLinear(uint8_t enable);   /* radiometric raw = Kelvin*100 */
Lepton_Status_t   Lepton_SetAGC(uint8_t enable);
Lepton_Status_t   Lepton_RunFFC(void);
void              Lepton_I2C_BusScan(void);               /* scan 0x08..0x77, fill diag */
void              Lepton_I2C_BitBang_Probe(void);          /* SOFTWARE I2C on PD12/13, bypasses I2C4 HW */

/* VoSPI capture */
HAL_StatusTypeDef Lepton_SPI_Read_Packet(uint8_t *p_buf);
void              Lepton_VoSPI_Resync(void);       /* force segment resync (CS high >185ms) */
uint8_t           Lepton_Capture_Frame(void);      /* 1 = full frame, 0 = failed/no sync */

float             Lepton_Raw_To_Temp(uint16_t raw_val);

/* ---- software chip-select helpers ---------------------------------------- */
static inline void LEPTON_CS_LOW (void){ HAL_GPIO_WritePin(LEPTON_CS_GPIO_PORT, LEPTON_CS_GPIO_PIN, GPIO_PIN_RESET); }
static inline void LEPTON_CS_HIGH(void){ HAL_GPIO_WritePin(LEPTON_CS_GPIO_PORT, LEPTON_CS_GPIO_PIN, GPIO_PIN_SET);   }

#endif /* __LEPTON_H */

#ifndef __LEPTON_STREAM_H
#define __LEPTON_STREAM_H

#include "main.h"
#include "lepton.h"

/* ---------------------------------------------------------------------------
 * Binary thermal-frame streamer for the Qt host (ESP_UART上位机/ESP_UART_Windows).
 *
 * Wire format expected by the host FrameParser (frameparser.h):
 *   [0xAA 0x55]   sync marker
 *   [0x01]        type: raw 16-bit thermal
 *   [u16 BE]      frame id (rolling counter)
 *   [u16 BE]      width  = 160
 *   [u16 BE]      height = 120
 *   [u32 BE]      payload length = width*height*2 = 38400
 *   [38400 bytes] pixels, u16 BE each (TLinear centikelvin, 0.01 K/LSB)
 *   [u16 BE]      checksum: 8-bit-wise sum of every byte from sync through payload
 *
 * Physical link (XIH6 v3 base board): CH340C on UART4 @ 1.5 Mbps
 * after the 2 Mbps field test exposed CH340C FIFO loss.
 * CH340C.TXD drives PA0 and CH340C.RXD listens on PA1, while the H743 muxes
 * PA0=UART4_TX / PA1=UART4_RX - CR2.SWAP crosses them back in silicon.
 *
 * Host commands:
 *   'S' / 'P'             = start / stop stream
 *   MODE_TOGGLE\n         = switch UART4 video <-> ESP32 TCP video
 *   MODE_UART4\n          = force UART4 video
 *   MODE_TCP\n            = force ESP32 TCP video
 * ------------------------------------------------------------------------- */

/* 2026-07-06 field verdict (README_6 §7): at 2 Mbps the CH340C loses ~7 bytes
 * per 38415-byte frame (serial_diag syncDelta median -7, never positive, zero
 * bad headers) — its tiny internal UART->USB FIFO (~32 B) overflows whenever
 * USB bulk-IN scheduling gaps exceed ~160 µs. Host software cannot fix that;
 * dropping to 1.5 Mbps (120 MHz kernel /80, 0% error) cuts the fill rate 25%.
 * Actual frame rate is unaffected: capture (~1.5 fps) is the bottleneck.
 * If loss persists, next steps: 1 Mbps (/120), or CH343P (1.5 KB FIFO). */
#define LEPTON_STREAM_BAUD       1500000UL

/* ---------------------------------------------------------------------------
 * Physical link selection (2026-07-08, ESP32 bridge bring-up):
 *   UART4 = CH340C USB serial (legacy, Qt 串口热成像 mode)
 *   SPI5  = ESP32-S3 bridge, STM32 as SPI SLAVE (Qt WiFi热成像 mode)
 *
 * SPI5 slave wiring (XIH6 base board, ESP32与STM32引脚连接.txt):
 *   PK0  = SPI5_SCK  (AF5)  <- ESP32 IO37 (master clock)
 *   PJ11 = SPI5_MISO (AF5)  -> ESP32 IO36 (frame data out)
 *   PH5  = SPI5_NSS  (AF5)  <- ESP32 IO47 (CS, active low, held low for the
 *          whole 38415-byte frame; high between frames = slave ignores SCK,
 *          so inter-frame clock glitches cannot shift the bit counter)
 *   PG3  = DRDY GPIO out    -> ESP32 IO39 (high = frame armed in TX DMA)
 *   NOTE PJ3(IO21)/PH6(IO48) are shared with LEPTON_RST/PWDN - never use.
 *
 * Handshake: SendFrame packs the same AA55 frame, arms SPI5 TX DMA, raises
 * DRDY. ESP32 sees DRDY high and clocks out exactly LEPTON_STREAM_FRAME_LEN
 * bytes @10MHz; TxCplt drops DRDY. If the master stops mid-frame (reboot,
 * WiFi stall) a 500ms timeout aborts the DMA and re-arms on the next frame.
 *
 * UART4 stays initialized in both builds for the command channel:
 *   'S'/'P' = stream start/stop,
 *   'T'/'U'/'W' or MODE_* lines = transport switch,
 *   'B'/'R' = ESP32 IO0 (PJ7, open-drain) manual override: 'B' = drive LOW
 *             (download mode), 'R' = release (high-Z, pull-up -> normal
 *             boot). Default sequencing is automatic, see below.
 *
 * ESP32 IO0 power-on sequencing (user requirement): PJ7 held LOW for the
 * first 30 s after power-on (ESP32 waits in download mode - flash window),
 * then released HIGH permanently. Lepton_Stream_Poll() (called from the
 * main super-loop) does the timed release; a manual 'B'/'R' disables the
 * automatic logic for the rest of the session.
 * ------------------------------------------------------------------------- */
/* Transport IDs for the RUNTIME selector (stream_link in lepton_stream.c).
   Selection is runtime-only: power-on default UART4, manual via 'T'/'U'/'W'
   or MODE_* commands. */
#define LEPTON_STREAM_LINK_UART4 0
#define LEPTON_STREAM_LINK_SPI5  1

/* 1 = auto-switch to SPI5/TCP on the ESP32 ready line (PG2) rising edge.
   0 (bring-up default) = UART4 stays the transport until an explicit
   command switches it; PG2 is still read and reported, just not acted on. */
#define LEPTON_STREAM_AUTO_TCP   0U

/* Slave-side DMA timeout: master must finish clocking a frame within this. */
#define LEPTON_STREAM_SPI_TIMEOUT_MS 500U

/* ESP32 IO0 (PJ7) flash window: LOW from power-on until this, then released. */
#define LEPTON_STREAM_ESP32_BOOT_LOW_MS 30000U

void              Lepton_Stream_Poll(void);

#define LEPTON_STREAM_HDR_LEN    13U         /* sync(2)+type(1)+id(2)+w(2)+h(2)+len(4) */
#define LEPTON_STREAM_PAYLOAD    ((uint32_t)LEPTON_IMG_WIDTH * LEPTON_IMG_HEIGHT * 2U)
#define LEPTON_STREAM_FRAME_LEN  (LEPTON_STREAM_HDR_LEN + LEPTON_STREAM_PAYLOAD + 2U)

void              Lepton_Stream_Init(UART_HandleTypeDef *huart);
HAL_StatusTypeDef Lepton_Stream_SendFrame(void);
uint8_t           Lepton_Stream_Active(void);
uint16_t          Lepton_Stream_FrameCount(void);

#endif /* __LEPTON_STREAM_H */

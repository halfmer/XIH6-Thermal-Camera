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
 * Host commands (single byte): 'S' = start stream, 'P' = stop (back to logs).
 * ------------------------------------------------------------------------- */

/* 2026-07-06 field verdict (README_6 §7): at 2 Mbps the CH340C loses ~7 bytes
 * per 38415-byte frame (serial_diag syncDelta median -7, never positive, zero
 * bad headers) — its tiny internal UART->USB FIFO (~32 B) overflows whenever
 * USB bulk-IN scheduling gaps exceed ~160 µs. Host software cannot fix that;
 * dropping to 1.5 Mbps (120 MHz kernel /80, 0% error) cuts the fill rate 25%.
 * Actual frame rate is unaffected: capture (~1.5 fps) is the bottleneck.
 * If loss persists, next steps: 1 Mbps (/120), or CH343P (1.5 KB FIFO). */
#define LEPTON_STREAM_BAUD       1500000UL

#define LEPTON_STREAM_HDR_LEN    13U         /* sync(2)+type(1)+id(2)+w(2)+h(2)+len(4) */
#define LEPTON_STREAM_PAYLOAD    ((uint32_t)LEPTON_IMG_WIDTH * LEPTON_IMG_HEIGHT * 2U)
#define LEPTON_STREAM_FRAME_LEN  (LEPTON_STREAM_HDR_LEN + LEPTON_STREAM_PAYLOAD + 2U)

void              Lepton_Stream_Init(UART_HandleTypeDef *huart);
HAL_StatusTypeDef Lepton_Stream_SendFrame(void);
uint8_t           Lepton_Stream_Active(void);
uint16_t          Lepton_Stream_FrameCount(void);

#endif /* __LEPTON_STREAM_H */

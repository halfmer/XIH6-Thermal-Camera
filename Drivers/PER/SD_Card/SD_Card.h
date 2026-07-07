#ifndef __SD_CARD_H
#define __SD_CARD_H

#include "main.h"

/* SD_CD detect pin (PG10).
 * On this board SD_CD is pulled up to 3V3 by RN2 (10k); the card-socket CD
 * switch shorts it to GND when a card is inserted.
 *   inserted  -> PG10 = LOW  (GPIO_PIN_RESET)
 *   removed   -> PG10 = HIGH (GPIO_PIN_SET)
 */
#define SD_CD_PORT          GPIOG
#define SD_CD_PIN           GPIO_PIN_10

/* status codes */
#define SD_OK               0
#define SD_ERROR            1
#define SD_NOT_PRESENT      2
#define SD_TIMEOUT          3

/* init diagnostics (set by SD_Card_Init on failure; see SD_Card.c) */
extern uint32_t sd_dbg_kerclk;
extern uint32_t sd_dbg_errorcode;
extern uint8_t  sd_dbg_stage;
extern uint8_t  sd_dbg_cardver;

/* API */
uint8_t SD_IsPresent(void);
uint8_t SD_Card_Init(void);
uint8_t SD_ReadDisk_DMA(uint8_t *pBuffer, uint32_t SectorAddr, uint32_t NumberOfSectors);
uint8_t SD_WriteDisk_DMA(const uint8_t *pBuffer, uint32_t SectorAddr, uint32_t NumberOfSectors);
void    SD_GetCardInfo(HAL_SD_CardInfoTypeDef *CardInfo);

#endif /* __SD_CARD_H */

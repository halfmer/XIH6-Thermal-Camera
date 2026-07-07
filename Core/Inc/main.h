/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define BEEP_GPIO_Pin GPIO_PIN_9
#define BEEP_GPIO_GPIO_Port GPIOG
#define LED_GPIO_Pin GPIO_PIN_15
#define LED_GPIO_GPIO_Port GPIOA
#define LED_CONTROL_Pin GPIO_PIN_15
#define LED_CONTROL_GPIO_Port GPIOJ
#define SPI_SSEL_Pin GPIO_PIN_4
#define SPI_SSEL_GPIO_Port GPIOE
#define LEPTON_CLK_Pin GPIO_PIN_8
#define LEPTON_CLK_GPIO_Port GPIOA
#define ESP_IO39_Pin GPIO_PIN_3
#define ESP_IO39_GPIO_Port GPIOG
#define ESP_IO38_Pin GPIO_PIN_2
#define ESP_IO38_GPIO_Port GPIOG
#define TP_SCK_Pin GPIO_PIN_7
#define TP_SCK_GPIO_Port GPIOF
#define TP_MISO_Pin GPIO_PIN_8
#define TP_MISO_GPIO_Port GPIOF
#define TP_MOSI_Pin GPIO_PIN_9
#define TP_MOSI_GPIO_Port GPIOF
#define TP_PEN_Pin GPIO_PIN_1
#define TP_PEN_GPIO_Port GPIOC
#define ESP_IO35_Pin GPIO_PIN_8
#define ESP_IO35_GPIO_Port GPIOJ
#define ESP_IO0_Pin GPIO_PIN_7
#define ESP_IO0_GPIO_Port GPIOJ
#define TP_CS_Pin GPIO_PIN_4
#define TP_CS_GPIO_Port GPIOH
#define LEPTON_VSYNC_Pin GPIO_PIN_2
#define LEPTON_VSYNC_GPIO_Port GPIOB
#define LEPTON_PWDN_Pin GPIO_PIN_6
#define LEPTON_PWDN_GPIO_Port GPIOH
#define ESP_TX_Pin GPIO_PIN_12
#define ESP_TX_GPIO_Port GPIOB
#define LEPTON_RST_Pin GPIO_PIN_3
#define LEPTON_RST_GPIO_Port GPIOJ
#define ESP_RX_Pin GPIO_PIN_13
#define ESP_RX_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */

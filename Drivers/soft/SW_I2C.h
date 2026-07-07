#ifndef __SW_I2C_H
#define __SW_I2C_H

#include "main.h" // 包含 HAL 库和底层宏定义

/* ================== 引脚宏定义 ================== */
#define SW_I2C_SCL_PORT     GPIOD
#define SW_I2C_SCL_PIN      GPIO_PIN_3

#define SW_I2C_SDA_PORT     GPIOD
#define SW_I2C_SDA_PIN      GPIO_PIN_4

/* ================== 硬件操作宏 ================== */
// 设置 SCL 输出高/低
#define SCL_H()             HAL_GPIO_WritePin(SW_I2C_SCL_PORT, SW_I2C_SCL_PIN, GPIO_PIN_SET)
#define SCL_L()             HAL_GPIO_WritePin(SW_I2C_SCL_PORT, SW_I2C_SCL_PIN, GPIO_PIN_RESET)

// 设置 SDA 输出高/低
// 注意：由于是开漏模式，SDA_H() 同时也相当于释放总线，使引脚变为输入态
#define SDA_H()             HAL_GPIO_WritePin(SW_I2C_SDA_PORT, SW_I2C_SDA_PIN, GPIO_PIN_SET)
#define SDA_L()             HAL_GPIO_WritePin(SW_I2C_SDA_PORT, SW_I2C_SDA_PIN, GPIO_PIN_RESET)

// 读取 SDA 当前电平
#define SDA_READ()          HAL_GPIO_ReadPin(SW_I2C_SDA_PORT, SW_I2C_SDA_PIN)

/* ================== API 函数声明 ================== */
void SW_I2C_Init(void);
void SW_I2C_Start(void);
void SW_I2C_Stop(void);
uint8_t SW_I2C_WaitAck(void);
void SW_I2C_Ack(void);
void SW_I2C_NAck(void);
void SW_I2C_SendByte(uint8_t byte);
uint8_t SW_I2C_ReadByte(uint8_t ack_mode);

#endif /* __SW_I2C_H */
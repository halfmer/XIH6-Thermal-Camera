#ifndef __SHT40_H
#define __SHT40_H

#include "main.h"
#include "sw_i2c.h"   // 引入软件I2C底层驱动
#include "stdio.h"
#include "DELAY.h"
/* ==================================================================== */
/*                           API 函数声明                               */
/* ==================================================================== */

/**
  * @brief  读取 SHT40 温湿度数据 (基于软件 I2C，含 CRC 校验)
  * @param  temp: 指向温度浮点变量的指针 (用于接收温度结果，单位: 摄氏度)
  * @param  hum:  指向湿度浮点变量的指针 (用于接收湿度结果，单位: %RH)
  * @retval 传感器状态码: 
  *         0: 读取成功且数据有效
  *         1: 总线写入失败 (传感器未连接或寻址无应答)
  *         2: 总线读取失败
  *         3: CRC 校验不通过 (数据在传输过程中受到干扰)
  */
uint8_t sht40_read_data(float *temp, float *hum);

/**
  * @brief  SHT40 专用的 CRC8 校验函数 (多项式: 0x31)
  * @note   通常不需要在外部直接调用此函数，已在 sht40_read_data 内部自动处理
  * @param  data: 待校验的数据数组指针
  * @param  len:  待校验的数据长度
  * @retval 计算得到的 CRC8 校验码
  */
uint8_t sht40_crc8(const uint8_t *data, int len);

#endif /* __SHT40_H */
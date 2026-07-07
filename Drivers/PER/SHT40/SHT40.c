#include "SHT40.h"

// SHT40 7位地址为 0x44。软件I2C需要完整的8位地址（包含读写位）
#define SHT40_ADDR_WRITE  (0x44 << 1) | 0x00  // 0x88 (写指令)
#define SHT40_ADDR_READ   (0x44 << 1) | 0x01  // 0x89 (读指令)

#define SHT40_CMD_HIGH_PRECISION 0xFD

/**
  * @brief  SHT40 专用的 CRC8 校验函数
  * @param  data: 待校验的数组指针
  * @param  len: 待校验的数据长度
  * @retval 计算出的 CRC8 结果
  */
uint8_t sht40_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF; // 初始值
    for (int i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++)
        {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1); // 多项式 0x31
        }
    }
    return crc;
}

/**
  * @brief  读取 SHT40 温湿度数据 (基于软件 I2C，含 CRC 校验)
  * @param  temp: 指向温度浮点数的指针 (输出)
  * @param  hum: 指向湿度浮点数的指针 (输出)
  * @retval 状态码: 0=成功, 1=写入失败, 2=读取失败, 3=CRC校验失败
  */
uint8_t sht40_read_data(float *temp, float *hum)
{
    uint8_t data[6];
    uint16_t raw_temp, raw_hum;

    /* ================= 第一阶段：发送测量命令 ================= */
    SW_I2C_Start();
    SW_I2C_SendByte(SHT40_ADDR_WRITE);  // 发送写地址
    if (SW_I2C_WaitAck() != 0)          // 等待传感器应答
    {
        SW_I2C_Stop();
        return 1; // 寻址失败
    }
    
    SW_I2C_SendByte(SHT40_CMD_HIGH_PRECISION); // 发送高精度测量命令
    if (SW_I2C_WaitAck() != 0)
    {
        SW_I2C_Stop();
        return 1; // 发送命令失败
    }
    SW_I2C_Stop();

    /* ================= 第二阶段：等待传感器完成测量 ================= */
    delay_ms(10); // 官方建议高精度模式 ≥ 9ms

    /* ================= 第三阶段：读取6字节数据 ================= */
    SW_I2C_Start();
    SW_I2C_SendByte(SHT40_ADDR_READ);   // 发送读地址
    if (SW_I2C_WaitAck() != 0)
    {
        SW_I2C_Stop();
        return 2; // 读取阶段寻址失败
    }

    // 依序读取数据并发送应答 (最后一次必须发送 NACK)
    data[0] = SW_I2C_ReadByte(1); // 温度 MSB  (发送 ACK)
    data[1] = SW_I2C_ReadByte(1); // 温度 LSB  (发送 ACK)
    data[2] = SW_I2C_ReadByte(1); // 温度 CRC  (发送 ACK)
    data[3] = SW_I2C_ReadByte(1); // 湿度 MSB  (发送 ACK)
    data[4] = SW_I2C_ReadByte(1); // 湿度 LSB  (发送 ACK)
    data[5] = SW_I2C_ReadByte(0); // 湿度 CRC  (发送 NACK，告知从机停止发送)
    SW_I2C_Stop();

    /* ================= 第四阶段：CRC 校验 ================= */
    // 校验温度数据 (前两个字节) 是否匹配第3个字节
    if (sht40_crc8(&data[0], 2) != data[2]) 
    {
        return 3; // 温度 CRC 失败
    }
    // 校验湿度数据 (第4、5个字节) 是否匹配第6个字节
    if (sht40_crc8(&data[3], 2) != data[5]) 
    {
        return 3; // 湿度 CRC 失败
    }

    /* ================= 第五阶段：数据转换 ================= */
    raw_temp = (data[0] << 8) | data[1];
    raw_hum  = (data[3] << 8) | data[4];

    // 转换公式来自 SHT40 数据手册
    *temp = -45.0f + 175.0f * ((float)raw_temp / 65535.0f);
    *hum  = -6.0f + 125.0f * ((float)raw_hum / 65535.0f);

    return 0; // 成功
}
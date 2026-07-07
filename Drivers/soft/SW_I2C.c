#include "sw_i2c.h"



// 修改 oled.c 中的延时函数
void SW_I2C_Delay(void)
{
	// 1. 必须强制声明为 volatile，禁止编译器优化掉此循环
	// 2. 480MHz 主频下，循环 500 次大约对应 2 微秒左右的延时
	volatile uint32_t t = 10; 
	while(t--);
}

/**
  * @brief  初始化 I2C 总线 (释放总线)
  * @note   GPIO 初始化已在 CubeMX 的 MX_GPIO_Init() 中完成，此处仅做电平复位
  */
void SW_I2C_Init(void)
{
    SCL_H();
    SDA_H();
    SW_I2C_Delay();
}

/**
  * @brief  产生 I2C 起始信号
  * @note   当 SCL 为高电平时，SDA 由高变低
  */
void SW_I2C_Start(void)
{
    SDA_H();
    SCL_H();
    SW_I2C_Delay();
    SDA_L();  // START: when SCL is high, SDA change form high to low
    SW_I2C_Delay();
    SCL_L();  // 钳住I2C总线，准备发送或接收数据
}

/**
  * @brief  产生 I2C 停止信号
  * @note   当 SCL 为高电平时，SDA 由低变高
  */
void SW_I2C_Stop(void)
{
    SCL_L();
    SDA_L();  // STOP: when SCL is high, SDA change form low to high
    SW_I2C_Delay();
    SCL_H();
    SW_I2C_Delay();
    SDA_H();  // 发送I2C总线结束信号
    SW_I2C_Delay();
}

/**
  * @brief  等待应答信号到来
  * @retval 0: 接收应答成功 (ACK)
  *         1: 接收应答失败 (NACK)
  */
uint8_t SW_I2C_WaitAck(void)
{
    uint8_t timeout_cnt = 0;
    
    SDA_H();          // 主机释放 SDA 线，交由从机控制
    SW_I2C_Delay();
    SCL_H();          // 拉高时钟线，准备读取
    SW_I2C_Delay();
    
    // 等待从机将 SDA 拉低 (带超时机制，防止死机)
    while(SDA_READ() != 0)
    {
        timeout_cnt++;
        if(timeout_cnt > 250)
        {
            SW_I2C_Stop();
            return 1; // 超时，无应答
        }
    }
    
    SCL_L();          // 时钟线拉低，完成一个时钟周期
    return 0;         // 成功接收到应答
}

/**
  * @brief  产生 ACK 应答
  */
void SW_I2C_Ack(void)
{
    SCL_L();
    SDA_L();          // 主机将 SDA 拉低
    SW_I2C_Delay();
    SCL_H();          // 产生一个时钟脉冲
    SW_I2C_Delay();
    SCL_L();
}

/**
  * @brief  产生 NACK 不应答
  */
void SW_I2C_NAck(void)
{
    SCL_L();
    SDA_H();          // 主机不拉低 SDA
    SW_I2C_Delay();
    SCL_H();          // 产生一个时钟脉冲
    SW_I2C_Delay();
    SCL_L();
}

/**
  * @brief  I2C 发送一个字节
  * @param  byte: 待发送的数据
  */
void SW_I2C_SendByte(uint8_t byte)
{
    uint8_t i;
    SCL_L();          // 拉低时钟开始数据传输
    
    for(i = 0; i < 8; i++)
    {
        // 准备数据位 (高位先发 MSB)
        if((byte & 0x80) != 0)
        {
            SDA_H();
        }
        else
        {
            SDA_L();
        }
        byte <<= 1;
        
        SW_I2C_Delay();
        SCL_H();      // 拉高 SCL，从机此时会在上升沿采样 SDA 信号
        SW_I2C_Delay();
        SCL_L();      // 拉低 SCL，准备发送下一位
    }
}

/**
  * @brief  I2C 读取一个字节
  * @param  ack_mode: 1 发送 ACK, 0 发送 NACK
  * @retval 读取到的数据
  */
uint8_t SW_I2C_ReadByte(uint8_t ack_mode)
{
    uint8_t i, receive_byte = 0;
    
    SDA_H();          // 必须先释放 SDA 总线，以便从机控制
    
    for(i = 0; i < 8; i++)
    {
        SCL_L();
        SW_I2C_Delay();
        SCL_H();      // 拉高 SCL
        receive_byte <<= 1;
        
        // 读取 SDA 电平
        if(SDA_READ() == GPIO_PIN_SET)
        {
            receive_byte |= 0x01;
        }
        SW_I2C_Delay();
    }
    
    // 接收完 8 bit 后，发送应答或不应答信号
    if(ack_mode == 1)
    {
        SW_I2C_Ack();
    }
    else
    {
        SW_I2C_NAck();
    }
    
    return receive_byte;
}
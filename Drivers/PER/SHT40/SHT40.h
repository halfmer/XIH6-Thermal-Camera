#ifndef __SHT40_H
#define __SHT40_H

#include "main.h"
#include "sw_i2c.h"   // ��������I2C�ײ�����
#include "stdio.h"
#include "DELAY.h"
/* ==================================================================== */
/*                           API ��������                               */
/* ==================================================================== */

/**
  * @brief  ��ȡ SHT40 ��ʪ������ (�������� I2C���� CRC У��)
  * @param  temp: ָ���¶ȸ��������ָ�� (���ڽ����¶Ƚ������λ: ���϶�)
  * @param  hum:  ָ��ʪ�ȸ��������ָ�� (���ڽ���ʪ�Ƚ������λ: %RH)
  * @retval ������״̬��: 
  *         0: ��ȡ�ɹ���������Ч
  *         1: ����д��ʧ�� (������δ���ӻ�Ѱַ��Ӧ��)
  *         2: ���߶�ȡʧ��
  *         3: CRC У�鲻ͨ�� (�����ڴ���������ܵ�����)
  */
uint8_t sht40_read_data(float *temp, float *hum);

/* Non-blocking two-phase read. Call from the super-loop; returns 0xFF while
   the conversion is in progress (call again), 0 on done, 1/2/3 on error. */
uint8_t sht40_poll(float *temp, float *hum);
uint8_t sht40_request(float *temp, float *hum);

/**
  * @brief  SHT40 ר�õ� CRC8 У�麯�� (����ʽ: 0x31)
  * @note   ͨ������Ҫ���ⲿֱ�ӵ��ô˺��������� sht40_read_data �ڲ��Զ�����
  * @param  data: ��У�����������ָ��
  * @param  len:  ��У������ݳ���
  * @retval ����õ��� CRC8 У����
  */
uint8_t sht40_crc8(const uint8_t *data, int len);

#endif /* __SHT40_H */
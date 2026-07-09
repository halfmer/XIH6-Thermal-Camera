#include "SHT40.h"

// SHT40 7λ��ַΪ 0x44������I2C��Ҫ������8λ��ַ��������дλ��
#define SHT40_ADDR_WRITE  (0x44 << 1) | 0x00  // 0x88 (дָ��)
#define SHT40_ADDR_READ   (0x44 << 1) | 0x01  // 0x89 (��ָ��)

#define SHT40_CMD_HIGH_PRECISION 0xFD

/* ---- non-blocking two-phase read (README_14 sec.9) ------------------------
   The old sht40_read_data() blocks 10ms for the conversion wait; called from
   the super-loop that delay starved VoSPI frame-acquisition restart and caused
   segment-loss / torn-frame surges. sht40_poll() splits the wait out:
     IDLE -> send measure cmd (~1ms SW_I2C), -> BUSY, stamp tick
     BUSY -> return SHT40_BUSY until >=10ms elapsed, then read 6B + CRC -> IDLE
   The 10ms wait is spread across ~90 loop iterations instead of blocking one. */
#define SHT40_BUSY       0xFF
#define SHT40_CONV_MS    10U

typedef enum { SHT40_IDLE = 0, SHT40_BUSY_S = 1 } sht40_phase_t;
static sht40_phase_t sht40_phase   = SHT40_IDLE;
static uint32_t      sht40_cmd_tick = 0;
static float        *sht40_p_temp  = NULL;
static float        *sht40_p_hum   = NULL;

/* Start a measurement (phase IDLE -> BUSY). Returns 1 on I2C write failure. */
uint8_t sht40_request(float *temp, float *hum)
{
    if (sht40_phase != SHT40_IDLE)
        return 0xFF;   /* already busy */

    sht40_p_temp = temp;
    sht40_p_hum  = hum;

    SW_I2C_Start();
    SW_I2C_SendByte(SHT40_ADDR_WRITE);
    if (SW_I2C_WaitAck() != 0) { SW_I2C_Stop(); return 1; }
    SW_I2C_SendByte(SHT40_CMD_HIGH_PRECISION);
    if (SW_I2C_WaitAck() != 0) { SW_I2C_Stop(); return 1; }
    SW_I2C_Stop();

    sht40_cmd_tick = HAL_GetTick();
    sht40_phase = SHT40_BUSY_S;
    return 0;
}

/* Poll a started measurement. Returns SHT40_BUSY while converting, 0 on
   done, 1/2/3 on error. Safe to call every loop iteration. */
uint8_t sht40_poll(float *temp, float *hum)
{
    (void)temp; (void)hum;

    if (sht40_phase == SHT40_IDLE)
        return 0xFE;   /* nothing in flight */

    if ((HAL_GetTick() - sht40_cmd_tick) < SHT40_CONV_MS)
        return SHT40_BUSY;

    {
        uint8_t  data[6];
        uint16_t raw_temp, raw_hum;

        SW_I2C_Start();
        SW_I2C_SendByte(SHT40_ADDR_READ);
        if (SW_I2C_WaitAck() != 0) { SW_I2C_Stop(); sht40_phase = SHT40_IDLE; return 2; }

        data[0] = SW_I2C_ReadByte(1);
        data[1] = SW_I2C_ReadByte(1);
        data[2] = SW_I2C_ReadByte(1);
        data[3] = SW_I2C_ReadByte(1);
        data[4] = SW_I2C_ReadByte(1);
        data[5] = SW_I2C_ReadByte(0);
        SW_I2C_Stop();

        sht40_phase = SHT40_IDLE;

        if (sht40_crc8(&data[0], 2) != data[2]) return 3;
        if (sht40_crc8(&data[3], 2) != data[5]) return 3;

        raw_temp = (data[0] << 8) | data[1];
        raw_hum  = (data[3] << 8) | data[4];
        if (sht40_p_temp) *sht40_p_temp = -45.0f + 175.0f * ((float)raw_temp / 65535.0f);
        if (sht40_p_hum)  *sht40_p_hum  = -6.0f + 125.0f * ((float)raw_hum / 65535.0f);
        return 0;
    }
}

/**
  * @brief  SHT40 ר�õ� CRC8 У�麯��
  * @param  data: ��У�������ָ��
  * @param  len: ��У������ݳ���
  * @retval ������� CRC8 ���
  */
uint8_t sht40_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF; // ��ʼֵ
    for (int i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++)
        {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1); // ����ʽ 0x31
        }
    }
    return crc;
}

/**
  * @brief  ��ȡ SHT40 ��ʪ������ (�������� I2C���� CRC У��)
  * @param  temp: ָ���¶ȸ�������ָ�� (���)
  * @param  hum: ָ��ʪ�ȸ�������ָ�� (���)
  * @retval ״̬��: 0=�ɹ�, 1=д��ʧ��, 2=��ȡʧ��, 3=CRCУ��ʧ��
  */
uint8_t sht40_read_data(float *temp, float *hum)
{
    uint8_t data[6];
    uint16_t raw_temp, raw_hum;

    /* ================= ��һ�׶Σ����Ͳ������� ================= */
    SW_I2C_Start();
    SW_I2C_SendByte(SHT40_ADDR_WRITE);  // ����д��ַ
    if (SW_I2C_WaitAck() != 0)          // �ȴ�������Ӧ��
    {
        SW_I2C_Stop();
        return 1; // Ѱַʧ��
    }
    
    SW_I2C_SendByte(SHT40_CMD_HIGH_PRECISION); // ���͸߾��Ȳ�������
    if (SW_I2C_WaitAck() != 0)
    {
        SW_I2C_Stop();
        return 1; // ��������ʧ��
    }
    SW_I2C_Stop();

    /* ================= �ڶ��׶Σ��ȴ���������ɲ��� ================= */
    delay_ms(10); // �ٷ�����߾���ģʽ �� 9ms

    /* ================= �����׶Σ���ȡ6�ֽ����� ================= */
    SW_I2C_Start();
    SW_I2C_SendByte(SHT40_ADDR_READ);   // ���Ͷ���ַ
    if (SW_I2C_WaitAck() != 0)
    {
        SW_I2C_Stop();
        return 2; // ��ȡ�׶�Ѱַʧ��
    }

    // �����ȡ���ݲ�����Ӧ�� (���һ�α��뷢�� NACK)
    data[0] = SW_I2C_ReadByte(1); // �¶� MSB  (���� ACK)
    data[1] = SW_I2C_ReadByte(1); // �¶� LSB  (���� ACK)
    data[2] = SW_I2C_ReadByte(1); // �¶� CRC  (���� ACK)
    data[3] = SW_I2C_ReadByte(1); // ʪ�� MSB  (���� ACK)
    data[4] = SW_I2C_ReadByte(1); // ʪ�� LSB  (���� ACK)
    data[5] = SW_I2C_ReadByte(0); // ʪ�� CRC  (���� NACK����֪�ӻ�ֹͣ����)
    SW_I2C_Stop();

    /* ================= ���Ľ׶Σ�CRC У�� ================= */
    // У���¶����� (ǰ�����ֽ�) �Ƿ�ƥ���3���ֽ�
    if (sht40_crc8(&data[0], 2) != data[2]) 
    {
        return 3; // �¶� CRC ʧ��
    }
    // У��ʪ������ (��4��5���ֽ�) �Ƿ�ƥ���6���ֽ�
    if (sht40_crc8(&data[3], 2) != data[5]) 
    {
        return 3; // ʪ�� CRC ʧ��
    }

    /* ================= ����׶Σ�����ת�� ================= */
    raw_temp = (data[0] << 8) | data[1];
    raw_hum  = (data[3] << 8) | data[4];

    // ת����ʽ���� SHT40 �����ֲ�
    *temp = -45.0f + 175.0f * ((float)raw_temp / 65535.0f);
    *hum  = -6.0f + 125.0f * ((float)raw_hum / 65535.0f);

    return 0; // �ɹ�
}
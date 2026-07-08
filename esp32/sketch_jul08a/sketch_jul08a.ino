/* ============================================================================
 * XIH6 LEPTON WiFi bridge  (ESP32-S3, Arduino / built-in FreeRTOS)
 *
 * Role per AGENTS.md: pure network bridge - SPI in, TCP out, no image work.
 *
 * Link to STM32H743 (STM32 = SPI5 SLAVE, see lepton_stream.h):
 *   IO37 -> PK0  SPI SCK   (we are the master and own the clock)
 *   IO36 <- PJ11 SPI MISO  (frame data from STM32)
 *   IO47 -> PH5  SPI CS    (active low, held low for the WHOLE frame;
 *                           high between frames = slave NSS gate shuts,
 *                           stray clock edges cannot desync the slave)
 *   IO39 <- PG3  DRDY      (high = a full AA55 frame armed in slave TX DMA)
 *   IO38 -> PG2  READY     (high = WiFi/TCP connected, STM32 may prefer TCP)
 *   IO21/IO48 shared with Lepton RST/PWDN on the STM32 - never drive.
 *   IO0 is our own boot strap (STM32 PJ7 drives it, 'B'/'R' on the CH340).
 *
 * Frame format (identical to the UART link - Qt/Android decode unchanged):
 *   AA 55 | 01 | fid u16 | w u16 | h u16 | len u32 | 38400B RAW16 | sum u16
 *   total = 13 + 38400 + 2 = 38415 bytes
 *
 * Architecture: producer-consumer, so a WiFi/TCP stall can never block the
 * SPI side (the STM32 keeps streaming; at worst whole frames are dropped):
 *   spiTask (core 1, high prio) : DRDY rising-edge ISR -> task notify ->
 *                                 CS low -> clock 38415B @10MHz -> CS high ->
 *                                 checksum -> push buffer to full queue
 *   tcpTask (core 0)            : pop buffer -> write whole frame to Qt
 *                                 (WiFi热成像 server, PC_IP:8888) -> recycle
 *   3 frame buffers travel between a free queue and a full queue; when the
 *   full queue backs up, the OLDEST frame is recycled (freshness wins).
 *   loop() only prints 1 Hz diagnostics on the USB console.
 * ==========================================================================*/

#include <WiFi.h>
#include <SPI.h>

/* ---- user config ---------------------------------------------------------*/
#define WIFI_SSID   "ESP32_TEST"
#define WIFI_PASS   "TEST123456"
#define SERVER_IP   "192.168.1.100"   /* PC running the Qt host, WiFi热成像 */
#define SERVER_PORT 8888

/* ---- pins / link ----------------------------------------------------------*/
#define PIN_SCK   37
#define PIN_MISO  36
#define PIN_CS    47                  /* soft-driven, STM32 SPI5 hard NSS */
#define PIN_DRDY  39
#define PIN_READY 38                  /* to STM32 PG2: TCP path ready */
#define SPI_HZ    10000000UL          /* 10 MHz: 38415 B in ~31 ms */

/* ---- frame geometry (must match lepton_stream.h) --------------------------*/
#define FRAME_HDR   13UL
#define FRAME_PAYLD (160UL * 120UL * 2UL)               /* 38400 */
#define FRAME_LEN   (FRAME_HDR + FRAME_PAYLD + 2UL)     /* 38415 */
#define SPI_CHUNK   4096UL            /* transferBytes slice size */
#define BUF_COUNT   3                 /* ~113 KB of internal SRAM */

static SPIClass     spi(FSPI);
static WiFiClient   tcp;
static uint8_t      bufs[BUF_COUNT][FRAME_LEN];
static QueueHandle_t q_free;          /* uint8_t* buffers ready for SPI fill */
static QueueHandle_t q_full;          /* uint8_t* verified frames for TCP    */
static TaskHandle_t  h_spi_task;

/* diagnostics (single writer per field, read by loop) */
static volatile uint32_t d_frames_ok = 0, d_crc_bad = 0, d_hdr_bad = 0;
static volatile uint32_t d_tcp_sent = 0, d_stale_drop = 0, d_tcp_fail = 0;
static volatile uint32_t d_drdy_timeout = 0;

/* ---- DRDY: rising-edge interrupt -> wake the SPI task --------------------*/
static void IRAM_ATTR drdyIsr(void)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(h_spi_task, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ---- producer: SPI reader -------------------------------------------------*/
static void spiTask(void *arg)
{
    (void)arg;
    for (;;)
    {
        /* Wait for DRDY. Level-check first so a notification missed while we
           were busy (or an edge that fired before boot) can never strand a
           frame: if DRDY is already high there IS a frame armed - just read
           it. CS framing makes this safe even after our own reboot; a torn
           read fails the checksum once and both sides self-heal (STM32 has
           a 500 ms abort on its slave DMA). */
        if (digitalRead(PIN_DRDY) == LOW)
        {
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0)
            {
                d_drdy_timeout++;          /* no frame within 1 s (idle mode?) */
                continue;
            }
            if (digitalRead(PIN_DRDY) == LOW)
                continue;                  /* spurious/raced edge */
        }

        uint8_t *buf = NULL;
        if (xQueueReceive(q_free, &buf, 0) != pdTRUE)
        {
            /* consumer is behind: recycle the OLDEST queued frame */
            uint8_t *old = NULL;
            if (xQueueReceive(q_full, &old, 0) == pdTRUE) { buf = old; d_stale_drop++; }
        }
        if (buf == NULL) { vTaskDelay(1); continue; }

        digitalWrite(PIN_CS, LOW);
        spi.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
        for (uint32_t off = 0; off < FRAME_LEN; off += SPI_CHUNK)
        {
            uint32_t n = FRAME_LEN - off;
            if (n > SPI_CHUNK) n = SPI_CHUNK;
            spi.transferBytes(NULL, buf + off, n);
        }
        spi.endTransaction();
        digitalWrite(PIN_CS, HIGH);

        /* verify before a single byte may travel further */
        bool ok = (buf[0] == 0xAA && buf[1] == 0x55 && buf[2] == 0x01);
        if (ok)
        {
            uint32_t plen = ((uint32_t)buf[9] << 24) | ((uint32_t)buf[10] << 16) |
                            ((uint32_t)buf[11] << 8) | buf[12];
            if (plen != FRAME_PAYLD) { ok = false; d_hdr_bad++; }
        }
        else d_hdr_bad++;
        if (ok)
        {
            uint16_t sum = 0;
            for (uint32_t i = 0; i < FRAME_HDR + FRAME_PAYLD; i++) sum += buf[i];
            uint16_t rx = ((uint16_t)buf[FRAME_LEN - 2] << 8) | buf[FRAME_LEN - 1];
            if (sum != rx) { ok = false; d_crc_bad++; }
        }

        if (ok) { d_frames_ok++; xQueueSend(q_full, &buf, 0); }
        else    {                xQueueSend(q_free, &buf, 0); }
    }
}

/* ---- consumer: WiFi/TCP sender --------------------------------------------*/
static void tcpTask(void *arg)
{
    (void)arg;
    for (;;)
    {
        if (WiFi.status() != WL_CONNECTED)
        {
            digitalWrite(PIN_READY, LOW);
            WiFi.mode(WIFI_STA);
            WiFi.begin(WIFI_SSID, WIFI_PASS);
            for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++)
                vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (!tcp.connected())
        {
            tcp.stop();
            if (!tcp.connect(SERVER_IP, SERVER_PORT, 1000))
            {
                digitalWrite(PIN_READY, LOW);
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            tcp.setNoDelay(true);
            digitalWrite(PIN_READY, HIGH);
        }

        uint8_t *buf = NULL;
        if (xQueueReceive(q_full, &buf, pdMS_TO_TICKS(200)) != pdTRUE)
            continue;

        size_t w = tcp.write(buf, FRAME_LEN);      /* whole frame or bust */
        if (w == FRAME_LEN) d_tcp_sent++;
        else { d_tcp_fail++; digitalWrite(PIN_READY, LOW); tcp.stop(); }

        xQueueSend(q_free, &buf, 0);               /* recycle either way */
    }
}

void setup()
{
    Serial.begin(115200);

    pinMode(PIN_DRDY, INPUT);
    pinMode(PIN_READY, OUTPUT);
    digitalWrite(PIN_READY, LOW);
    pinMode(PIN_CS, OUTPUT);
    digitalWrite(PIN_CS, HIGH);                    /* deselected between frames */
    spi.begin(PIN_SCK, PIN_MISO, -1, -1);          /* CS is soft-driven above */

    q_free = xQueueCreate(BUF_COUNT, sizeof(uint8_t *));
    q_full = xQueueCreate(BUF_COUNT, sizeof(uint8_t *));
    for (int i = 0; i < BUF_COUNT; i++) { uint8_t *b = bufs[i]; xQueueSend(q_free, &b, 0); }

    xTaskCreatePinnedToCore(spiTask, "spi_rx",  4096, NULL, 10, &h_spi_task, 1);
    xTaskCreatePinnedToCore(tcpTask, "tcp_tx",  8192, NULL,  5, NULL,        0);
    attachInterrupt(digitalPinToInterrupt(PIN_DRDY), drdyIsr, RISING);

    Serial.println("[BRIDGE] XIH6 LEPTON SPI->TCP bridge up (dual-task)");
}

void loop()
{
    static uint32_t last = 0;
    if (millis() - last >= 1000)
    {
        last = millis();
        Serial.printf("[BRIDGE] ok=%lu sent=%lu crcbad=%lu hdrbad=%lu stale=%lu tcpfail=%lu drdyto=%lu wifi=%d ip=%s\r\n",
                      (unsigned long)d_frames_ok, (unsigned long)d_tcp_sent,
                      (unsigned long)d_crc_bad, (unsigned long)d_hdr_bad,
                      (unsigned long)d_stale_drop, (unsigned long)d_tcp_fail,
                      (unsigned long)d_drdy_timeout,
                      (int)(WiFi.status() == WL_CONNECTED),
                      WiFi.localIP().toString().c_str());
    }
    vTaskDelay(pdMS_TO_TICKS(50));
}

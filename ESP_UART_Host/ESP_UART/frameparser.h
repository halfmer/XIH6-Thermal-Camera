#ifndef FRAMEPARSER_H
#define FRAMEPARSER_H

#include <QObject>
#include <QByteArray>
#include <cstdint>

// Binary protocol parser for thermal frame data over serial
//
// Frame format (ESP32 → PC):
//   [0xAA, 0x55]     sync marker
//   [type]            1 byte   — 0x01 = raw 16-bit thermal
//   [frame_id]        2 bytes  — uint16 BE, rolling counter
//   [width]           2 bytes  — uint16 BE
//   [height]          2 bytes  — uint16 BE
//   [pixel_len]       4 bytes  — uint32 BE, = width * height * 2
//   [pixel_data]      N bytes  — raw uint16 BE per pixel
//   [checksum]        2 bytes  — uint16 BE, simple sum of all previous bytes

struct ThermalFrame {
    uint16_t frameId = 0;
    uint16_t width   = 0;
    uint16_t height  = 0;
    QByteArray pixelData;  // raw uint16 BE data
    bool valid = false;
};

class FrameParser : public QObject
{
    Q_OBJECT

public:
    explicit FrameParser(QObject *parent = nullptr);

    // Feed raw bytes from serial port
    void feed(const QByteArray &data);

    // Reset parser state
    void reset();

    int framesReceived() const { return m_frameCount; }
    int badFrames() const { return m_badCount; }

signals:
    void frameReady(const ThermalFrame &frame);
    void parseError(const QString &message);

private:
    enum State { WaitingSync0, WaitingSync1, ReadingHeader, ReadingPayload, ReadingChecksum };

    static constexpr quint8  SYNC0 = 0xAA;
    static constexpr quint8  SYNC1 = 0x55;
    static constexpr int HEADER_SIZE = 11;   // type(1) + id(2) + w(2) + h(2) + len(4)
    static constexpr int CHECKSUM_SIZE = 2;

    QByteArray m_buffer;
    State m_state = WaitingSync0;
    int m_payloadLen = 0;

    uint8_t  m_type = 0;
    uint16_t m_frameId = 0;
    uint16_t m_width = 0;
    uint16_t m_height = 0;

    int m_frameCount = 0;
    int m_badCount = 0;

    bool tryParseFrame();
    quint16 readU16BE(const quint8 *p) const;
    quint32 readU32BE(const quint8 *p) const;
};

#endif // FRAMEPARSER_H

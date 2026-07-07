#include "frameparser.h"
#include <QtEndian>

FrameParser::FrameParser(QObject *parent)
    : QObject(parent)
{
}

void FrameParser::feed(const QByteArray &data)
{
    m_buffer.append(data);

    // Try to parse as many frames as possible from buffer
    while (m_buffer.size() >= 2) {
        switch (m_state) {
        case WaitingSync0:
            // Scan for first sync byte
            for (int i = 0; i < m_buffer.size(); ++i) {
                if (static_cast<quint8>(m_buffer.at(i)) == SYNC0) {
                    m_buffer.remove(0, i);
                    m_state = WaitingSync1;
                    break;
                }
            }
            if (m_state != WaitingSync1) {
                m_buffer.clear();  // no sync found, discard all
                return;
            }
            break;

        case WaitingSync1:
            if (m_buffer.size() < 2) return;
            if (static_cast<quint8>(m_buffer.at(1)) == SYNC1) {
                m_state = ReadingHeader;
            } else {
                // False sync — skip the first byte and go back to hunting
                m_buffer.remove(0, 1);
                m_state = WaitingSync0;
            }
            break;

        case ReadingHeader:
            if (m_buffer.size() < 2 + HEADER_SIZE) return;

            {
                const quint8 *hdr = reinterpret_cast<const quint8 *>(m_buffer.constData()) + 2;
                m_type    = hdr[0];
                m_frameId = readU16BE(hdr + 1);
                m_width   = readU16BE(hdr + 3);
                m_height  = readU16BE(hdr + 5);
                m_payloadLen = static_cast<int>(readU32BE(hdr + 7));

                // Sanity check
                if (m_width == 0 || m_height == 0 || m_width > 640 || m_height > 480
                    || m_payloadLen != static_cast<int>(m_width) * m_height * 2
                    || m_type != 0x01) {
                    emit parseError(QString("Bad header: %1x%2 len=%3 type=%4")
                                        .arg(m_width).arg(m_height).arg(m_payloadLen).arg(m_type));
                    m_buffer.remove(0, 2);  // skip the bad sync, try again
                    m_state = WaitingSync0;
                    m_badCount++;
                    break;  // re-enter switch
                }

                m_state = ReadingPayload;
            }
            break;

        case ReadingPayload: {
            int totalLen = 2 + HEADER_SIZE + m_payloadLen + CHECKSUM_SIZE;
            if (m_buffer.size() < totalLen) return;

            // Extract payload
            const char *payloadStart = m_buffer.constData() + 2 + HEADER_SIZE;
            QByteArray pixels(payloadStart, m_payloadLen);

            // Verify checksum
            const quint8 *checkStart = reinterpret_cast<const quint8 *>(m_buffer.constData());
            quint16 computed = 0;
            int checkLen = 2 + HEADER_SIZE + m_payloadLen;
            for (int i = 0; i < checkLen; ++i)
                computed += checkStart[i];

            quint16 received = readU16BE(checkStart + checkLen);

            if (computed == received) {
                ThermalFrame frame;
                frame.frameId  = m_frameId;
                frame.width    = m_width;
                frame.height   = m_height;
                frame.pixelData = pixels;
                frame.valid    = true;
                m_frameCount++;
                emit frameReady(frame);
            } else {
                m_badCount++;
                emit parseError(QString("Checksum mismatch: got %1 expected %2")
                                    .arg(received).arg(computed));
            }

            m_buffer.remove(0, totalLen);
            m_state = WaitingSync0;
            break;
        }

        case ReadingChecksum:
            // Handled inside ReadingPayload, shouldn't get here
            m_state = WaitingSync0;
            break;
        }
    }
}

void FrameParser::reset()
{
    m_buffer.clear();
    m_state = WaitingSync0;
    m_frameCount = 0;
    m_badCount = 0;
}

// ── Big-endian readers ────────────────────────────────────────────────

quint16 FrameParser::readU16BE(const quint8 *p) const
{
    return qFromBigEndian<quint16>(p);
}

quint32 FrameParser::readU32BE(const quint8 *p) const
{
    return qFromBigEndian<quint32>(p);
}

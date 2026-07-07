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
                    emit diagnostic(QString("bad_header drop=2 buf=%1 type=%2 fid=%3 w=%4 h=%5 len=%6 head=%7")
                                        .arg(m_buffer.size())
                                        .arg(m_type)
                                        .arg(m_frameId)
                                        .arg(m_width)
                                        .arg(m_height)
                                        .arg(m_payloadLen)
                                        .arg(QString::fromLatin1(m_buffer.left(32).toHex(' '))));
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
            quint16 computedNoSync = 0;
            int checkLen = 2 + HEADER_SIZE + m_payloadLen;
            for (int i = 0; i < checkLen; ++i)
                computed += checkStart[i];
            for (int i = 2; i < checkLen; ++i)
                computedNoSync += checkStart[i];

            quint16 received = readU16BE(checkStart + checkLen);
            quint16 receivedLE = qFromLittleEndian<quint16>(checkStart + checkLen);

            bool checksumOk = computed == received || computedNoSync == received ||
                computed == receivedLE || computedNoSync == receivedLE;

            int consumeLen = totalLen;
            if (checksumOk) {
                ThermalFrame frame;
                frame.frameId  = m_frameId;
                frame.width    = m_width;
                frame.height   = m_height;
                frame.pixelData = pixels;
                frame.valid    = true;
                m_frameCount++;
                emit frameReady(frame);
            } else {
                int nextSync = -1;
                for (int i = 2; i + 2 + HEADER_SIZE <= m_buffer.size(); ++i) {
                    if (static_cast<quint8>(m_buffer.at(i)) != SYNC0 ||
                        static_cast<quint8>(m_buffer.at(i + 1)) != SYNC1) {
                        continue;
                    }

                    const quint8 *nextHdr =
                        reinterpret_cast<const quint8 *>(m_buffer.constData()) + i + 2;
                    const quint8 nextType = nextHdr[0];
                    const quint16 nextWidth = readU16BE(nextHdr + 3);
                    const quint16 nextHeight = readU16BE(nextHdr + 5);
                    const quint32 nextPayloadLen = readU32BE(nextHdr + 7);

                    if (nextType == 0x01 && nextWidth > 0 && nextHeight > 0 &&
                        nextWidth <= 640 && nextHeight <= 480 &&
                        nextPayloadLen == static_cast<quint32>(nextWidth) * nextHeight * 2U) {
                        nextSync = i;
                        break;
                    }
                }

                if (nextSync > 0) {
                    consumeLen = nextSync;
                } else {
                    if (m_buffer.size() < totalLen + 2 + HEADER_SIZE)
                        return;
                    consumeLen = 1;
                }

                m_badCount++;
                // syncDelta: where the next plausible frame head sits relative
                // to where this frame should have ended. Negative = bytes were
                // LOST on the link, positive = bytes were INSERTED. This is the
                // single most useful number for the drop-vs-corrupt verdict.
                emit diagnostic(QString("checksum_bad fid=%1 gotBE=%2 gotLE=%3 expected=%4 expectedNoSync=%5 diff=%6 buf=%7 total=%8 consume=%9 nextSync=%10 syncDelta=%11 head=%12 tail=%13")
                                    .arg(m_frameId)
                                    .arg(received)
                                    .arg(receivedLE)
                                    .arg(computed)
                                    .arg(computedNoSync)
                                    .arg(static_cast<qint32>(computed) - static_cast<qint32>(received))
                                    .arg(m_buffer.size())
                                    .arg(totalLen)
                                    .arg(consumeLen)
                                    .arg(nextSync)
                                    .arg(nextSync > 0 ? (nextSync - totalLen) : 0)
                                    .arg(QString::fromLatin1(m_buffer.left(32).toHex(' ')))
                                    .arg(QString::fromLatin1(m_buffer.mid(qMax(0, totalLen - 16), 32).toHex(' '))));
                emit parseError(QString("Checksum mismatch: fid=%1 gotBE=%2 gotLE=%3 expected=%4 expectedNoSync=%5 diff=%6")
                                    .arg(m_frameId)
                                    .arg(received)
                                    .arg(receivedLE)
                                    .arg(computed)
                                    .arg(computedNoSync)
                                    .arg(static_cast<qint32>(computed) - static_cast<qint32>(received)));
            }

            m_buffer.remove(0, consumeLen);
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

#include "../frameparser.h"

#include <QCoreApplication>
#include <QDebug>
#include <QRandomGenerator>
#include <random>

static constexpr int kWidth = 160;
static constexpr int kHeight = 120;
static constexpr int kPayloadLen = kWidth * kHeight * 2;
static constexpr int kFrameLen = 13 + kPayloadLen + 2;

static QByteArray makeFrame(quint16 frameId, bool forceSyncInPayload = false)
{
    QByteArray frame;
    frame.reserve(kFrameLen);

    auto put8 = [&frame](quint8 v) { frame.append(static_cast<char>(v)); };
    auto put16 = [&put8](quint16 v) {
        put8(static_cast<quint8>(v >> 8));
        put8(static_cast<quint8>(v & 0xff));
    };
    auto put32 = [&put8](quint32 v) {
        put8(static_cast<quint8>(v >> 24));
        put8(static_cast<quint8>((v >> 16) & 0xff));
        put8(static_cast<quint8>((v >> 8) & 0xff));
        put8(static_cast<quint8>(v & 0xff));
    };

    put8(0xaa);
    put8(0x55);
    put8(0x01);
    put16(frameId);
    put16(kWidth);
    put16(kHeight);
    put32(kPayloadLen);

    for (int i = 0; i < kWidth * kHeight; ++i) {
        if (forceSyncInPayload && i == 1234) {
            put8(0xaa);
            put8(0x55);
        } else {
            const quint16 v = static_cast<quint16>(30000 + ((frameId * 37 + i * 13) & 0x0fff));
            put16(v);
        }
    }

    quint16 checksum = 0;
    for (unsigned char byte : frame)
        checksum = static_cast<quint16>(checksum + byte);
    put16(checksum);

    return frame;
}

struct ParseStats {
    int frames = 0;
    int errors = 0;
    QList<quint16> ids;
    QString lastError;
};

static ParseStats parseStream(const QByteArray &stream, int minChunk, int maxChunk,
                              quint32 seed = 0x12345678)
{
    FrameParser parser;
    ParseStats stats;
    std::mt19937 rng(seed);

    QObject::connect(&parser, &FrameParser::frameReady, &parser, [&](const ThermalFrame &frame) {
        ++stats.frames;
        stats.ids.append(frame.frameId);
    });
    QObject::connect(&parser, &FrameParser::parseError, &parser, [&](const QString &message) {
        ++stats.errors;
        stats.lastError = message;
    });

    int pos = 0;
    while (pos < stream.size()) {
        const int remain = stream.size() - pos;
        const int span = maxChunk - minChunk + 1;
        int chunk = minChunk;
        if (span > 1)
            chunk += static_cast<int>(rng() % static_cast<quint32>(span));
        chunk = qMin(chunk, remain);
        parser.feed(stream.mid(pos, chunk));
        pos += chunk;
    }

    return stats;
}

static ParseStats parseAsSerial2Mbps(const QByteArray &stream, quint32 seed)
{
    FrameParser parser;
    ParseStats stats;
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> intervalUs(100, 20000);

    QObject::connect(&parser, &FrameParser::frameReady, &parser, [&](const ThermalFrame &frame) {
        ++stats.frames;
        stats.ids.append(frame.frameId);
    });
    QObject::connect(&parser, &FrameParser::parseError, &parser, [&](const QString &message) {
        ++stats.errors;
        stats.lastError = message;
    });

    int pos = 0;
    while (pos < stream.size()) {
        const int us = intervalUs(rng);
        const int bytesAt2Mbps8N1 = qMax(1, (2000000 / 10) * us / 1000000);
        const int jitter = static_cast<int>(rng() % 257U) - 128;
        const int chunk = qBound(1, bytesAt2Mbps8N1 + jitter, stream.size() - pos);
        parser.feed(stream.mid(pos, chunk));
        pos += chunk;
    }

    return stats;
}

static bool expect(const char *name, const ParseStats &stats, int frames, int errors)
{
    const bool ok = stats.frames == frames && stats.errors == errors;
    qInfo().noquote() << QString("%1: frames=%2 errors=%3 ids=%4 last=%5")
                             .arg(name)
                             .arg(stats.frames)
                             .arg(stats.errors)
                             .arg([&] {
                                 QStringList out;
                                 for (quint16 id : stats.ids)
                                     out << QString::number(id);
                                 return out.join(',');
                             }())
                             .arg(stats.lastError);
    if (!ok)
        qCritical().noquote() << QString("FAILED %1: expected frames=%2 errors=%3")
                                     .arg(name)
                                     .arg(frames)
                                     .arg(errors);
    return ok;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QByteArray good;
    for (quint16 id = 0; id < 3; ++id)
        good.append(makeFrame(id, true));

    bool ok = true;
    ok &= expect("one_chunk_3frames", parseStream(good, good.size(), good.size()), 3, 0);
    ok &= expect("one_byte_chunks", parseStream(good, 1, 1), 3, 0);
    ok &= expect("random_chunks", parseStream(good, 1, 997), 3, 0);
    ok &= expect("garbage_prefix", parseStream(QByteArray("abc\x00\xff", 5) + good, 1, 300), 3, 0);

    for (quint32 seed = 1; seed <= 100; ++seed) {
        const ParseStats serialStats = parseAsSerial2Mbps(good, seed);
        if (serialStats.frames != 3 || serialStats.errors != 0) {
            qCritical().noquote() << QString("FAILED serial_2mbps_clean seed=%1 frames=%2 errors=%3")
                                         .arg(seed)
                                         .arg(serialStats.frames)
                                         .arg(serialStats.errors);
            ok = false;
            break;
        }
    }

    QByteArray flipped = makeFrame(10) + makeFrame(11);
    flipped[1000] = static_cast<char>(static_cast<unsigned char>(flipped[1000]) ^ 0x5a);
    ok &= expect("one_payload_byte_flipped", parseStream(flipped, 1, 500), 1, 1);

    QByteArray dropped = makeFrame(20) + makeFrame(21) + makeFrame(22);
    dropped.remove(1000, 1);
    const ParseStats droppedStats = parseStream(dropped, 1, 500);
    ok &= droppedStats.frames >= 2 && droppedStats.errors >= 1;
    qInfo().noquote() << QString("one_payload_byte_dropped: frames=%1 errors=%2 ids=%3 last=%4")
                             .arg(droppedStats.frames)
                             .arg(droppedStats.errors)
                             .arg([&] {
                                 QStringList out;
                                 for (quint16 id : droppedStats.ids)
                                     out << QString::number(id);
                                 return out.join(',');
                             }())
                             .arg(droppedStats.lastError);

    QByteArray inserted = makeFrame(30) + makeFrame(31) + makeFrame(32);
    inserted.insert(1000, static_cast<char>(0x5a));
    const ParseStats insertedStats = parseAsSerial2Mbps(inserted, 42);
    ok &= insertedStats.frames >= 2 && insertedStats.errors >= 1;
    qInfo().noquote() << QString("one_payload_byte_inserted: frames=%1 errors=%2 ids=%3 last=%4")
                             .arg(insertedStats.frames)
                             .arg(insertedStats.errors)
                             .arg([&] {
                                 QStringList out;
                                 for (quint16 id : insertedStats.ids)
                                     out << QString::number(id);
                                 return out.join(',');
                             }())
                             .arg(insertedStats.lastError);

    return ok ? 0 : 1;
}

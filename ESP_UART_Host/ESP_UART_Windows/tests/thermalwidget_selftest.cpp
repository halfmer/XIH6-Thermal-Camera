#include "../colormap.h"
#include "../framegate.h"

#include <QImage>
#include <QSet>
#include <QVector>

static constexpr int kWidth = 160;
static constexpr int kHeight = 120;

static QByteArray makeBigEndianPayload()
{
    QByteArray payload;
    payload.reserve(kWidth * kHeight * 2);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const quint16 v = static_cast<quint16>(29500 + x * 12 + y * 8);
            payload.append(static_cast<char>(v >> 8));
            payload.append(static_cast<char>(v & 0xff));
        }
    }
    return payload;
}

static QVector<quint16> convertPayloadLikeMainWindow(const QByteArray &payload)
{
    QVector<quint16> host(payload.size() / 2);
    const auto *bytes = reinterpret_cast<const quint8 *>(payload.constData());
    for (int i = 0; i < host.size(); ++i)
        host[i] = (static_cast<quint16>(bytes[i * 2]) << 8) | bytes[i * 2 + 1];
    return host;
}

int main()
{
    const QByteArray payload = makeBigEndianPayload();
    const QVector<quint16> host = convertPayloadLikeMainWindow(payload);
    if (host.size() != kWidth * kHeight)
        return 1;
    if (host.first() != 29500)
        return 2;
    if (host.last() <= host.first())
        return 3;

    quint16 vmin = 0;
    quint16 vmax = 0;
    double vavg = 0.0;
    const QImage image = ColorMap::apply(host.constData(), kWidth, kHeight, ColorMap::Ironbow,
                                         &vmin, &vmax, &vavg);
    if (image.isNull() || image.width() != kWidth || image.height() != kHeight)
        return 4;
    if (vmin != host.first() || vmax != host.last() || vavg <= vmin || vavg >= vmax)
        return 5;

    QSet<QRgb> colors;
    for (int y = 0; y < image.height(); y += 4) {
        for (int x = 0; x < image.width(); x += 4)
            colors.insert(image.pixel(x, y));
    }
    if (colors.size() < 16)
        return 6;

    // ── FrameGate: stitched-frame (torn) detection ──────────────────────
    {
        // Smooth vertical gradient (+8 counts/row): must pass.
        QVector<quint16> smooth(kWidth * kHeight);
        for (int y = 0; y < kHeight; ++y)
            for (int x = 0; x < kWidth; ++x)
                smooth[y * kWidth + x] = static_cast<quint16>(30000 + y * 8);
        if (FrameGate::looksTorn(smooth.constData(), kWidth, kHeight))
            return 7;

        // Time-skewed stitch at the 59|60 segment seam: must be caught.
        QVector<quint16> tornMid = smooth;
        for (int y = 60; y < kHeight; ++y)
            for (int x = 0; x < kWidth; ++x)
                tornMid[y * kWidth + x] = static_cast<quint16>(tornMid[y * kWidth + x] + 1200);
        if (!FrameGate::looksTorn(tornMid.constData(), kWidth, kHeight))
            return 8;

        // Same at the 29|30 seam.
        QVector<quint16> tornTop = smooth;
        for (int y = 30; y < kHeight; ++y)
            for (int x = 0; x < kWidth; ++x)
                tornTop[y * kWidth + x] = static_cast<quint16>(tornTop[y * kWidth + x] + 900);
        if (!FrameGate::looksTorn(tornTop.constData(), kWidth, kHeight))
            return 9;

        // Strong natural horizontal edge OFF the seams (45|46): must NOT trip.
        QVector<quint16> edge = smooth;
        for (int y = 46; y < kHeight; ++y)
            for (int x = 0; x < kWidth; ++x)
                edge[y * kWidth + x] = static_cast<quint16>(edge[y * kWidth + x] + 1500);
        if (FrameGate::looksTorn(edge.constData(), kWidth, kHeight))
            return 10;

        // Report fields: worst seam of tornMid is the middle one (index 1).
        const FrameGate::TearReport rep = FrameGate::analyze(tornMid.constData(), kWidth, kHeight);
        if (!rep.torn || rep.worstBoundary != 1 || rep.worstDiff < 1000.0)
            return 11;
    }

    return 0;
}

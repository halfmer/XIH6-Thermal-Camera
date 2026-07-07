#include "../colormap.h"

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

    return colors.size() >= 16 ? 0 : 6;
}

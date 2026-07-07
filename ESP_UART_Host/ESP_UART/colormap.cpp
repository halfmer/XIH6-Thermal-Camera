#include "colormap.h"
#include <algorithm>
#include <cmath>

const QStringList &ColorMap::presetNames()
{
    static const QStringList names = {
        "铁红 (Ironbow)", "白热 (Grayscale)", "黑热 (Black Hot)",
        "彩虹 (Rainbow)", "熔岩 (Lava)", "极地 (Arctic)"
    };
    return names;
}

// ── Public: apply colormap to raw 16-bit thermal data ────────────────

QImage ColorMap::apply(const quint16 *raw, int width, int height,
                        Preset preset, quint16 *outMin, quint16 *outMax)
{
    QImage image(width, height, QImage::Format_RGB32);
    if (width <= 0 || height <= 0)
        return image;

    // Find min / max for auto-ranging
    const int count = width * height;
    quint16 vmin = raw[0], vmax = raw[0];
    for (int i = 1; i < count; ++i) {
        if (raw[i] < vmin) vmin = raw[i];
        if (raw[i] > vmax) vmax = raw[i];
    }
    if (outMin) *outMin = vmin;
    if (outMax) *outMax = vmax;

    const QVector<QRgb> &table = lut(preset);
    const int range = vmax - vmin;
    const float scale = (range > 0) ? 255.0f / range : 0.0f;

    quint32 *bits = reinterpret_cast<quint32 *>(image.bits());
    for (int i = 0; i < count; ++i) {
        const int idx = qBound(0, static_cast<int>((raw[i] - vmin) * scale), 255);
        bits[i] = table.at(idx);
    }

    return image;
}

// ── LUT access ──────────────────────────────────────────────────────

const QVector<QRgb> &ColorMap::lut(Preset preset)
{
    static const auto ironbow  = buildIronbow();
    static const auto gray     = buildGrayscale();
    static const auto blackhot = buildBlackHot();
    static const auto rainbow  = buildRainbow();
    static const auto lava     = buildLava();
    static const auto arctic   = buildArctic();

    switch (preset) {
    case Ironbow:   return ironbow;
    case Grayscale: return gray;
    case BlackHot:  return blackhot;
    case Rainbow:   return rainbow;
    case Lava:      return lava;
    case Arctic:    return arctic;
    }
    return ironbow;
}

// ── Palette builders (256 entries each) ──────────────────────────────

QVector<QRgb> ColorMap::buildIronbow()
{
    QVector<QRgb> t(256);
    for (int i = 0; i < 256; ++i) {
        float x = i / 255.0f;
        int r, g, b;
        if (x < 0.25f) {  // black → blue
            float s = x / 0.25f;
            r = 0; g = 0; b = static_cast<int>(128 + 127 * s);
        } else if (x < 0.50f) {  // blue → cyan → green
            float s = (x - 0.25f) / 0.25f;
            r = 0; g = static_cast<int>(255 * s); b = static_cast<int>(255 * (1 - s));
        } else if (x < 0.75f) {  // green → yellow → red
            float s = (x - 0.50f) / 0.25f;
            r = static_cast<int>(255 * s); g = 255; b = 0;
        } else {  // red → white
            float s = (x - 0.75f) / 0.25f;
            r = 255; g = std::min(255, static_cast<int>(255 * s)); b = std::min(255, static_cast<int>(255 * s));
        }
        t[i] = qRgb(r, g, b);
    }
    return t;
}

QVector<QRgb> ColorMap::buildGrayscale()
{
    QVector<QRgb> t(256);
    for (int i = 0; i < 256; ++i)
        t[i] = qRgb(i, i, i);
    return t;
}

QVector<QRgb> ColorMap::buildBlackHot()
{
    QVector<QRgb> t(256);
    for (int i = 0; i < 256; ++i) {
        int v = 255 - i;
        t[i] = qRgb(v, v, v);
    }
    return t;
}

QVector<QRgb> ColorMap::buildRainbow()
{
    QVector<QRgb> t(256);
    for (int i = 0; i < 256; ++i) {
        float x = i / 255.0f;
        int r, g, b;
        if (x < 0.2f) {
            float s = x / 0.2f;
            r = 0; g = 0; b = static_cast<int>(128 + 127 * s);
        } else if (x < 0.4f) {
            float s = (x - 0.2f) / 0.2f;
            r = 0; g = static_cast<int>(255 * s); b = 255;
        } else if (x < 0.6f) {
            float s = (x - 0.4f) / 0.2f;
            r = 0; g = 255; b = static_cast<int>(255 * (1 - s));
        } else if (x < 0.8f) {
            float s = (x - 0.6f) / 0.2f;
            r = static_cast<int>(255 * s); g = static_cast<int>(255 * (1 - s)); b = 0;
        } else {
            float s = (x - 0.8f) / 0.2f;
            r = 255; g = static_cast<int>(255 * s); b = static_cast<int>(255 * s);
        }
        t[i] = qRgb(r, g, b);
    }
    return t;
}

QVector<QRgb> ColorMap::buildLava()
{
    QVector<QRgb> t(256);
    for (int i = 0; i < 256; ++i) {
        float x = i / 255.0f;
        int r, g, b;
        if (x < 0.33f) {  // black → dark red
            float s = x / 0.33f;
            r = static_cast<int>(128 * s); g = 0; b = 0;
        } else if (x < 0.66f) {  // dark red → orange
            float s = (x - 0.33f) / 0.33f;
            r = 128 + static_cast<int>(127 * s); g = static_cast<int>(100 * s); b = 0;
        } else {  // orange → yellow → white
            float s = (x - 0.66f) / 0.34f;
            r = 255; g = 100 + static_cast<int>(155 * s); b = static_cast<int>(255 * s);
        }
        t[i] = qRgb(r, g, b);
    }
    return t;
}

QVector<QRgb> ColorMap::buildArctic()
{
    QVector<QRgb> t(256);
    for (int i = 0; i < 256; ++i) {
        float x = i / 255.0f;
        int r, g, b;
        if (x < 0.33f) {  // dark blue → blue-white
            float s = x / 0.33f;
            r = static_cast<int>(200 * s); g = static_cast<int>(200 * s); b = 128 + static_cast<int>(127 * s);
        } else if (x < 0.66f) {  // blue-white → cyan
            float s = (x - 0.33f) / 0.33f;
            r = 200; g = 200 + static_cast<int>(55 * s); b = 255;
        } else {  // cyan → yellow → red
            float s = (x - 0.66f) / 0.34f;
            r = 200 + static_cast<int>(55 * s); g = 255 - static_cast<int>(155 * s); b = 255 - static_cast<int>(200 * s);
        }
        t[i] = qRgb(qBound(0, r, 255), qBound(0, g, 255), qBound(0, b, 255));
    }
    return t;
}

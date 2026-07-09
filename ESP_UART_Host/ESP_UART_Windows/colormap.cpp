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
                        Preset preset, quint16 *outMin, quint16 *outMax,
                        double *outAvg)
{
    QImage image(width, height, QImage::Format_RGB32);
    if (width <= 0 || height <= 0)
        return image;

    // Robust auto-range (README_14 §10-11): filter out VoSPI dropout (0) and
    // saturated/error (0xFFFF) pixels first — these are not real temperatures
    // and their presence was what stretched plain min/max to 0..65535 and
    // turned normal 26C scenes pink. Then take p2/p98 of the remaining valid
    // pixels for a stable display range. A flame pixel (0xFFFF, excluded from
    // the valid set) still maps to the LUT top via the qBound clamp in the
    // render loop below, so the flame reads as max-colour, not "a few degrees".
    // (STM32 dead-pixel fix already replaces most 0/0xFFFF before transmission;
    // this filter is the host-side safety net for any that slip through.)
    const int count = width * height;
    QVector<quint16> valid;
    valid.reserve(count);
    double sum = 0.0;
    int validCount = 0;
    for (int i = 0; i < count; ++i) {
        const quint16 v = raw[i];
        sum += v;
        if ((v != 0U) && (v != 0xFFFFU)) {
            valid.append(v);
            validCount++;
        }
    }
    if (validCount < 10) {                 // fallback: scene is all-dropout
        valid.clear();
        for (int i = 0; i < count; ++i) valid.append(raw[i]);
        validCount = count;
    }
    std::sort(valid.begin(), valid.end());

    quint16 vmin = valid.first();
    quint16 vmax = valid.last();
    if (validCount >= 100) {
        vmin = valid[validCount * 2 / 100];       // p2 of valid
        vmax = valid[validCount * 98 / 100];      // p98 of valid
        if (vmax <= vmin) { vmax = valid.last(); vmin = valid.first(); }
    }
    if (outMin) *outMin = vmin;
    if (outMax) *outMax = vmax;
    if (outAvg) *outAvg = sum / count;

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

double ColorMap::rawToCelsius(quint16 raw)
{
    // Lepton 3.5: raw = centikelvin (0.01 K)
    return raw * 0.01 - 273.15;
}

QString ColorMap::temperatureString(quint16 raw)
{
    double c = rawToCelsius(raw);
    if (c < -50.0 || c > 500.0)
        return QString("RAW:%1").arg(raw);
    return QString("%1 °C").arg(c, 0, 'f', 1);
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

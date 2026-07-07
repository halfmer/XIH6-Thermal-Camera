#ifndef COLORMAP_H
#define COLORMAP_H

#include <QImage>
#include <QVector>
#include <QString>
#include <QRgb>

// Pre-computed color look-up tables for thermal imaging
class ColorMap
{
public:
    enum Preset {
        Ironbow,      // default — most common thermal palette
        Grayscale,    // white-hot
        BlackHot,     // inverted grayscale
        Rainbow,      // full spectrum
        Lava,         // red-orange-yellow-white
        Arctic        // blue-cyan-green-yellow-red
    };

    static const QStringList &presetNames();
    static QImage apply(const quint16 *raw, int width, int height,
                        Preset preset = Ironbow,
                        quint16 *outMin = nullptr,
                        quint16 *outMax = nullptr);

private:
    static const QVector<QRgb> &lut(Preset preset);
    static QVector<QRgb> buildIronbow();
    static QVector<QRgb> buildGrayscale();
    static QVector<QRgb> buildBlackHot();
    static QVector<QRgb> buildRainbow();
    static QVector<QRgb> buildLava();
    static QVector<QRgb> buildArctic();
};

#endif // COLORMAP_H

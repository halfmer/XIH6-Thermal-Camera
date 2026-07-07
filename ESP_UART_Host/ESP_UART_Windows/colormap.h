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

    // Apply colormap to raw 16-bit data, return QImage + auto-range stats
    static QImage apply(const quint16 *raw, int width, int height,
                        Preset preset = Ironbow,
                        quint16 *outMin = nullptr,
                        quint16 *outMax = nullptr,
                        double *outAvg = nullptr);

    // Convert Lepton 3.5 raw value (centikelvin) to Celsius
    static double rawToCelsius(quint16 raw);
    static QString temperatureString(quint16 raw);

    // Access LUT for external rendering (e.g. color scale bar)
    static const QVector<QRgb> &lut(Preset preset);

private:
    static QVector<QRgb> buildIronbow();
    static QVector<QRgb> buildGrayscale();
    static QVector<QRgb> buildBlackHot();
    static QVector<QRgb> buildRainbow();
    static QVector<QRgb> buildLava();
    static QVector<QRgb> buildArctic();
};

#endif // COLORMAP_H

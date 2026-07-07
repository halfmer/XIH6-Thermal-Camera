#ifndef THERMALWIDGET_H
#define THERMALWIDGET_H

#include <QWidget>
#include <QImage>
#include <QElapsedTimer>
#include <QVector>
#include "colormap.h"

class ThermalWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ThermalWidget(QWidget *parent = nullptr);

    void setColorMap(ColorMap::Preset preset);
    ColorMap::Preset colorMap() const { return m_colorMap; }

    // Feed a decoded thermal frame for display
    void displayFrame(const quint16 *rawData, int width, int height);

    // Performance stats
    double currentFps() const   { return m_fps; }
    double currentAvg() const   { return m_avgTemp; }

    // Save current view (with overlays) as image file
    bool saveScreenshot(const QString &filePath);
    // Get raw pixel value at image coordinates (for spot measurement)
    quint16 pixelAt(int imgX, int imgY) const;
    // Map widget coordinates to image coordinates
    QPoint widgetToImage(const QPoint &widgetPos) const;

    // Show/hide the spot measurement marker
    void setSpotVisible(bool v)  { m_spotVisible = v; update(); }
    void setSpotPos(const QPoint &imgPos);

    QSize minimumSizeHint() const override { return QSize(320, 240); }
    QSize sizeHint() const override { return QSize(640, 480); }

signals:
    void spotTemperatureChanged(const QString &text);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QImage m_image;
    ColorMap::Preset m_colorMap = ColorMap::Ironbow;

    // Full-frame raw data for pixel lookup (spot measurement)
    QVector<quint16> m_rawData;
    int m_dataWidth  = 0;
    int m_dataHeight = 0;

    double m_avgTemp = 0.0;

    // FPS tracking
    QElapsedTimer m_fpsTimer;
    int m_frameCount = 0;
    double m_fps = 0.0;

    // Image placement rect (computed each paint)
    QRect m_imgRect;

    // Spot measurement
    bool    m_spotVisible = false;
    QPoint  m_spotImgPos;      // spot position in image coordinates

    // Color scale bar
    QImage m_scaleBarCache;
    void rebuildScaleBar();
    void drawOverlay(QPainter &painter, const QRect &imgRect);
    void drawScaleBar(QPainter &painter, const QRect &imgRect);
};

#endif // THERMALWIDGET_H

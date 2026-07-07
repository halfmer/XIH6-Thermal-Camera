#ifndef THERMALWIDGET_H
#define THERMALWIDGET_H

#include <QWidget>
#include <QImage>
#include <QElapsedTimer>
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
    double currentFps() const { return m_fps; }
    quint16 minTemp() const { return m_minTemp; }
    quint16 maxTemp() const { return m_maxTemp; }

    QSize minimumSizeHint() const override { return QSize(320, 240); }
    QSize sizeHint() const override { return QSize(640, 480); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QImage m_image;
    ColorMap::Preset m_colorMap = ColorMap::Ironbow;

    quint16 m_minTemp = 0;
    quint16 m_maxTemp = 0;

    // FPS tracking
    QElapsedTimer m_fpsTimer;
    int m_frameCount = 0;
    double m_fps = 0.0;

    void drawOverlay(QPainter &painter, const QRect &imgRect);
};

#endif // THERMALWIDGET_H

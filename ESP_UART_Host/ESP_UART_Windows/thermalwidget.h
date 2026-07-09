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

    // Feed a decoded thermal frame for display. Returns false when the tear
    // gate rejected a stitched frame — the previous image stays on screen and
    // the caller may log/count the rejection.
    bool displayFrame(const quint16 *rawData, int width, int height);

    // Tear-gate: reject frames stitched from segments of different capture
    // rounds (visible horizontal seams) instead of displaying them.
    void setTearGateEnabled(bool on) { m_tearGateEnabled = on; }
    bool tearGateEnabled() const     { return m_tearGateEnabled; }
    int  tornFrames() const          { return m_tornCount; }

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

    // Tear gate state. After kTearStreakForce consecutive rejections the next
    // torn frame is shown anyway, so a scene with a real hot edge sitting
    // exactly on a segment seam can never freeze the display outright.
    static constexpr int kTearStreakForce = 5;
    bool m_tearGateEnabled = true;
    int  m_tornCount  = 0;
    int  m_tornStreak = 0;

    // Full-frame raw data for pixel lookup (spot measurement)
    QVector<quint16> m_rawData;
    int m_dataWidth  = 0;
    int m_dataHeight = 0;

    // Segment cache (README_14 sec.10): a torn frame's 4 segments are each
    // internally valid (STM32 commits whole 30-row segments); only the
    // segment BOUNDARIES tear. So instead of discarding a torn frame, copy
    // each of its segments into the cache and re-assemble for display.
    // The displayed image is always the freshest segment combination -
    // motion stays fluid (no freeze), seams fade within a few frames.
    static constexpr int kSegCount = 4;
    static constexpr int kSegRows  = 30;   // 120 / 4
    QVector<quint16> m_segCache;          // width*kSegRows per segment, x4
    bool m_segCacheValid = false;

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

#include "thermalwidget.h"
#include <QPainter>
#include <QFontDatabase>

ThermalWidget::ThermalWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(320, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    m_fpsTimer.start();
}

void ThermalWidget::setColorMap(ColorMap::Preset preset)
{
    m_colorMap = preset;
    // Re-render current data with new colormap
    if (!m_image.isNull())
        update();
}

void ThermalWidget::displayFrame(const quint16 *rawData, int width, int height)
{
    m_image = ColorMap::apply(rawData, width, height, m_colorMap, &m_minTemp, &m_maxTemp);

    // FPS
    m_frameCount++;
    qint64 elapsed = m_fpsTimer.elapsed();
    if (elapsed >= 1000) {
        m_fps = m_frameCount * 1000.0 / elapsed;
        m_frameCount = 0;
        m_fpsTimer.restart();
    }

    update();
}

void ThermalWidget::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    if (m_image.isNull()) {
        // Placeholder
        painter.fillRect(rect(), QColor(20, 20, 25));
        painter.setPen(QColor(80, 80, 80));
        QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        f.setPointSize(14);
        painter.setFont(f);
        painter.drawText(rect(), Qt::AlignCenter, "等待热成像数据...\n连接 ESP32 并切换到热成像模式");
        return;
    }

    // Scale image to fit widget, keeping aspect ratio
    QRect imgRect = rect();
    float widgetAspect = static_cast<float>(width()) / height();
    float imgAspect = static_cast<float>(m_image.width()) / m_image.height();

    if (imgAspect > widgetAspect) {
        // Widget wider — fit by height
        int h = height();
        int w = static_cast<int>(h * imgAspect);
        imgRect = QRect((width() - w) / 2, 0, w, h);
    } else {
        // Widget taller — fit by width
        int w = width();
        int h = static_cast<int>(w / imgAspect);
        imgRect = QRect(0, (height() - h) / 2, w, h);
    }

    // Background
    painter.fillRect(rect(), QColor(10, 10, 15));
    painter.fillRect(imgRect, QColor(0, 0, 0));

    // Draw the thermal image
    painter.drawImage(imgRect, m_image);

    // Overlays
    drawOverlay(painter, imgRect);
}

void ThermalWidget::drawOverlay(QPainter &painter, const QRect &imgRect)
{
    const int margin = 8;

    // Crosshair at center
    painter.setPen(QPen(QColor(255, 255, 255, 80), 1, Qt::DashLine));
    int cx = imgRect.center().x();
    int cy = imgRect.center().y();
    painter.drawLine(cx - 20, cy, cx + 20, cy);
    painter.drawLine(cx, cy - 20, cx, cy + 20);

    // ── Top-left: min/max temperature ──────────────────
    QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monoFont.setPointSize(9);
    painter.setFont(monoFont);
    painter.setPen(Qt::white);

    // Background boxes
    QString minStr = QString(" MIN: %1 ").arg(m_minTemp);
    QString maxStr = QString(" MAX: %1 ").arg(m_maxTemp);
    QFontMetrics fm(monoFont);

    int boxH = fm.height() + 4;
    int minW = fm.horizontalAdvance(minStr);
    int maxW = fm.horizontalAdvance(maxStr);

    // Min (blue tint) top-left
    QRect minRect(imgRect.left() + margin, imgRect.top() + margin, minW, boxH);
    painter.fillRect(minRect, QColor(0, 80, 200, 180));
    painter.drawText(minRect, Qt::AlignCenter, minStr);

    // Max (red tint) below min
    QRect maxRect(imgRect.left() + margin, minRect.bottom() + 2, maxW, boxH);
    painter.fillRect(maxRect, QColor(200, 40, 0, 180));
    painter.drawText(maxRect, Qt::AlignCenter, maxStr);

    // ── Top-right: FPS + resolution ──────────────────────
    QString infoStr = QString("%1×%2  |  %3 FPS  |  %4")
                          .arg(m_image.width())
                          .arg(m_image.height())
                          .arg(m_fps, 0, 'f', 1)
                          .arg(ColorMap::presetNames().at(static_cast<int>(m_colorMap)));
    int infoW = fm.horizontalAdvance(infoStr);
    QRect infoRect(imgRect.right() - infoW - margin - 8, imgRect.top() + margin,
                   infoW + 8, boxH);
    painter.fillRect(infoRect, QColor(0, 0, 0, 150));
    painter.setPen(QColor(200, 200, 200));
    painter.drawText(infoRect, Qt::AlignCenter, infoStr);

    // ── Bottom-right: center pixel value ────────────────
    int centerX = m_image.width() / 2;
    int centerY = m_image.height() / 2;
    if (centerX < m_image.width() && centerY < m_image.height()) {
        // Remap center pixel from image coords to widget coords
        float scaleX = static_cast<float>(imgRect.width()) / m_image.width();
        float scaleY = static_cast<float>(imgRect.height()) / m_image.height();
        int px = imgRect.left() + static_cast<int>(centerX * scaleX);
        int py = imgRect.top() + static_cast<int>(centerY * scaleY);

        painter.setPen(QPen(QColor(255, 255, 0, 100), 1));
        painter.drawLine(px - 6, py, px + 6, py);
        painter.drawLine(px, py - 6, px, py + 6);
    }
}

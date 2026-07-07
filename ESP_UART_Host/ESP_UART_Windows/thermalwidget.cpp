#include "thermalwidget.h"
#include <QPainter>
#include <QFontDatabase>
#include <QMouseEvent>
#include <QFile>
#include <cmath>

ThermalWidget::ThermalWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(320, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    m_fpsTimer.start();
}

void ThermalWidget::setColorMap(ColorMap::Preset preset)
{
    m_colorMap = preset;
    rebuildScaleBar();
    if (!m_image.isNull()) update();
}

void ThermalWidget::setSpotPos(const QPoint &imgPos)
{
    m_spotImgPos = imgPos;
    m_spotVisible = true;
    update();
}

// ── Coordinate mapping ─────────────────────────────────────────────────

QPoint ThermalWidget::widgetToImage(const QPoint &widgetPos) const
{
    if (m_imgRect.isEmpty() || m_dataWidth <= 0 || m_dataHeight <= 0)
        return QPoint(-1, -1);

    int ix = static_cast<int>((widgetPos.x() - m_imgRect.left())
                              * m_dataWidth  / static_cast<double>(m_imgRect.width()));
    int iy = static_cast<int>((widgetPos.y() - m_imgRect.top())
                              * m_dataHeight / static_cast<double>(m_imgRect.height()));
    if (ix < 0 || ix >= m_dataWidth || iy < 0 || iy >= m_dataHeight)
        return QPoint(-1, -1);
    return QPoint(ix, iy);
}

quint16 ThermalWidget::pixelAt(int imgX, int imgY) const
{
    if (imgX < 0 || imgX >= m_dataWidth || imgY < 0 || imgY >= m_dataHeight)
        return 0;
    return m_rawData.at(imgY * m_dataWidth + imgX);
}

// ── Mouse interaction ──────────────────────────────────────────────────

void ThermalWidget::mousePressEvent(QMouseEvent *event)
{
    QPoint imgPos = widgetToImage(event->pos());
    if (imgPos.x() >= 0) {
        setSpotPos(imgPos);
        quint16 v = pixelAt(imgPos.x(), imgPos.y());
        emit spotTemperatureChanged(
            QString("X:%1 Y:%2  |  %3").arg(imgPos.x()).arg(imgPos.y())
                .arg(ColorMap::temperatureString(v)));
    }
}

void ThermalWidget::mouseMoveEvent(QMouseEvent *event)
{
    QPoint imgPos = widgetToImage(event->pos());
    if (imgPos.x() >= 0) {
        quint16 v = pixelAt(imgPos.x(), imgPos.y());
        emit spotTemperatureChanged(
            QString("X:%1 Y:%2  |  %3").arg(imgPos.x()).arg(imgPos.y())
                .arg(ColorMap::temperatureString(v)));
    } else {
        emit spotTemperatureChanged(QString());
    }
}

void ThermalWidget::leaveEvent(QEvent *)
{
    emit spotTemperatureChanged(QString());
}

// ── Frame display ──────────────────────────────────────────────────────

void ThermalWidget::displayFrame(const quint16 *rawData, int width, int height)
{
    const int count = width * height;

    // Cache raw data for spot measurement
    if (m_rawData.size() != count) m_rawData.resize(count);
    memcpy(m_rawData.data(), rawData, count * sizeof(quint16));
    m_dataWidth  = width;
    m_dataHeight = height;

    quint16 vmin, vmax;
    double vavg;
    m_image = ColorMap::apply(rawData, width, height, m_colorMap,
                              &vmin, &vmax, &vavg);
    m_avgTemp = vavg;

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

// ── Screenshot ─────────────────────────────────────────────────────────

bool ThermalWidget::saveScreenshot(const QString &filePath)
{
    // Render current widget (with overlays) to an image
    QImage screenshot(size(), QImage::Format_RGB32);
    screenshot.fill(QColor(10, 10, 15));
    QPainter p(&screenshot);
    p.setRenderHint(QPainter::Antialiasing);

    // Replicate paint logic
    if (m_image.isNull()) {
        p.fillRect(rect(), QColor(20, 20, 25));
        p.setPen(QColor(80, 80, 80));
        QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        f.setPointSize(14);
        p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter, "等待热成像数据...");
        p.end();
        return screenshot.save(filePath);
    }

    QRect imgRect = m_imgRect;  // use cached from last paint
    if (imgRect.isEmpty()) {
        // Compute if not yet painted
        float wa = static_cast<float>(width()) / height();
        float ia = static_cast<float>(m_image.width()) / m_image.height();
        if (ia > wa) {
            int h = height(); int w = static_cast<int>(h * ia);
            imgRect = QRect((width() - w) / 2, 0, w, h);
        } else {
            int w = width(); int h = static_cast<int>(w / ia);
            imgRect = QRect(0, (height() - h) / 2, w, h);
        }
    }

    p.fillRect(rect(), QColor(10, 10, 15));
    p.drawImage(imgRect, m_image);
    drawOverlay(p, imgRect);
    drawScaleBar(p, imgRect);
    p.end();

    return screenshot.save(filePath);
}

// ── Paint ──────────────────────────────────────────────────────────────

void ThermalWidget::resizeEvent(QResizeEvent *) { rebuildScaleBar(); }

void ThermalWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    if (m_image.isNull()) {
        painter.fillRect(rect(), QColor(20, 20, 25));
        painter.setPen(QColor(80, 80, 80));
        QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        f.setPointSize(14);
        painter.setFont(f);
        painter.drawText(rect(), Qt::AlignCenter,
                         "等待热成像数据...\n连接设备并切换到热成像模式");
        return;
    }

    // Calculate image placement (centered, aspect-ratio preserving)
    float widgetAspect = static_cast<float>(width()) / height();
    float imgAspect    = static_cast<float>(m_image.width()) / m_image.height();

    if (imgAspect > widgetAspect) {
        int h = height();
        int w = static_cast<int>(h * imgAspect);
        m_imgRect = QRect((width() - w) / 2, 0, w, h);
    } else {
        int w = width();
        int h = static_cast<int>(w / imgAspect);
        m_imgRect = QRect(0, (height() - h) / 2, w, h);
    }

    // Background
    painter.fillRect(rect(), QColor(10, 10, 15));
    painter.fillRect(m_imgRect, QColor(0, 0, 0));

    // Thermal image
    painter.drawImage(m_imgRect, m_image);

    // Overlays + scale bar
    drawOverlay(painter, m_imgRect);
    drawScaleBar(painter, m_imgRect);
}

// ── Overlays ───────────────────────────────────────────────────────────

void ThermalWidget::drawOverlay(QPainter &painter, const QRect &imgRect)
{
    const int margin = 8;
    QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monoFont.setPointSize(9);
    painter.setFont(monoFont);
    QFontMetrics fm(monoFont);
    int boxH = fm.height() + 4;

    // ── Top-left: MIN / MAX / AVG ──────────────────────
    QStringList labels = {
        QString("MIN: %1").arg(ColorMap::temperatureString(
            m_rawData.isEmpty() ? 0 : *std::min_element(m_rawData.begin(), m_rawData.end()))),
        QString("MAX: %1").arg(ColorMap::temperatureString(
            m_rawData.isEmpty() ? 0 : *std::max_element(m_rawData.begin(), m_rawData.end()))),
        QString("AVG: %1").arg(ColorMap::temperatureString(
            static_cast<quint16>(m_avgTemp)))
    };
    QList<QColor> bgColors = {
        QColor(0, 80, 200, 180),
        QColor(200, 40, 0, 180),
        QColor(40, 40, 40, 180)
    };

    int y = imgRect.top() + margin;
    for (int i = 0; i < labels.size(); ++i) {
        int w = fm.horizontalAdvance(labels[i]) + 8;
        QRect r(imgRect.left() + margin, y, w, boxH);
        painter.fillRect(r, bgColors[i]);
        painter.setPen(Qt::white);
        painter.drawText(r, Qt::AlignCenter, labels[i]);
        y = r.bottom() + 2;
    }

    // ── Top-right: FPS + res + colormap ───────────────────
    QString infoStr = QString("%1×%2  |  %3 FPS  |  %4")
                          .arg(m_image.width()).arg(m_image.height())
                          .arg(m_fps, 0, 'f', 1)
                          .arg(ColorMap::presetNames().at(static_cast<int>(m_colorMap)));
    int infoW = fm.horizontalAdvance(infoStr) + 12;
    QRect infoRect(imgRect.right() - infoW, imgRect.top() + margin, infoW, boxH);
    painter.fillRect(infoRect, QColor(0, 0, 0, 160));
    painter.setPen(QColor(200, 200, 200));
    painter.drawText(infoRect, Qt::AlignCenter, infoStr);

    // ── Center crosshair ─────────────────────────────────
    painter.setPen(QPen(QColor(255, 255, 255, 60), 1, Qt::DashLine));
    int cx = imgRect.center().x();
    int cy = imgRect.center().y();
    painter.drawLine(cx - 24, cy, cx + 24, cy);
    painter.drawLine(cx, cy - 24, cx, cy + 24);

    // ── Spot measurement marker ──────────────────────────
    if (m_spotVisible && m_spotImgPos.x() >= 0) {
        double sx = imgRect.left() + (m_spotImgPos.x() + 0.5) * imgRect.width()  / m_dataWidth;
        double sy = imgRect.top()  + (m_spotImgPos.y() + 0.5) * imgRect.height() / m_dataHeight;
        int px = static_cast<int>(sx);
        int py = static_cast<int>(sy);

        // Crosshair at spot
        painter.setPen(QPen(Qt::yellow, 1));
        painter.drawLine(px - 12, py, px + 12, py);
        painter.drawLine(px, py - 12, px, py + 12);
        painter.setPen(QPen(Qt::black, 1));
        painter.drawEllipse(QPoint(px, py), 4, 4);
        painter.setPen(QPen(Qt::yellow, 1));
        painter.drawEllipse(QPoint(px, py), 5, 5);

        // Temperature label at spot
        quint16 v = pixelAt(m_spotImgPos.x(), m_spotImgPos.y());
        QString tStr = ColorMap::temperatureString(v);
        int tw = fm.horizontalAdvance(tStr) + 8;
        QRect labelRect(px + 14, py - boxH / 2, tw, boxH);
        if (labelRect.right() > imgRect.right()) labelRect.moveRight(px - 14);
        if (labelRect.bottom() > imgRect.bottom()) labelRect.moveBottom(imgRect.bottom() - 2);
        painter.fillRect(labelRect, QColor(0, 0, 0, 200));
        painter.setPen(Qt::yellow);
        painter.drawText(labelRect, Qt::AlignCenter, tStr);
    }
}

// ── Color scale bar ────────────────────────────────────────────────────

void ThermalWidget::rebuildScaleBar()
{
    const int barH = 256;
    m_scaleBarCache = QImage(20, barH, QImage::Format_RGB32);
    const auto &table = ColorMap::lut(m_colorMap);
    for (int i = 0; i < barH; ++i) {
        QRgb c = table.at(255 - i);  // top = hot, bottom = cold
        for (int x = 0; x < 20; ++x)
            m_scaleBarCache.setPixelColor(x, i, QColor(c));
    }
}

void ThermalWidget::drawScaleBar(QPainter &painter, const QRect &imgRect)
{
    if (m_scaleBarCache.isNull()) rebuildScaleBar();

    const int barW = 16;
    const int barH = qMin(256, imgRect.height() * 3 / 4);
    int barX = imgRect.right() + 10;
    int barY = imgRect.top() + (imgRect.height() - barH) / 2;

    // Don't draw if doesn't fit
    if (barX + barW + 30 > width()) return;

    // Background
    QRect barRect(barX, barY, barW, barH);
    painter.fillRect(barRect.adjusted(-2, -2, 2, 2), QColor(0, 0, 0, 160));

    // Scaled color bar
    QImage scaled = m_scaleBarCache.scaled(barW, barH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    painter.drawImage(barRect.topLeft(), scaled);

    // Border
    painter.setPen(QPen(QColor(255, 255, 255, 60), 1));
    painter.drawRect(barRect);

    // Temperature labels
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPointSize(8);
    painter.setFont(f);
    painter.setPen(QColor(200, 200, 200));

    // Max temp at top
    quint16 vmax = m_rawData.isEmpty() ? 0
        : *std::max_element(m_rawData.begin(), m_rawData.end());
    QString maxStr = ColorMap::temperatureString(vmax);
    painter.drawText(barX + barW + 4, barY + 10, maxStr);

    // Mid temp
    if (!m_rawData.isEmpty()) {
        quint16 vmin = *std::min_element(m_rawData.begin(), m_rawData.end());
        quint16 vmid = vmin + (vmax - vmin) / 2;
        QString midStr = ColorMap::temperatureString(vmid);
        painter.drawText(barX + barW + 4, barY + barH / 2 + 4, midStr);
    }

    // Min temp at bottom
    quint16 vmin = m_rawData.isEmpty() ? 0
        : *std::min_element(m_rawData.begin(), m_rawData.end());
    QString minStr = ColorMap::temperatureString(vmin);
    painter.drawText(barX + barW + 4, barY + barH - 2, minStr);
}

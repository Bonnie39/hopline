#include "app/PreviewWidget.h"

#include <cmath>

#include <QPainter>

namespace hopline {

PreviewWidget::PreviewWidget(QWidget* parent)
    : QWidget(parent)
{
    setAutoFillBackground(false);
    setMinimumSize(320, 180);
}

void PreviewWidget::setFrame(const VideoFrame& frame)
{
    if (!frame.valid()) {
        clear();
        return;
    }

    // copy(): the QImage must not alias the caller's buffer, which is reused per frame.
    m_image = QImage(frame.rgba.data(), frame.width, frame.height,
                     frame.width * 4, QImage::Format_RGBA8888)
                  .copy();
    update();
}

void PreviewWidget::clear()
{
    m_image = QImage();
    update();
}

void PreviewWidget::setCanvas(int width, int height)
{
    m_canvasW = width;
    m_canvasH = height;
    update();
}

void PreviewWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(16, 16, 16));

    // No active sequence: just a placeholder.
    if (m_canvasW <= 0 || m_canvasH <= 0) {
        painter.setPen(QColor(96, 96, 96));
        painter.drawText(rect(), Qt::AlignCenter, "preview");
        return;
    }

    // The sequence canvas — a black frame of the sequence aspect, letterboxed into
    // the widget. Decoded frames are fit inside it (so a clip of a different aspect
    // pillar/letterboxes within the canvas rather than filling the whole preview).
    QSize canvas(m_canvasW, m_canvasH);
    canvas.scale(size(), Qt::KeepAspectRatio);
    const QRect canvasRect(QPoint((width() - canvas.width()) / 2, (height() - canvas.height()) / 2), canvas);
    painter.fillRect(canvasRect, Qt::black);

    if (!m_image.isNull()) {
        // Render the frame at its actual size relative to the sequence canvas (100%
        // scale, centered, cropped to the frame) — no fit-scaling. The user positions
        // and scales clips with transform controls (not built yet).
        const double scale = static_cast<double>(canvasRect.width()) / m_canvasW;
        const int drawW = static_cast<int>(std::lround(m_image.width() * scale));
        const int drawH = static_cast<int>(std::lround(m_image.height() * scale));
        const QRect target(canvasRect.center().x() - drawW / 2, canvasRect.center().y() - drawH / 2,
                           drawW, drawH);
        painter.setClipRect(canvasRect);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(target, m_image);
        painter.setClipping(false);
    }
}

}  // namespace hopline

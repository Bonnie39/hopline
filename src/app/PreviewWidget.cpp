#include "app/PreviewWidget.h"

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

void PreviewWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(16, 16, 16));

    if (m_image.isNull()) {
        painter.setPen(QColor(96, 96, 96));
        painter.drawText(rect(), Qt::AlignCenter, "preview");
        return;
    }

    QSize scaled = m_image.size();
    scaled.scale(size(), Qt::KeepAspectRatio);
    const QRect target(QPoint((width() - scaled.width()) / 2, (height() - scaled.height()) / 2), scaled);

    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(target, m_image);
}

}  // namespace hopline

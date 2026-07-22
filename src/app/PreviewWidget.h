#pragma once

#include <QImage>
#include <QWidget>

#include "media/VideoFrame.h"

namespace hopline {

class PreviewWidget : public QWidget {
    Q_OBJECT

public:
    explicit PreviewWidget(QWidget* parent = nullptr);

    void setFrame(const VideoFrame& frame);
    void clear();

    // The active sequence's resolution defines the canvas: the preview shows a
    // black frame of that aspect and fits each decoded frame inside it. (0,0 =
    // no sequence — shows a placeholder.)
    void setCanvas(int width, int height);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage m_image;
    int m_canvasW = 0;
    int m_canvasH = 0;
};

}  // namespace hopline

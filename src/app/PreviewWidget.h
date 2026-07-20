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

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage m_image;
};

}  // namespace hopline

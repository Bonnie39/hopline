#pragma once

#include <QWidget>

namespace hopline {

// A Premiere-style horizontal zoom scroll bar. The handle spans the visible
// range within the total scrollable extent; dragging its body scrolls and
// dragging either rounded end zooms. Works purely in seconds — the timeline
// owns the pixel mapping — and reports changes back via rangeChanged.
class TimelineScrollBar : public QWidget {
    Q_OBJECT

public:
    explicit TimelineScrollBar(QWidget* parent = nullptr);

    void setRange(double total, double start, double span);

signals:
    void rangeChanged(double start, double span);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    QSize sizeHint() const override;

private:
    enum class Mode { None, Scroll, ZoomStart, ZoomEnd };

    int trackLeft() const;
    int trackWidth() const;
    double secToX(double sec) const;
    double xToSec(double x) const;

    double m_total = 1.0;
    double m_start = 0.0;
    double m_span = 1.0;

    Mode m_mode = Mode::None;
    int m_pressX = 0;
    double m_pressStart = 0.0;
    double m_pressSpan = 0.0;
};

}  // namespace hopline

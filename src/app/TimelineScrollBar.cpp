#include "app/TimelineScrollBar.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace hopline {
namespace {

constexpr int kThickness = 16;
constexpr int kMargin = 2;      // groove inset from the widget edges
constexpr int kInsetY = 3;      // handle inset top/bottom
constexpr int kGrip = 8;        // px at each handle end that grabs the zoom
constexpr int kMinHandlePx = 20;

const QColor kGroove(26, 27, 30);
const QColor kHandle(78, 80, 86);
const QColor kHandleActive(104, 106, 114);
const QColor kKnob(176, 178, 186);
const QColor kKnobActive(214, 216, 224);

}  // namespace

TimelineScrollBar::TimelineScrollBar(QWidget* parent)
    : QWidget(parent)
{
    setFixedHeight(kThickness);
    setMouseTracking(true);
}

QSize TimelineScrollBar::sizeHint() const
{
    return QSize(0, kThickness);
}

int TimelineScrollBar::trackLeft() const
{
    return kMargin;
}

int TimelineScrollBar::trackWidth() const
{
    return std::max(1, width() - 2 * kMargin);
}

double TimelineScrollBar::secToX(double sec) const
{
    return trackLeft() + (sec / m_total) * trackWidth();
}

double TimelineScrollBar::xToSec(double x) const
{
    return (x - trackLeft()) / trackWidth() * m_total;
}

void TimelineScrollBar::setRange(double total, double start, double span)
{
    total = std::max(total, 1e-6);
    span = std::clamp(span, 1e-6, total);
    start = std::clamp(start, 0.0, std::max(0.0, total - span));
    if (total == m_total && start == m_start && span == m_span) {
        return;
    }
    m_total = total;
    m_start = start;
    m_span = span;
    update();
}

void TimelineScrollBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRect groove(trackLeft(), kInsetY, trackWidth(), height() - 2 * kInsetY);
    p.setPen(Qt::NoPen);
    p.setBrush(kGroove);
    p.drawRoundedRect(groove, 4, 4);

    int hx0 = static_cast<int>(std::round(secToX(m_start)));
    int hx1 = static_cast<int>(std::round(secToX(m_start + m_span)));
    if (hx1 - hx0 < kMinHandlePx) {
        hx1 = hx0 + kMinHandlePx;
    }
    const QRect handle(hx0, kInsetY, hx1 - hx0, height() - 2 * kInsetY);
    p.setBrush(m_mode != Mode::None ? kHandleActive : kHandle);
    p.drawRoundedRect(handle, 4, 4);

    // Circular grab knobs at each end mark the zoom grips.
    const double midY = height() / 2.0;
    const double r = (height() - 2 * kInsetY) / 2.0;
    p.setBrush(m_mode != Mode::None ? kKnobActive : kKnob);
    p.drawEllipse(QPointF(hx0 + r, midY), r, r);
    p.drawEllipse(QPointF(hx1 - r, midY), r, r);
}

void TimelineScrollBar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || trackWidth() <= 0) {
        return;
    }
    const int x = event->position().toPoint().x();
    int hx0 = static_cast<int>(std::round(secToX(m_start)));
    int hx1 = static_cast<int>(std::round(secToX(m_start + m_span)));
    if (hx1 - hx0 < kMinHandlePx) {
        hx1 = hx0 + kMinHandlePx;
    }

    if (x >= hx0 - kGrip && x <= hx0 + kGrip) {
        m_mode = Mode::ZoomStart;
    } else if (x >= hx1 - kGrip && x <= hx1 + kGrip) {
        m_mode = Mode::ZoomEnd;
    } else if (x > hx0 && x < hx1) {
        m_mode = Mode::Scroll;
    } else {
        // Click on the groove: jump so the handle centers on the click, then scroll.
        const double newStart = std::clamp(xToSec(x) - m_span / 2.0, 0.0, std::max(0.0, m_total - m_span));
        m_start = newStart;
        m_mode = Mode::Scroll;
        emit rangeChanged(m_start, m_span);
    }

    m_pressX = x;
    m_pressStart = m_start;
    m_pressSpan = m_span;
    update();
}

void TimelineScrollBar::mouseMoveEvent(QMouseEvent* event)
{
    const int x = event->position().toPoint().x();

    if (m_mode == Mode::None) {
        int hx0 = static_cast<int>(std::round(secToX(m_start)));
        int hx1 = static_cast<int>(std::round(secToX(m_start + m_span)));
        if (hx1 - hx0 < kMinHandlePx) {
            hx1 = hx0 + kMinHandlePx;
        }
        const bool onEnd = (x >= hx0 - kGrip && x <= hx0 + kGrip) || (x >= hx1 - kGrip && x <= hx1 + kGrip);
        setCursor(onEnd ? Qt::PointingHandCursor : Qt::ArrowCursor);  // knobs zoom, body scrolls
        return;
    }

    const double minSpan = std::max(m_total * 1e-4, 1e-4);
    const double ds = static_cast<double>(x - m_pressX) / trackWidth() * m_total;

    switch (m_mode) {
    case Mode::Scroll: {
        const double newStart = std::clamp(m_pressStart + ds, 0.0, std::max(0.0, m_total - m_span));
        emit rangeChanged(newStart, m_span);
        break;
    }
    case Mode::ZoomEnd: {
        const double newSpan = std::clamp(m_pressSpan + ds, minSpan, m_total - m_pressStart);
        emit rangeChanged(m_pressStart, newSpan);
        break;
    }
    case Mode::ZoomStart: {
        const double end = m_pressStart + m_pressSpan;
        const double newStart = std::clamp(m_pressStart + ds, 0.0, end - minSpan);
        emit rangeChanged(newStart, end - newStart);
        break;
    }
    default:
        break;
    }
}

void TimelineScrollBar::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }
    m_mode = Mode::None;
    update();
}

}  // namespace hopline

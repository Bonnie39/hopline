#include "app/TimelineWidget.h"

#include <QFileInfo>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "model/Project.h"

namespace hopline {
namespace {

constexpr int kHeaderWidth = 72;
constexpr int kRulerHeight = 24;
constexpr int kTrackHeight = 56;
constexpr int kTrackGap = 2;

const QColor kBackground(26, 26, 26);
const QColor kRulerBackground(34, 34, 34);
const QColor kHeaderBackground(30, 30, 30);
const QColor kLaneVideo(38, 38, 42);
const QColor kLaneAudio(36, 40, 38);
const QColor kGridLine(58, 58, 58);
const QColor kText(170, 170, 170);
const QColor kClipVideo(58, 106, 150);
const QColor kClipAudio(58, 140, 104);
const QColor kPlayhead(235, 80, 80);

// Ruler labels stay legible by stepping through a 1-2-5 sequence.
double niceInterval(double pixelsPerSecond)
{
    constexpr double kMinLabelSpacing = 80.0;
    const double target = kMinLabelSpacing / pixelsPerSecond;
    const double magnitude = std::pow(10.0, std::floor(std::log10(std::max(target, 1e-6))));
    for (double step : { 1.0, 2.0, 5.0, 10.0 }) {
        if (magnitude * step >= target) {
            return magnitude * step;
        }
    }
    return magnitude * 10.0;
}

QString timeLabel(double seconds)
{
    const int total = static_cast<int>(seconds);
    const int minutes = total / 60;
    const int secs = total % 60;
    const int frac = static_cast<int>((seconds - total) * 100 + 0.5);

    if (frac > 0) {
        return QString("%1:%2.%3").arg(minutes).arg(secs, 2, 10, QChar('0')).arg(frac, 2, 10, QChar('0'));
    }
    return QString("%1:%2").arg(minutes).arg(secs, 2, 10, QChar('0'));
}

}  // namespace

TimelineWidget::TimelineWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(kRulerHeight + kTrackHeight * 2 + 16);
    setMouseTracking(false);
    setFocusPolicy(Qt::StrongFocus);
}

void TimelineWidget::setProject(const Project* project)
{
    m_project = project;
    update();
}

void TimelineWidget::setPlayhead(Tick time)
{
    m_playhead = std::max<Tick>(0, time);

    // Called every UI tick; only repaint when it lands on a different pixel.
    const int x = xForTick(m_playhead);
    if (x != m_lastPlayheadX) {
        m_lastPlayheadX = x;
        update();
    }
}

void TimelineWidget::zoomToFit()
{
    if (!m_project) {
        return;
    }
    const double seconds = secondsFromTicks(m_project->sequence().duration());
    const int usable = width() - kHeaderWidth - 16;
    if (seconds > 0.0 && usable > 0) {
        m_pixelsPerSecond = usable / seconds;
        m_scrollSeconds = 0.0;
        m_lastPlayheadX = -1;
        update();
    }
}

int TimelineWidget::xForTick(Tick time) const
{
    return kHeaderWidth + static_cast<int>((secondsFromTicks(time) - m_scrollSeconds) * m_pixelsPerSecond);
}

Tick TimelineWidget::tickForX(int x) const
{
    const double seconds = (x - kHeaderWidth) / m_pixelsPerSecond + m_scrollSeconds;
    return ticksFromSeconds(std::max(0.0, seconds));
}

void TimelineWidget::scrubTo(int x)
{
    Tick time = tickForX(x);
    if (m_project) {
        // Land on a frame boundary: a playhead between frames isn't a real edit point.
        time = m_project->sequence().snapToFrame(time);
        time = std::min(time, m_project->sequence().duration());
    }
    setPlayhead(time);
    emit playheadDragged(m_playhead);
}

void TimelineWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && event->position().x() >= kHeaderWidth) {
        m_scrubbing = true;
        scrubTo(static_cast<int>(event->position().x()));
    }
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_scrubbing) {
        scrubTo(static_cast<int>(event->position().x()));
    }
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_scrubbing = false;
    }
}

void TimelineWidget::wheelEvent(QWheelEvent* event)
{
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0.0) {
        return;
    }

    if (event->modifiers() & Qt::ControlModifier) {
        // Zoom about the cursor, so whatever is under it stays put.
        const double anchorX = event->position().x();
        const double anchorSeconds = (anchorX - kHeaderWidth) / m_pixelsPerSecond + m_scrollSeconds;

        m_pixelsPerSecond = std::clamp(m_pixelsPerSecond * std::pow(1.2, steps), 1.0, 20000.0);
        m_scrollSeconds = anchorSeconds - (anchorX - kHeaderWidth) / m_pixelsPerSecond;
    } else {
        m_scrollSeconds -= steps * 40.0 / m_pixelsPerSecond;
    }

    m_scrollSeconds = std::max(0.0, m_scrollSeconds);
    m_lastPlayheadX = -1;
    update();
    event->accept();
}

void TimelineWidget::drawRuler(QPainter& painter)
{
    const QRect ruler(0, 0, width(), kRulerHeight);
    painter.fillRect(ruler, kRulerBackground);

    if (m_pixelsPerSecond <= 0.0) {
        return;
    }

    const double interval = niceInterval(m_pixelsPerSecond);
    const double first = std::floor(m_scrollSeconds / interval) * interval;
    const double last = m_scrollSeconds + (width() - kHeaderWidth) / m_pixelsPerSecond;

    painter.setPen(kText);
    QFont font = painter.font();
    font.setPointSizeF(7.5);
    painter.setFont(font);

    for (double t = first; t <= last; t += interval) {
        const int x = xForTick(ticksFromSeconds(t));
        if (x < kHeaderWidth) {
            continue;
        }
        painter.setPen(kGridLine);
        painter.drawLine(x, kRulerHeight - 6, x, kRulerHeight);
        painter.setPen(kText);
        painter.drawText(x + 3, kRulerHeight - 8, timeLabel(t));
    }

    painter.setPen(kGridLine);
    painter.drawLine(0, kRulerHeight - 1, width(), kRulerHeight - 1);
}

void TimelineWidget::drawTracks(QPainter& painter)
{
    if (!m_project) {
        return;
    }

    const Sequence& sequence = m_project->sequence();
    int y = kRulerHeight + kTrackGap;

    for (size_t i = 0; i < sequence.trackCount(); ++i) {
        const Track& track = sequence.track(i);
        const bool isVideo = track.kind() == Track::Kind::Video;
        const QRect lane(kHeaderWidth, y, width() - kHeaderWidth, kTrackHeight);

        painter.fillRect(QRect(0, y, kHeaderWidth, kTrackHeight), kHeaderBackground);
        painter.fillRect(lane, isVideo ? kLaneVideo : kLaneAudio);

        painter.setPen(kText);
        painter.drawText(QRect(0, y, kHeaderWidth - 8, kTrackHeight),
                         Qt::AlignRight | Qt::AlignVCenter, QString::fromStdString(track.name()));

        for (const Clip& clip : track.clips()) {
            const int x0 = xForTick(clip.timelineStart);
            const int x1 = xForTick(clip.range().end());
            if (x1 < kHeaderWidth || x0 > width()) {
                continue;
            }

            QRect box(std::max(x0, kHeaderWidth), y + 3, x1 - std::max(x0, kHeaderWidth), kTrackHeight - 6);
            if (box.width() < 1) {
                box.setWidth(1);
            }

            const QColor fill = isVideo ? kClipVideo : kClipAudio;
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawRoundedRect(box, 3, 3);

            painter.setPen(fill.lighter(140));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(box, 3, 3);

            if (box.width() > 40) {
                QString label = "clip";
                if (const MediaSource* media = m_project->media(clip.source)) {
                    label = QFileInfo(QString::fromStdString(media->path)).fileName();
                }
                painter.setPen(QColor(235, 235, 235));
                painter.drawText(box.adjusted(6, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft, label);
            }
        }

        painter.setPen(kGridLine);
        painter.drawLine(0, y + kTrackHeight, width(), y + kTrackHeight);
        y += kTrackHeight + kTrackGap;
    }
}

void TimelineWidget::drawPlayhead(QPainter& painter)
{
    const int x = xForTick(m_playhead);
    if (x < kHeaderWidth) {
        return;
    }

    painter.setPen(QPen(kPlayhead, 1));
    painter.drawLine(x, 0, x, height());

    // Small handle so it stays findable when zoomed out.
    const QPolygon handle({ QPoint(x - 4, 0), QPoint(x + 4, 0), QPoint(x, 7) });
    painter.setPen(Qt::NoPen);
    painter.setBrush(kPlayhead);
    painter.drawPolygon(handle);
}

void TimelineWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), kBackground);

    drawTracks(painter);
    drawRuler(painter);
    drawPlayhead(painter);
}

}  // namespace hopline

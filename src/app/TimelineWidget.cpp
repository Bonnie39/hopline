#include "app/TimelineWidget.h"

#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QMenu>
#include <QMimeData>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QUrl>
#include <QWheelEvent>

#include "app/MediaBrowser.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "app/PreviewCache.h"
#include "model/Project.h"

namespace hopline {
namespace {

constexpr int kHeaderWidth = 72;
constexpr int kRulerHeight = 24;
constexpr int kTrackHeight = 56;
constexpr int kTrackGap = 2;
constexpr int kTrimHandlePx = 6;
constexpr int kDragThresholdPx = 3;

const QColor kBackground(26, 26, 26);
const QColor kRulerBackground(34, 34, 34);
const QColor kHeaderBackground(30, 30, 30);
const QColor kLaneVideo(38, 38, 42);
const QColor kLaneAudio(36, 40, 38);
const QColor kGridLine(58, 58, 58);
const QColor kText(170, 170, 170);
const QColor kClipVideo(58, 106, 150);
const QColor kClipAudio(58, 140, 104);
const QColor kSelected(240, 200, 90);
const QColor kGhost(150, 150, 150);
const QColor kWaveform(150, 225, 190);
const QColor kPlayhead(235, 80, 80);

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
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);  // so the cursor can react to the trim zone without a button down
    setAcceptDrops(true);
}

void TimelineWidget::setProject(const Project* project)
{
    m_project = project;
    update();
}

void TimelineWidget::setPlayhead(Tick time)
{
    m_playhead = std::max<Tick>(0, time);
    const int x = xForTick(m_playhead);
    if (x != m_lastPlayheadX) {
        m_lastPlayheadX = x;
        update();
    }
}

void TimelineWidget::clearSelection()
{
    if (m_selected != kInvalidClip) {
        m_selected = kInvalidClip;
        emit selectionChanged(kInvalidClip);
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

int TimelineWidget::trackTop(std::size_t index) const
{
    return kRulerHeight + kTrackGap + static_cast<int>(index) * (kTrackHeight + kTrackGap);
}

TimelineWidget::Hit TimelineWidget::hitTest(const QPoint& pos) const
{
    Hit hit;
    if (pos.x() < kHeaderWidth) {
        return hit;
    }
    if (pos.y() < kRulerHeight) {
        hit.onRuler = true;
        return hit;
    }
    if (!m_project) {
        return hit;
    }

    const Sequence& sequence = m_project->sequence();
    for (std::size_t i = 0; i < sequence.trackCount(); ++i) {
        const int top = trackTop(i);
        if (pos.y() < top || pos.y() >= top + kTrackHeight) {
            continue;
        }
        for (const Clip& clip : sequence.track(i).clips()) {
            const int x0 = xForTick(clip.timelineStart);
            const int x1 = xForTick(clip.range().end());
            if (pos.x() < x0 || pos.x() > x1) {
                continue;
            }
            hit.onClip = true;
            hit.trackIndex = i;
            hit.clip = clip.id;
            const bool wideEnough = (x1 - x0) > kTrimHandlePx * 3;
            if (wideEnough && pos.x() - x0 <= kTrimHandlePx) {
                hit.edge = 0;
            } else if (wideEnough && x1 - pos.x() <= kTrimHandlePx) {
                hit.edge = 1;
            } else {
                hit.edge = -1;
            }
            return hit;
        }
        return hit;  // empty area of this track
    }
    return hit;
}

Tick TimelineWidget::snapDelta(Tick raw) const
{
    const Tick frame = m_project ? m_project->sequence().frameDuration() : 1;
    if (frame <= 0) {
        return raw;
    }
    return std::llround(static_cast<double>(raw) / frame) * frame;
}

std::vector<ClipId> TimelineWidget::affectedByDrag() const
{
    std::vector<ClipId> ids;
    if (!m_project || m_dragClip == kInvalidClip) {
        return ids;
    }
    const Clip* clip = m_project->sequence().findClip(m_dragClip);
    if (clip && clip->linked()) {
        for (const auto& [track, id] : m_project->sequence().clipsInGroup(clip->linkGroup)) {
            ids.push_back(id);
        }
    } else {
        ids.push_back(m_dragClip);
    }
    return ids;
}

// Preview must stop exactly where the edit would be rejected, so the clip never
// extends past its limit and snaps back. Constrains delta against every affected
// clip's timeline start, minimum duration, and source media bounds.
Tick TimelineWidget::clampMoveDelta(Tick delta) const
{
    if (!m_project) {
        return delta;
    }
    Tick lo = std::numeric_limits<Tick>::min();
    for (ClipId id : affectedByDrag()) {
        if (const Clip* c = m_project->sequence().findClip(id)) {
            lo = std::max(lo, -c->timelineStart);  // keep every member's start >= 0
        }
    }
    return std::max(delta, lo);
}

Tick TimelineWidget::clampTrimDelta(Tick delta, bool trimHead) const
{
    if (!m_project) {
        return delta;
    }
    const Sequence& sequence = m_project->sequence();
    const Tick frame = sequence.frameDuration();

    Tick lo = std::numeric_limits<Tick>::min();
    Tick hi = std::numeric_limits<Tick>::max();

    for (ClipId id : affectedByDrag()) {
        const Clip* c = sequence.findClip(id);
        if (!c) {
            continue;
        }
        if (trimHead) {
            // + shortens (dur -= delta); - extends left into unused source.
            hi = std::min(hi, c->duration - frame);
            lo = std::max(lo, std::max(-c->timelineStart, -c->sourceIn));
        } else {
            // - shortens; + extends right, bounded by remaining source.
            lo = std::max(lo, frame - c->duration);
            const MediaSource* media = m_project->media(c->source);
            const Tick sourceEnd = media ? media->duration : c->sourceIn + c->duration;
            hi = std::min(hi, sourceEnd - (c->sourceIn + c->duration));
        }
    }

    if (lo > hi) {
        return 0;
    }
    return std::clamp(delta, lo, hi);
}

void TimelineWidget::updateHoverCursor(const QPoint& pos)
{
    // Only the trim edges get a custom cursor; the body keeps the default arrow.
    const Hit hit = hitTest(pos);
    if (hit.onClip && (hit.edge == 0 || hit.edge == 1)) {
        setCursor(Qt::SizeHorCursor);
    } else {
        unsetCursor();
    }
}

void TimelineWidget::scrubTo(int x)
{
    Tick time = tickForX(x);
    if (m_project) {
        time = m_project->sequence().snapToFrame(time);
        time = std::min(time, m_project->sequence().duration());
    }
    setPlayhead(time);
    emit playheadDragged(m_playhead);
}

void TimelineWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const QPoint pos = event->position().toPoint();
    const Hit hit = hitTest(pos);

    if (hit.onRuler) {
        m_drag = Drag::Scrub;
        scrubTo(pos.x());
        return;
    }

    if (hit.onClip) {
        if (m_selected != hit.clip) {
            m_selected = hit.clip;
            emit selectionChanged(m_selected);
        }
        m_dragTrack = hit.trackIndex;
        m_dragClip = hit.clip;
        if (const Clip* clip = m_project->sequence().findClip(hit.clip)) {
            m_dragOrigStart = clip->timelineStart;
            m_dragOrigDuration = clip->duration;
        }
        m_drag = hit.edge == 0 ? Drag::TrimHead : hit.edge == 1 ? Drag::TrimTail : Drag::Move;
        if (m_drag != Drag::Move) {
            setCursor(Qt::SizeHorCursor);  // trim only; moving keeps the default cursor
        }
        m_previewDelta = 0;
        m_pressX = pos.x();
        m_dragMoved = false;
        update();
        return;
    }

    clearSelection();
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint pos = event->position().toPoint();

    if (m_drag == Drag::Scrub) {
        scrubTo(pos.x());
        return;
    }
    if (m_drag == Drag::None) {
        updateHoverCursor(pos);
        return;
    }

    const int dxPixels = pos.x() - m_pressX;
    if (std::abs(dxPixels) > kDragThresholdPx) {
        m_dragMoved = true;
    }

    Tick delta = snapDelta(ticksFromSeconds(dxPixels / m_pixelsPerSecond));
    switch (m_drag) {
    case Drag::Move:
        delta = clampMoveDelta(delta);
        break;
    case Drag::TrimHead:
        delta = clampTrimDelta(delta, true);
        break;
    case Drag::TrimTail:
        delta = clampTrimDelta(delta, false);
        break;
    default:
        break;
    }

    m_previewDelta = delta;
    update();
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }

    if (m_drag != Drag::None && m_drag != Drag::Scrub && m_dragMoved && m_previewDelta != 0) {
        commitDrag();
    }

    m_drag = Drag::None;
    m_previewDelta = 0;
    m_dragClip = kInvalidClip;
    updateHoverCursor(event->position().toPoint());
    update();
}

void TimelineWidget::commitDrag()
{
    switch (m_drag) {
    case Drag::Move:
        emit clipMoved(m_dragTrack, m_dragClip, m_dragOrigStart + m_previewDelta);
        break;
    case Drag::TrimHead:
        emit clipTrimmed(m_dragTrack, m_dragClip, true, m_previewDelta);
        break;
    case Drag::TrimTail:
        emit clipTrimmed(m_dragTrack, m_dragClip, false, m_previewDelta);
        break;
    default:
        break;
    }
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* event)
{
    const Hit hit = hitTest(event->pos());
    if (!hit.onClip || !m_project) {
        return;
    }

    if (m_selected != hit.clip) {
        m_selected = hit.clip;
        emit selectionChanged(m_selected);
        update();
    }

    const Clip* clip = m_project->sequence().findClip(hit.clip);
    QMenu menu(this);
    QAction* unlink = menu.addAction("Unlink");
    unlink->setEnabled(clip && clip->linked());
    menu.addSeparator();
    QAction* remove = menu.addAction("Delete Clip");

    QAction* chosen = menu.exec(event->globalPos());
    if (chosen == unlink) {
        emit unlinkRequested(hit.clip);
    } else if (chosen == remove) {
        emit deleteRequested(hit.trackIndex, hit.clip);
    }
}

int TimelineWidget::firstTrackOfKind(bool video) const
{
    if (!m_project) {
        return -1;
    }
    const auto want = video ? Track::Kind::Video : Track::Kind::Audio;
    for (std::size_t i = 0; i < m_project->sequence().trackCount(); ++i) {
        if (m_project->sequence().track(i).kind() == want) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void TimelineWidget::updateDropGhost(const QPoint& pos, const QMimeData* mime)
{
    m_dropActive = false;
    if (m_project && mime->hasFormat(kMediaMimeType)) {
        const MediaId id = mime->data(kMediaMimeType).toULongLong();
        if (const MediaSource* media = m_project->media(id)) {
            m_dropStart = std::max<Tick>(0, m_project->sequence().snapToFrame(tickForX(pos.x())));
            m_dropDuration = media->duration;
            m_dropVideo = media->hasVideo;
            m_dropAudio = media->hasAudio;
            m_dropActive = true;
        }
    }
    update();
}

void TimelineWidget::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasFormat(kMediaMimeType) || event->mimeData()->hasUrls()) {
        updateDropGhost(event->position().toPoint(), event->mimeData());
        event->acceptProposedAction();
    }
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent* event)
{
    updateDropGhost(event->position().toPoint(), event->mimeData());
    event->acceptProposedAction();
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent*)
{
    m_dropActive = false;
    update();
}

void TimelineWidget::dropEvent(QDropEvent* event)
{
    m_dropActive = false;

    const int x = event->position().toPoint().x();
    Tick start = tickForX(x);
    if (m_project) {
        start = m_project->sequence().snapToFrame(start);
    }
    start = std::max<Tick>(0, start);

    if (event->mimeData()->hasFormat(kMediaMimeType)) {
        const MediaId id = event->mimeData()->data(kMediaMimeType).toULongLong();
        emit mediaDropped(id, start);
        event->acceptProposedAction();
    } else if (event->mimeData()->hasUrls()) {
        for (const QUrl& url : event->mimeData()->urls()) {
            if (url.isLocalFile()) {
                emit fileDropped(url.toLocalFile(), start);
            }
        }
        event->acceptProposedAction();
    }
    update();
}

void TimelineWidget::wheelEvent(QWheelEvent* event)
{
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0.0) {
        return;
    }

    if (event->modifiers() & Qt::ControlModifier) {
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
    painter.fillRect(QRect(0, 0, width(), kRulerHeight), kRulerBackground);
    if (m_pixelsPerSecond <= 0.0) {
        return;
    }

    const double interval = niceInterval(m_pixelsPerSecond);
    const double first = std::floor(m_scrollSeconds / interval) * interval;
    const double last = m_scrollSeconds + (width() - kHeaderWidth) / m_pixelsPerSecond;

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
    const bool dragging = (m_drag == Drag::Move || m_drag == Drag::TrimHead || m_drag == Drag::TrimTail);
    const std::vector<ClipId> affected = dragging ? affectedByDrag() : std::vector<ClipId>{};

    auto isAffected = [&](ClipId id) {
        return std::find(affected.begin(), affected.end(), id) != affected.end();
    };
    auto isHighlighted = [&](const Clip& clip) {
        if (m_selected == kInvalidClip) {
            return false;
        }
        if (clip.id == m_selected) {
            return true;
        }
        const Clip* sel = sequence.findClip(m_selected);
        return sel && sel->linked() && sel->linkGroup == clip.linkGroup;
    };

    for (std::size_t i = 0; i < sequence.trackCount(); ++i) {
        const Track& track = sequence.track(i);
        const bool isVideo = track.kind() == Track::Kind::Video;
        const int y = trackTop(i);

        painter.fillRect(QRect(0, y, kHeaderWidth, kTrackHeight), kHeaderBackground);
        painter.fillRect(QRect(kHeaderWidth, y, width() - kHeaderWidth, kTrackHeight),
                         isVideo ? kLaneVideo : kLaneAudio);

        painter.setPen(kText);
        painter.drawText(QRect(0, y, kHeaderWidth - 8, kTrackHeight),
                         Qt::AlignRight | Qt::AlignVCenter, QString::fromStdString(track.name()));

        for (const Clip& clip : track.clips()) {
            Tick start = clip.timelineStart;
            Tick duration = clip.duration;
            Tick sourceIn = clip.sourceIn;
            if (dragging && isAffected(clip.id)) {
                if (m_drag == Drag::Move) {
                    start += m_previewDelta;
                } else if (m_drag == Drag::TrimHead) {
                    start += m_previewDelta;
                    duration -= m_previewDelta;
                    sourceIn += m_previewDelta;  // head trim advances into the source
                } else {
                    duration += m_previewDelta;
                }
            }

            const int x0 = xForTick(start);
            const int x1 = xForTick(start + duration);
            if (x1 < kHeaderWidth || x0 > width()) {
                continue;
            }

            // While trimming, outline the pre-trim extent so you can see what's
            // being cut away (or how far it was before extending).
            const bool trimming = (m_drag == Drag::TrimHead || m_drag == Drag::TrimTail);
            if (trimming && isAffected(clip.id)) {
                const int gx0 = std::max(xForTick(clip.timelineStart), kHeaderWidth);
                const int gx1 = xForTick(clip.range().end());
                QRect ghost(gx0, y + 3, gx1 - gx0, kTrackHeight - 6);
                QPen ghostPen(kGhost, 1, Qt::DashLine);
                painter.setPen(ghostPen);
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(ghost, 3, 3);
            }

            QRect box(std::max(x0, kHeaderWidth), y + 3, x1 - std::max(x0, kHeaderWidth), kTrackHeight - 6);
            if (box.width() < 1) {
                box.setWidth(1);
            }

            const QColor fill = isVideo ? kClipVideo : kClipAudio;
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawRoundedRect(box, 3, 3);

            if (m_preview && box.width() > 4) {
                const int x0f = xForTick(start);
                const int fullW = xForTick(start + duration) - x0f;
                const double srcStartSec = secondsFromTicks(sourceIn);
                const double srcSpanSec = secondsFromTicks(duration);
                if (isVideo) {
                    drawThumbnails(painter, clip, box, x0f, fullW, srcStartSec, srcSpanSec);
                } else {
                    drawWaveform(painter, clip, box, x0f, fullW, srcStartSec, srcSpanSec);
                }
            }

            if (isHighlighted(clip)) {
                painter.setPen(QPen(kSelected, 2));
            } else {
                painter.setPen(fill.lighter(140));
            }
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(box, 3, 3);

            if (box.width() > 40) {
                QString label = "clip";
                if (const MediaSource* media = m_project->media(clip.source)) {
                    label = QFileInfo(QString::fromStdString(media->path)).fileName();
                }
                const QFontMetrics fm(painter.font());
                const QString elided = fm.elidedText(label, Qt::ElideRight, box.width() - 12);
                const QRect textRect = fm.boundingRect(elided).adjusted(0, 0, 6, 2);
                // Dark pill so the name stays legible over thumbnails.
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(0, 0, 0, 120));
                painter.drawRoundedRect(QRect(box.left() + 3, box.top() + 3, textRect.width(), fm.height() + 2), 2, 2);
                painter.setPen(QColor(235, 235, 235));
                painter.drawText(box.left() + 6, box.top() + 3 + fm.ascent() + 1, elided);
            }
        }

        painter.setPen(kGridLine);
        painter.drawLine(0, y + kTrackHeight, width(), y + kTrackHeight);
    }
}

void TimelineWidget::drawThumbnails(QPainter& painter, const Clip& clip, const QRect& box, int x0,
                                    int fullWidth, double srcStart, double srcSpan)
{
    const PreviewCache::Thumbnails* thumbs = m_preview->thumbnails(clip.source);
    if (!thumbs || thumbs->images.empty() || fullWidth <= 0) {
        return;
    }
    const int tileW = thumbs->images.front().width();
    if (tileW <= 0) {
        return;
    }

    painter.save();
    painter.setClipRect(box);
    for (int px = box.left(); px < box.right(); px += tileW) {
        const double fraction = std::clamp((px + tileW / 2.0 - x0) / fullWidth, 0.0, 1.0);
        const double srcSec = srcStart + fraction * srcSpan;
        int idx = static_cast<int>(srcSec / thumbs->interval);
        idx = std::clamp(idx, 0, static_cast<int>(thumbs->images.size()) - 1);
        painter.drawImage(QRect(px, box.top(), tileW, box.height()), thumbs->images[idx]);
    }
    painter.restore();
}

void TimelineWidget::drawWaveform(QPainter& painter, const Clip& clip, const QRect& box, int x0,
                                  int fullWidth, double srcStart, double srcSpan)
{
    const PreviewCache::Waveform* wave = m_preview->waveform(clip.source);
    if (!wave || wave->peaks.empty() || fullWidth <= 0) {
        return;
    }

    const int centerY = box.center().y();
    const double halfH = box.height() / 2.0 - 2.0;
    painter.save();
    painter.setClipRect(box);
    painter.setPen(kWaveform);
    for (int x = box.left(); x < box.right(); ++x) {
        const double fraction = std::clamp((x - x0 + 0.5) / fullWidth, 0.0, 1.0);
        const double srcSec = srcStart + fraction * srcSpan;
        const size_t bucket = static_cast<size_t>(srcSec * wave->bucketsPerSecond);
        const float peak = bucket < wave->peaks.size() ? wave->peaks[bucket] : 0.0f;
        const int h = static_cast<int>(peak * halfH);
        painter.drawLine(x, centerY - h, x, centerY + h);
    }
    painter.restore();
}

void TimelineWidget::drawDropGhost(QPainter& painter)
{
    if (!m_dropActive) {
        return;
    }
    const int x0 = std::max(xForTick(m_dropStart), kHeaderWidth);
    const int x1 = xForTick(m_dropStart + m_dropDuration);
    if (x1 <= kHeaderWidth) {
        return;
    }

    QPen pen(kSelected, 1, Qt::DashLine);
    painter.setPen(pen);
    painter.setBrush(QColor(kSelected.red(), kSelected.green(), kSelected.blue(), 40));

    auto drawLane = [&](int trackIndex) {
        if (trackIndex < 0) {
            return;
        }
        const int y = trackTop(static_cast<std::size_t>(trackIndex));
        painter.drawRoundedRect(QRect(x0, y + 3, x1 - x0, kTrackHeight - 6), 3, 3);
    };
    if (m_dropVideo) {
        drawLane(firstTrackOfKind(true));
    }
    if (m_dropAudio) {
        drawLane(firstTrackOfKind(false));
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
    drawDropGhost(painter);
    drawRuler(painter);
    drawPlayhead(painter);
}

}  // namespace hopline

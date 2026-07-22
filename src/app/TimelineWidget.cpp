#include "app/TimelineWidget.h"

#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QIcon>
#include <QMenu>
#include <QMimeData>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QResizeEvent>
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
constexpr int kDefaultTrackHeight = 56;
constexpr int kMinTrackHeight = 16;   // ~just the clip name strip
constexpr int kMaxTrackHeight = 260;
constexpr int kTrackGap = 2;
constexpr int kTrimHandlePx = 6;
constexpr int kDragThresholdPx = 3;
constexpr int kVBarWidth = 14;      // vertical zoom-bar gutter on the right
constexpr int kVBarMinHandle = 20;  // min handle length in px
constexpr int kClipLabelStrip = 15;  // colored name strip at the top of a clip
constexpr int kDividerGrab = 4;      // px around a header divider that grabs it
constexpr int kMinSection = 40;      // keep both A/V sections at least this tall

// Neutral shades matching the app palette; the track area is a darker "well" than
// the surrounding panels, and the lanes carry no blue/green tint.
const QColor kBackground(16, 17, 19);       // timeline canvas — darker than panels
const QColor kRulerBackground(32, 33, 36);  // ruler chrome
const QColor kHeaderBackground(24, 25, 27);  // track-name column (panel shade)
const QColor kLaneVideo(22, 23, 25);        // track row, neutral
const QColor kLaneAudio(22, 23, 25);        // same — no green tint
const QColor kGridLine(44, 45, 49);
const QColor kHeaderDivider(50, 52, 57);  // grabbable track edge in the header (thin, subtle)
const QColor kAVDivider(62, 64, 70);      // grabbable A/V split, across the whole timeline
const QColor kText(165, 166, 170);
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
    setMinimumHeight(kRulerHeight + kDefaultTrackHeight * 2 + 16);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);  // so the cursor can react to the trim zone without a button down
    setAcceptDrops(true);
}

void TimelineWidget::setProject(const Project* project)
{
    m_project = project;
    update();
    emit viewChanged();
}

double TimelineWidget::viewStart() const
{
    return m_scrollSeconds;
}

double TimelineWidget::viewSpan() const
{
    const int usable = contentRight() - kHeaderWidth;
    return usable > 0 ? usable / m_pixelsPerSecond : 0.0;
}

double TimelineWidget::viewTotal() const
{
    double dur = 0.0;
    if (m_project) {
        dur = secondsFromTicks(m_project->sequence().duration());
    }
    return std::max(dur, viewStart() + viewSpan());
}

void TimelineWidget::setView(double start, double span)
{
    const int usable = contentRight() - kHeaderWidth;
    if (usable <= 0 || span <= 1e-6) {
        return;
    }
    m_pixelsPerSecond = std::clamp(usable / span, minPixelsPerSecond(), 20000.0);
    m_scrollSeconds = std::max(0.0, start);
    m_lastPlayheadX = -1;
    update();
    emit viewChanged();  // reflect clamping back to the scroll bar
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
    const int usable = contentRight() - kHeaderWidth - 16;
    if (seconds > 0.0 && usable > 0) {
        m_pixelsPerSecond = usable / seconds;
        m_scrollSeconds = 0.0;
        m_lastPlayheadX = -1;
        update();
        emit viewChanged();
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

int TimelineWidget::contentRight() const
{
    return width() - kVBarWidth;
}

void TimelineWidget::syncTrackHeights()
{
    const std::size_t n = m_project ? m_project->sequence().trackCount() : 0;
    if (m_trackH.size() != n) {
        m_trackH.resize(n, kDefaultTrackHeight);  // new tracks default; extras dropped
    }
}

int TimelineWidget::trackHeightAt(std::size_t index) const
{
    return index < m_trackH.size() ? m_trackH[index] : kDefaultTrackHeight;
}

int TimelineWidget::trackAtLevel(bool video, int level) const
{
    if (!m_project) {
        return -1;
    }
    const Sequence& seq = m_project->sequence();
    int l = 0;
    for (std::size_t i = 0; i < seq.trackCount(); ++i) {
        if ((seq.track(i).kind() == Track::Kind::Video) == video) {
            if (l == level) {
                return static_cast<int>(i);
            }
            ++l;
        }
    }
    return -1;
}

int TimelineWidget::heightOfLevel(bool video, int level) const
{
    const int idx = trackAtLevel(video, level);
    return idx >= 0 ? trackHeightAt(static_cast<std::size_t>(idx)) : kDefaultTrackHeight;
}

int TimelineWidget::sectionTrackCount(bool video) const
{
    if (!m_project) {
        return 0;
    }
    const Sequence& seq = m_project->sequence();
    int n = 0;
    for (std::size_t i = 0; i < seq.trackCount(); ++i) {
        if ((seq.track(i).kind() == Track::Kind::Video) == video) {
            ++n;
        }
    }
    return n;
}

int TimelineWidget::sectionContentHeight(bool video) const
{
    int total = 0, n = 0;
    if (m_project) {
        const Sequence& seq = m_project->sequence();
        for (std::size_t i = 0; i < seq.trackCount(); ++i) {
            if ((seq.track(i).kind() == Track::Kind::Video) == video) {
                total += trackHeightAt(i);
                ++n;
            }
        }
    }
    return n > 0 ? total + (n - 1) * kTrackGap : 0;
}

int TimelineWidget::sectionViewport(bool video) const
{
    const TrackLayout lay = trackLayout();
    return std::max(0, video ? lay.dividerY - kRulerHeight : height() - lay.dividerY);
}

double TimelineWidget::maxScroll(bool video) const
{
    return std::max(0.0, static_cast<double>(sectionContentHeight(video) - sectionViewport(video)));
}

void TimelineWidget::clampScrolls()
{
    m_videoScroll = std::clamp(m_videoScroll, 0.0, maxScroll(true));
    m_audioScroll = std::clamp(m_audioScroll, 0.0, maxScroll(false));
}

// The longer the timeline, the further out we can zoom — always at least far
// enough to fit the whole sequence in the visible width.
double TimelineWidget::minPixelsPerSecond() const
{
    const int usable = contentRight() - kHeaderWidth;
    const double dur = m_project ? secondsFromTicks(m_project->sequence().duration()) : 0.0;
    if (usable <= 0 || dur <= 0.0) {
        return 1.0;
    }
    return std::min(1.0, usable / dur);
}

int TimelineWidget::trackTop(std::size_t index) const
{
    if (!m_project) {
        return kRulerHeight;
    }
    const bool video = m_project->sequence().track(index).kind() == Track::Kind::Video;
    return levelToY(levelOfTrack(index), video);
}

TimelineWidget::TrackLayout TimelineWidget::trackLayout() const
{
    // The divider sits where the user dragged it (default center); the two sections
    // grow outward from it and scroll independently when they overflow.
    TrackLayout lay;
    const int avail = std::max(0, height() - kRulerHeight);
    int div = kRulerHeight + static_cast<int>(std::lround(m_dividerFrac * avail));
    // Keep both sections visible; fall back to the middle when the widget is tiny.
    const int lo = kRulerHeight + std::min(kMinSection, avail / 2);
    const int hi = kRulerHeight + std::max(avail / 2, avail - kMinSection);
    lay.dividerY = std::clamp(div, lo, hi);
    lay.videoBottom = lay.audioTop = lay.dividerY;
    return lay;
}

int TimelineWidget::trackAtY(int y) const
{
    if (!m_project) {
        return -1;
    }
    const TrackLayout lay = trackLayout();
    for (std::size_t i = 0; i < m_project->sequence().trackCount(); ++i) {
        const bool video = m_project->sequence().track(i).kind() == Track::Kind::Video;
        if (video ? (y >= lay.dividerY) : (y < lay.dividerY)) {
            continue;  // clipped to the wrong section
        }
        const int top = trackTop(i);
        if (y >= top && y < top + trackHeightAt(i)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int TimelineWidget::levelOfTrack(std::size_t index) const
{
    if (!m_project) {
        return 0;
    }
    const Sequence& seq = m_project->sequence();
    const bool video = seq.track(index).kind() == Track::Kind::Video;
    int level = 0;
    for (std::size_t i = 0; i < index; ++i) {
        if ((seq.track(i).kind() == Track::Kind::Video) == video) {
            ++level;
        }
    }
    return level;
}

int TimelineWidget::levelToY(int level, bool video) const
{
    const TrackLayout lay = trackLayout();
    int cum = 0;  // sum of (height + gap) for the levels nearer the divider
    for (int l = 0; l < level; ++l) {
        cum += heightOfLevel(video, l) + kTrackGap;
    }
    return video ? lay.dividerY - cum - heightOfLevel(video, level) + static_cast<int>(std::lround(m_videoScroll))
                 : lay.dividerY + cum - static_cast<int>(std::lround(m_audioScroll));
}

int TimelineWidget::levelForY(int y, bool video) const
{
    // Walk levels out from the divider (heights vary per track) and return the one
    // whose band contains y, extrapolating past the last track with the default height.
    const TrackLayout lay = trackLayout();
    if (video) {
        double bottom = lay.dividerY + m_videoScroll;  // level 0's bottom edge
        for (int l = 0; l < 512; ++l) {
            const int h = heightOfLevel(video, l);
            const double top = bottom - h;
            if (y >= top - kTrackGap / 2.0) {
                return l;
            }
            bottom = top - kTrackGap;
        }
        return 511;
    }
    double top = lay.dividerY - m_audioScroll;  // level 0's top edge
    for (int l = 0; l < 512; ++l) {
        const int h = heightOfLevel(video, l);
        const double bottom = top + h;
        if (y <= bottom + kTrackGap / 2.0) {
            return l;
        }
        top = bottom + kTrackGap;
    }
    return 511;
}

TimelineWidget::Hit TimelineWidget::hitTest(const QPoint& pos) const
{
    Hit hit;
    if (pos.x() < kHeaderWidth || pos.x() >= contentRight()) {
        return hit;  // header column or vertical-bar gutter
    }
    if (pos.y() < kRulerHeight) {
        hit.onRuler = true;
        return hit;
    }
    if (!m_project) {
        return hit;
    }

    const Sequence& sequence = m_project->sequence();
    const TrackLayout lay = trackLayout();
    for (std::size_t i = 0; i < sequence.trackCount(); ++i) {
        const bool video = sequence.track(i).kind() == Track::Kind::Video;
        if (video ? (pos.y() >= lay.dividerY) : (pos.y() < lay.dividerY)) {
            continue;  // clipped to the wrong section
        }
        const int top = trackTop(i);
        if (pos.y() < top || pos.y() >= top + trackHeightAt(i)) {
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
        const Tick dur = m_project->sequence().duration();
        if (dur > 0) {
            time = std::min(time, dur);  // don't scrub past content when there is any
        }
    }
    time = std::max<Tick>(0, time);
    setPlayhead(time);
    emit playheadDragged(m_playhead);
}

void TimelineWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        m_drag = Drag::Pan;
        m_panStartX = event->position().toPoint().x();
        m_panStartScroll = m_scrollSeconds;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const QPoint pos = event->position().toPoint();

    // Vertical zoom-bar gutter on the right: scroll (body) or zoom track height (ends).
    if (pos.x() >= contentRight() && pos.y() >= kRulerHeight && m_project && m_project->hasActiveSequence()) {
        syncTrackHeights();
        const bool video = pos.y() < trackLayout().dividerY;
        if (sectionTrackCount(video) <= 0) {
            return;
        }
        const QRect h = vbarHandle(video);
        m_vdragVideo = video;
        m_vPressY = pos.y();
        m_vPressScroll = video ? m_videoScroll : m_audioScroll;
        m_vPressHeights.clear();
        {
            const Sequence& seq = m_project->sequence();
            for (std::size_t i = 0; i < seq.trackCount(); ++i) {
                if ((seq.track(i).kind() == Track::Kind::Video) == video) {
                    m_vPressHeights.push_back(trackHeightAt(i));
                }
            }
        }
        m_vPressHandleTop = h.top();
        m_vPressHandleBottom = h.bottom();
        constexpr int kGripV = 8;
        if (pos.y() <= h.top() + kGripV) {
            m_vdrag = VDrag::ZoomTop;
        } else if (pos.y() >= h.bottom() - kGripV) {
            m_vdrag = VDrag::ZoomBottom;
        } else if (pos.y() > h.top() && pos.y() < h.bottom()) {
            m_vdrag = VDrag::Scroll;
        } else {
            // Click on the groove: jump so the handle centers on the click, then scroll.
            const QRect bar = vbarRect(video);
            const double contentH = sectionContentHeight(video);
            const double viewport = sectionViewport(video);
            if (contentH > viewport && bar.height() > 0) {
                double windowTop = (double(pos.y() - bar.top()) / bar.height()) * contentH - viewport / 2.0;
                windowTop = std::clamp(windowTop, 0.0, contentH - viewport);
                if (video) {
                    m_videoScroll = (contentH - viewport) - windowTop;
                } else {
                    m_audioScroll = windowTop;
                }
                clampScrolls();
            }
            m_vdrag = VDrag::Scroll;
            m_vPressScroll = video ? m_videoScroll : m_audioScroll;
        }
        setCursor(m_vdrag == VDrag::Scroll ? Qt::ArrowCursor : Qt::PointingHandCursor);
        update();
        return;
    }

    // Grabbable dividers on the header column (A/V split, or a track's edge).
    const DividerHit dh = dividerHitTest(pos);
    if (dh.kind != DividerKind::None) {
        syncTrackHeights();
        m_divDrag = dh.kind;
        m_divTrack = dh.trackIndex;
        m_divPressY = pos.y();
        m_divPressHeight = trackHeightAt(dh.trackIndex);
        m_divPressFrac = m_dividerFrac;
        setCursor(Qt::SizeVerCursor);
        return;
    }

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
        m_dragLevelDelta = 0;
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
    if (m_drag == Drag::Pan) {
        const int dx = pos.x() - m_panStartX;
        m_scrollSeconds = std::max(0.0, m_panStartScroll - dx / m_pixelsPerSecond);
        m_lastPlayheadX = -1;
        update();
        emit viewChanged();
        return;
    }
    if (m_vdrag != VDrag::None) {
        updateVDrag(pos.y());
        return;
    }
    if (m_divDrag != DividerKind::None) {
        updateDividerDrag(pos.y());
        return;
    }
    if (m_drag == Drag::None) {
        // Over a vertical bar: knobs zoom (pointing hand), body scrolls (arrow).
        if (pos.x() >= contentRight() && pos.y() >= kRulerHeight && m_project
            && m_project->hasActiveSequence()) {
            const bool video = pos.y() < trackLayout().dividerY;
            if (sectionTrackCount(video) > 0) {
                const QRect h = vbarHandle(video);
                const bool onKnob = (pos.y() <= h.top() + 8) || (pos.y() >= h.bottom() - 8);
                setCursor(onKnob ? Qt::PointingHandCursor : Qt::ArrowCursor);
            } else {
                unsetCursor();
            }
            return;
        }
        if (dividerHitTest(pos).kind != DividerKind::None) {
            setCursor(Qt::SizeVerCursor);  // grabbable header divider
            return;
        }
        updateHoverCursor(pos);
        return;
    }

    const int dxPixels = pos.x() - m_pressX;
    if (std::abs(dxPixels) > kDragThresholdPx) {
        m_dragMoved = true;
    }

    Tick delta = snapDelta(ticksFromSeconds(dxPixels / m_pixelsPerSecond));
    switch (m_drag) {
    case Drag::Move: {
        delta = clampMoveDelta(delta);
        // Vertical: how many track-levels (in the dragged clip's kind) the cursor
        // has crossed. The move mirrors this across every linked member's kind.
        if (m_project && m_dragTrack < m_project->sequence().trackCount()) {
            const bool video = m_project->sequence().track(m_dragTrack).kind() == Track::Kind::Video;
            m_dragLevelDelta = levelForY(pos.y(), video) - levelOfTrack(m_dragTrack);
            if (m_dragLevelDelta != 0) {
                m_dragMoved = true;  // a purely vertical move still counts
            }
        }
        break;
    }
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
    if (event->button() == Qt::MiddleButton) {
        if (m_drag == Drag::Pan) {
            m_drag = Drag::None;
            unsetCursor();
        }
        return;
    }
    if (m_vdrag != VDrag::None) {
        m_vdrag = VDrag::None;
        unsetCursor();
        update();
        return;
    }
    if (m_divDrag != DividerKind::None) {
        m_divDrag = DividerKind::None;
        unsetCursor();
        update();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }

    const bool moved = m_previewDelta != 0 || (m_drag == Drag::Move && m_dragLevelDelta != 0);
    if (m_drag != Drag::None && m_drag != Drag::Scrub && m_dragMoved && moved) {
        commitDrag();
    }

    m_drag = Drag::None;
    m_previewDelta = 0;
    m_dragLevelDelta = 0;
    m_dragClip = kInvalidClip;
    updateHoverCursor(event->position().toPoint());
    update();
}

void TimelineWidget::commitDrag()
{
    switch (m_drag) {
    case Drag::Move:
        emit clipMoved(m_dragTrack, m_dragClip, m_dragLevelDelta, m_dragOrigStart + m_previewDelta);
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

void TimelineWidget::updateVDrag(int y)
{
    const bool video = m_vdragVideo;
    const QRect bar = vbarRect(video);
    const int barH = std::max(1, bar.height());
    const int n = sectionTrackCount(video);
    const double viewport = sectionViewport(video);
    if (n <= 0) {
        return;
    }

    if (m_vdrag == VDrag::Scroll) {
        const double contentH = sectionContentHeight(video);  // track height is fixed while scrolling
        if (contentH <= viewport) {
            return;  // everything fits; nothing to scroll
        }
        const double dContent = static_cast<double>(y - m_vPressY) / barH * contentH;
        const double pressWindowTop = video ? (contentH - viewport - m_vPressScroll) : m_vPressScroll;
        const double windowTop = std::clamp(pressWindowTop + dContent, 0.0, contentH - viewport);
        if (video) {
            m_videoScroll = (contentH - viewport) - windowTop;
        } else {
            m_audioScroll = windowTop;
        }
    } else {
        // Zoom: resize the handle from one end, keeping the other end anchored, and
        // translate the new handle length into a per-section track height.
        double lenPx = 0.0;
        double anchorFrac = 0.0;  // bar fraction of the anchored (opposite) end
        if (m_vdrag == VDrag::ZoomBottom) {
            const int newBottom = std::clamp(y, m_vPressHandleTop + kVBarMinHandle, bar.bottom());
            lenPx = newBottom - m_vPressHandleTop;
            anchorFrac = static_cast<double>(m_vPressHandleTop - bar.top()) / barH;
        } else {  // ZoomTop
            const int newTop = std::clamp(y, bar.top(), m_vPressHandleBottom - kVBarMinHandle);
            lenPx = m_vPressHandleBottom - newTop;
            anchorFrac = static_cast<double>(m_vPressHandleBottom - bar.top()) / barH;
        }
        const double frac = std::clamp(lenPx / barH, 1e-3, 1.0);
        const double desiredContentH = viewport / frac;
        // Scale every track in the section proportionally from its press height.
        int pressContentH = 0;
        for (int h : m_vPressHeights) {
            pressContentH += h;
        }
        if (!m_vPressHeights.empty()) {
            pressContentH += (static_cast<int>(m_vPressHeights.size()) - 1) * kTrackGap;
        }
        const double factor = pressContentH > 0 ? desiredContentH / pressContentH : 1.0;
        syncTrackHeights();
        const Sequence& seq = m_project->sequence();
        std::size_t k = 0;
        for (std::size_t i = 0; i < seq.trackCount(); ++i) {
            if ((seq.track(i).kind() == Track::Kind::Video) == video) {
                const int base = k < m_vPressHeights.size() ? m_vPressHeights[k] : kDefaultTrackHeight;
                m_trackH[i] = std::clamp(static_cast<int>(std::lround(base * factor)),
                                         kMinTrackHeight, kMaxTrackHeight);
                ++k;
            }
        }

        const double contentH = sectionContentHeight(video);  // actual, with the clamped heights
        double windowTop = (m_vdrag == VDrag::ZoomBottom) ? anchorFrac * contentH
                                                          : anchorFrac * contentH - viewport;
        windowTop = std::clamp(windowTop, 0.0, std::max(0.0, contentH - viewport));
        if (video) {
            m_videoScroll = std::max(0.0, contentH - viewport) - windowTop;
        } else {
            m_audioScroll = windowTop;
        }
    }
    clampScrolls();
    update();
}

TimelineWidget::DividerHit TimelineWidget::dividerHitTest(const QPoint& pos) const
{
    DividerHit hit;
    if (!m_project || !m_project->hasActiveSequence()) {
        return hit;
    }
    if (pos.y() < kRulerHeight || pos.x() >= contentRight()) {
        return hit;  // ruler or vertical-bar gutter
    }
    // The A/V divider is grabbable anywhere along the timeline.
    const TrackLayout lay = trackLayout();
    if (std::abs(pos.y() - lay.dividerY) <= kDividerGrab) {
        hit.kind = DividerKind::AV;
        return hit;
    }
    // Individual track edges are grabbable only in the header column.
    if (pos.x() >= kHeaderWidth) {
        return hit;
    }
    const Sequence& seq = m_project->sequence();
    for (std::size_t i = 0; i < seq.trackCount(); ++i) {
        const bool video = seq.track(i).kind() == Track::Kind::Video;
        if (video ? (pos.y() >= lay.dividerY) : (pos.y() < lay.dividerY)) {
            continue;
        }
        const int top = trackTop(i);
        const int edgeY = video ? top : top + trackHeightAt(i);  // the track's resize edge
        if (std::abs(pos.y() - edgeY) <= kDividerGrab) {
            hit.kind = DividerKind::Track;
            hit.trackIndex = i;
            return hit;
        }
    }
    return hit;
}

void TimelineWidget::updateDividerDrag(int y)
{
    if (m_divDrag == DividerKind::AV) {
        const int avail = std::max(1, height() - kRulerHeight);
        m_dividerFrac = std::clamp(static_cast<double>(y - kRulerHeight) / avail, 0.0, 1.0);
    } else if (m_divDrag == DividerKind::Track && m_project) {
        // Video tracks resize from the top edge (drag up grows), audio from the bottom.
        const bool video = m_project->sequence().track(m_divTrack).kind() == Track::Kind::Video;
        const int dy = y - m_divPressY;
        const int newH = std::clamp(m_divPressHeight + (video ? -dy : dy), kMinTrackHeight, kMaxTrackHeight);
        syncTrackHeights();
        if (m_divTrack < m_trackH.size()) {
            m_trackH[m_divTrack] = newH;
        }
    }
    clampScrolls();
    update();
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* event)
{
    if (!m_project) {
        return;
    }
    const Hit hit = hitTest(event->pos());
    if (!hit.onClip) {
        // Empty area / track header: track management.
        QMenu menu(this);
        QAction* addVideo = menu.addAction("Add Video Track");
        QAction* addAudio = menu.addAction("Add Audio Track");
        const int trk = trackAtY(event->pos().y());
        QAction* delTrack = nullptr;
        if (trk >= 0) {
            menu.addSeparator();
            delTrack = menu.addAction(
                QString("Delete %1").arg(QString::fromStdString(m_project->sequence().track(trk).name())));
        }
        QAction* chosen = menu.exec(event->globalPos());
        if (chosen == addVideo) {
            emit addTrackRequested(true);
        } else if (chosen == addAudio) {
            emit addTrackRequested(false);
        } else if (delTrack && chosen == delTrack) {
            emit deleteTrackRequested(static_cast<std::size_t>(trk));
        }
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

    QMenu* labelMenu = menu.addMenu("Label");
    QList<QAction*> labelActions;
    QAction* none = labelMenu->addAction("None");
    none->setData(0);
    labelActions << none;
    for (int i = 1; i <= labelCount(); ++i) {
        QPixmap sw(14, 14);
        sw.fill(Qt::transparent);
        QPainter p(&sw);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(labelColor(i));
        p.drawRoundedRect(QRectF(0.5, 0.5, 13, 13), 3, 3);
        p.end();
        QAction* a = labelMenu->addAction(QIcon(sw), labelName(i));
        a->setData(i);
        labelActions << a;
    }

    menu.addSeparator();
    QAction* remove = menu.addAction("Delete Clip");

    QAction* chosen = menu.exec(event->globalPos());
    if (chosen == unlink) {
        emit unlinkRequested(hit.clip);
    } else if (chosen == remove) {
        emit deleteRequested(hit.trackIndex, hit.clip);
    } else if (labelActions.contains(chosen)) {
        emit clipLabelRequested(hit.trackIndex, hit.clip, chosen->data().toInt());
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
            // The cursor's region (above/below the divider) picks the level; both the
            // V and A halves land on that level (mirrored), creating tracks if needed.
            const bool videoRegion = pos.y() < trackLayout().dividerY;
            m_dropLevel = levelForY(pos.y(), videoRegion);
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

    const QPoint pos = event->position().toPoint();
    Tick start = tickForX(pos.x());
    if (m_project) {
        start = m_project->sequence().snapToFrame(start);
    }
    start = std::max<Tick>(0, start);
    const bool videoRegion = m_project && pos.y() < trackLayout().dividerY;
    const int level = levelForY(pos.y(), videoRegion);

    if (event->mimeData()->hasFormat(kMediaMimeType)) {
        const MediaId id = event->mimeData()->data(kMediaMimeType).toULongLong();
        emit mediaDropped(id, start, level);
        event->acceptProposedAction();
    } else if (event->mimeData()->hasUrls()) {
        for (const QUrl& url : event->mimeData()->urls()) {
            if (url.isLocalFile()) {
                emit fileDropped(url.toLocalFile(), start, level);
            }
        }
        event->acceptProposedAction();
    }
    update();
}

void TimelineWidget::wheelEvent(QWheelEvent* event)
{
    // Holding Alt makes Qt/Windows deliver the wheel delta on the x axis, so take
    // whichever axis carries it.
    const QPoint delta = event->angleDelta();
    const double steps = (delta.y() != 0 ? delta.y() : delta.x()) / 120.0;
    if (steps == 0.0) {
        return;
    }

    if (event->modifiers() & Qt::AltModifier) {
        // Alt = horizontal zoom about the cursor.
        const double anchorX = event->position().x();
        const double anchorSeconds = (anchorX - kHeaderWidth) / m_pixelsPerSecond + m_scrollSeconds;
        m_pixelsPerSecond = std::clamp(m_pixelsPerSecond * std::pow(1.2, steps), minPixelsPerSecond(), 20000.0);
        m_scrollSeconds = std::max(0.0, anchorSeconds - (anchorX - kHeaderWidth) / m_pixelsPerSecond);
        m_lastPlayheadX = -1;
        update();
        emit viewChanged();
        event->accept();
        return;
    }
    if (event->modifiers() & Qt::ControlModifier) {
        // Ctrl = scroll the section the cursor is over (does not change track height).
        const bool video = event->position().y() < trackLayout().dividerY;
        double& scroll = video ? m_videoScroll : m_audioScroll;
        scroll = std::clamp(scroll - steps * 40.0, 0.0, maxScroll(video));
        update();
        event->accept();
        return;
    }

    m_scrollSeconds = std::max(0.0, m_scrollSeconds - steps * 40.0 / m_pixelsPerSecond);
    m_lastPlayheadX = -1;
    update();
    emit viewChanged();
    event->accept();
}

void TimelineWidget::resizeEvent(QResizeEvent*)
{
    emit viewChanged();  // visible span depends on width
}

void TimelineWidget::drawRuler(QPainter& painter)
{
    painter.fillRect(QRect(0, 0, width(), kRulerHeight), kRulerBackground);
    if (m_pixelsPerSecond <= 0.0) {
        return;
    }

    const double interval = niceInterval(m_pixelsPerSecond);
    const double first = std::floor(m_scrollSeconds / interval) * interval;
    const double last = m_scrollSeconds + (contentRight() - kHeaderWidth) / m_pixelsPerSecond;

    QFont font = painter.font();
    font.setPointSizeF(7.5);
    painter.setFont(font);

    for (double t = first; t <= last; t += interval) {
        const int x = xForTick(ticksFromSeconds(t));
        if (x < kHeaderWidth) {
            continue;
        }
        if (x >= contentRight()) {
            break;
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

    // A/V divider (drawn last, over the lanes) between the video section (above) and
    // the audio section (below): a subtle line across the whole timeline.
    const TrackLayout lay = trackLayout();

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
        const int th = trackHeightAt(i);
        const int y = trackTop(i);

        // Clip each track to its section so a scrolled track can't paint into the
        // ruler, the other section, or the vertical-bar gutter.
        const QRect sectionRect = isVideo
            ? QRect(0, kRulerHeight, contentRight(), lay.dividerY - kRulerHeight)
            : QRect(0, lay.dividerY, contentRight(), height() - lay.dividerY);
        painter.save();
        painter.setClipRect(sectionRect);

        painter.fillRect(QRect(0, y, kHeaderWidth, th), kHeaderBackground);
        painter.fillRect(QRect(kHeaderWidth, y, contentRight() - kHeaderWidth, th),
                         isVideo ? kLaneVideo : kLaneAudio);

        painter.setPen(kText);
        painter.drawText(QRect(0, y, kHeaderWidth - 8, th),
                         Qt::AlignRight | Qt::AlignVCenter, QString::fromStdString(track.name()));

        for (const Clip& clip : track.clips()) {
            Tick start = clip.timelineStart;
            Tick duration = clip.duration;
            Tick sourceIn = clip.sourceIn;
            // Trimming resizes the clip live; moving leaves the clip in place (with
            // its selection outline) and shows a destination silhouette after the loop.
            if (dragging && isAffected(clip.id)) {
                if (m_drag == Drag::TrimHead) {
                    start += m_previewDelta;
                    duration -= m_previewDelta;
                    sourceIn += m_previewDelta;  // head trim advances into the source
                } else if (m_drag == Drag::TrimTail) {
                    duration += m_previewDelta;
                }
            }

            const int x0 = xForTick(start);
            const int x1 = xForTick(start + duration);
            if (x1 < kHeaderWidth || x0 > contentRight()) {
                continue;
            }

            // While trimming, outline the pre-trim extent so you can see what's
            // being cut away (or how far it was before extending).
            const bool trimming = (m_drag == Drag::TrimHead || m_drag == Drag::TrimTail);
            if (trimming && isAffected(clip.id)) {
                const int gx0 = std::max(xForTick(clip.timelineStart), kHeaderWidth);
                const int gx1 = xForTick(clip.range().end());
                QRect ghost(gx0, y + 3, gx1 - gx0, th - 6);
                QPen ghostPen(kGhost, 1, Qt::DashLine);
                painter.setPen(ghostPen);
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(ghost, 3, 3);
            }

            QRect box(std::max(x0, kHeaderWidth), y + 3, x1 - std::max(x0, kHeaderWidth), th - 6);
            if (box.width() < 1) {
                box.setWidth(1);
            }

            // A clip is a label-colored name strip on top of a darker preview body.
            // Default color follows the source, not the track: a video's audio half is
            // blue like its video; green is reserved for audio-only sources.
            const MediaSource* clipMedia = m_project->media(clip.source);
            const bool audioOnly = clipMedia && !clipMedia->hasVideo;
            const QColor stripColor = clip.label > 0 ? labelColor(clip.label)
                                                     : (audioOnly ? kClipAudio : kClipVideo);
            const QColor bodyColor = stripColor.darker(190);
            const int stripH = std::min(kClipLabelStrip, box.height());
            const QRect content(box.left(), box.top() + stripH, box.width(), box.height() - stripH);

            painter.save();
            QPainterPath clipPath;
            clipPath.addRoundedRect(box, 3, 3);
            painter.setClipPath(clipPath, Qt::IntersectClip);
            painter.setPen(Qt::NoPen);
            painter.fillRect(box, bodyColor);
            painter.fillRect(QRect(box.left(), box.top(), box.width(), stripH), stripColor);

            // Preview fills the body under the strip; omit it when the clip is too small.
            const bool roomForPreview = box.width() >= 24 && content.height() >= 12;
            if (m_preview && roomForPreview) {
                const int x0f = xForTick(start);
                const int fullW = xForTick(start + duration) - x0f;
                const double srcStartSec = secondsFromTicks(sourceIn);
                const double srcSpanSec = secondsFromTicks(duration);
                if (isVideo) {
                    drawThumbnails(painter, clip, content, x0f, fullW, srcStartSec, srcSpanSec);
                } else {
                    drawWaveform(painter, clip, content, x0f, fullW, srcStartSec, srcSpanSec);
                }
            }
            painter.restore();

            // Filename in the strip, legible against its color.
            if (box.width() > 28) {
                QString name = "clip";
                if (clipMedia) {
                    name = QFileInfo(QString::fromStdString(clipMedia->path)).fileName();
                }
                const QFontMetrics fm(painter.font());
                const QString elided = fm.elidedText(name, Qt::ElideRight, box.width() - 10);
                painter.setPen(stripColor.lightnessF() > 0.6 ? QColor(20, 20, 22) : QColor(238, 238, 240));
                painter.drawText(box.left() + 5, box.top() + (stripH + fm.ascent() - fm.descent()) / 2, elided);
            }

            if (isHighlighted(clip)) {
                painter.setPen(QPen(kSelected, 2));
            } else {
                painter.setPen(stripColor.lighter(140));
            }
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(box, 3, 3);
        }

        painter.setPen(kGridLine);
        painter.drawLine(0, y + th, contentRight(), y + th);
        // Thin, grabbable divider in the header at the track's resize edge
        // (video resizes from its top edge, audio from its bottom edge).
        const int edgeY = isVideo ? y : y + th;
        painter.fillRect(QRect(0, edgeY, kHeaderWidth, 1), kHeaderDivider);
        painter.restore();
    }

    // Destination silhouette(s) for a move: only the dragged clip changes track;
    // linked partners keep their track and shift in time together.
    if (m_drag == Drag::Move && (m_previewDelta != 0 || m_dragLevelDelta != 0)) {
        painter.setPen(QPen(QColor(225, 226, 232, 170), 1, Qt::DashLine));
        painter.setBrush(QColor(255, 255, 255, 28));
        for (std::size_t i = 0; i < sequence.trackCount(); ++i) {
            const bool isVideo = sequence.track(i).kind() == Track::Kind::Video;
            for (const Clip& clip : sequence.track(i).clips()) {
                if (!isAffected(clip.id)) {
                    continue;
                }
                const bool dragged = clip.id == m_dragClip;
                const Tick destStart = clip.timelineStart + m_previewDelta;
                const int destLevel = levelOfTrack(i) + (dragged ? m_dragLevelDelta : 0);
                if (destStart == clip.timelineStart && destLevel == levelOfTrack(i)) {
                    continue;  // no change for this member
                }
                const int destY = levelToY(destLevel, isVideo);
                const int dx0 = std::max(xForTick(destStart), kHeaderWidth);
                const int dx1 = std::min(xForTick(destStart + clip.duration), contentRight());
                if (dx1 <= kHeaderWidth) {
                    continue;
                }
                painter.drawRoundedRect(QRect(dx0, destY + 3, dx1 - dx0, heightOfLevel(isVideo, destLevel) - 6), 3, 3);
            }
        }
    }

    // Drawn on top of the lanes so it stays visible where the two sections meet.
    painter.fillRect(QRect(0, lay.dividerY - 1, contentRight(), 2), kAVDivider);
}

void TimelineWidget::drawThumbnails(QPainter& painter, const Clip& clip, const QRect& box, int x0,
                                    int fullWidth, double srcStart, double srcSpan)
{
    const PreviewCache::Thumbnails* thumbs = m_preview->thumbnails(clip.source);
    if (!thumbs || thumbs->images.empty() || fullWidth <= 0) {
        return;
    }
    const QImage& first = thumbs->images.front();
    if (first.width() <= 0 || first.height() <= 0) {
        return;
    }
    // Scale each thumbnail to the body height while keeping its aspect ratio.
    const int tileW = std::max(1, first.width() * box.height() / first.height());

    painter.save();
    painter.setClipRect(box, Qt::IntersectClip);  // stay within the track's section clip
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
    if (!wave || wave->left.empty() || fullWidth <= 0) {
        return;
    }

    const MediaSource* wm = m_project->media(clip.source);
    const bool audioOnly = wm && !wm->hasVideo;
    const QColor col = clip.label > 0 ? labelColor(clip.label).lighter(160)
                                      : (audioOnly ? kWaveform : kClipVideo.lighter(160));
    auto peakAt = [&](int x, const std::vector<float>& ch) -> float {
        const double fraction = std::clamp((x - x0 + 0.5) / fullWidth, 0.0, 1.0);
        const double srcSec = srcStart + fraction * srcSpan;
        const size_t bucket = static_cast<size_t>(srcSec * wave->bucketsPerSecond);
        return bucket < ch.size() ? ch[bucket] : 0.0f;
    };

    painter.save();
    painter.setClipRect(box, Qt::IntersectClip);  // stay within the track's section clip
    painter.setPen(col);

    // Tall tracks split into L/R lanes; short ones show a single merged envelope.
    if (box.height() >= 40) {
        const int halfBox = box.height() / 2;
        const int cyL = box.top() + halfBox / 2;
        const int cyR = box.top() + halfBox + (box.height() - halfBox) / 2;
        const double hL = halfBox / 2.0 - 1.0;
        const double hR = (box.height() - halfBox) / 2.0 - 1.0;
        for (int x = box.left(); x < box.right(); ++x) {
            const int l = static_cast<int>(peakAt(x, wave->left) * hL);
            const int r = static_cast<int>(peakAt(x, wave->right) * hR);
            painter.drawLine(x, cyL - l, x, cyL + l);
            painter.drawLine(x, cyR - r, x, cyR + r);
        }
        painter.setPen(QColor(col.red(), col.green(), col.blue(), 70));
        painter.drawLine(box.left(), box.top() + halfBox, box.right(), box.top() + halfBox);
        if (box.width() > 30) {
            painter.setPen(QColor(210, 212, 218));
            painter.drawText(box.left() + 3, cyL + 4, "L");
            painter.drawText(box.left() + 3, cyR + 4, "R");
        }
    } else {
        const int cy = box.center().y();
        const double h = box.height() / 2.0 - 2.0;
        for (int x = box.left(); x < box.right(); ++x) {
            const float peak = std::max(peakAt(x, wave->left), peakAt(x, wave->right));
            const int hh = static_cast<int>(peak * h);
            painter.drawLine(x, cy - hh, x, cy + hh);
        }
    }
    painter.restore();
}

void TimelineWidget::drawDropGhost(QPainter& painter)
{
    if (!m_dropActive) {
        return;
    }
    const int x0 = std::max(xForTick(m_dropStart), kHeaderWidth);
    const int x1 = std::min(xForTick(m_dropStart + m_dropDuration), contentRight());
    if (x1 <= kHeaderWidth) {
        return;
    }

    const TrackLayout lay = trackLayout();
    QPen pen(kSelected, 1, Qt::DashLine);
    painter.setPen(pen);
    painter.setBrush(QColor(kSelected.red(), kSelected.green(), kSelected.blue(), 40));

    auto drawLane = [&](bool video) {
        const QRect sectionRect = video
            ? QRect(0, kRulerHeight, contentRight(), lay.dividerY - kRulerHeight)
            : QRect(0, lay.dividerY, contentRight(), height() - lay.dividerY);
        painter.save();
        painter.setClipRect(sectionRect);
        const int y = levelToY(m_dropLevel, video);  // handles levels beyond existing tracks
        painter.drawRoundedRect(QRect(x0, y + 3, x1 - x0, heightOfLevel(video, m_dropLevel) - 6), 3, 3);
        painter.restore();
    };
    if (m_dropVideo) {
        drawLane(true);
    }
    if (m_dropAudio) {
        drawLane(false);
    }
}

void TimelineWidget::drawPlayhead(QPainter& painter)
{
    const int x = xForTick(m_playhead);
    if (x < kHeaderWidth || x >= contentRight()) {
        return;
    }
    painter.setPen(QPen(kPlayhead, 1));
    painter.drawLine(x, 0, x, height());

    const QPolygon handle({ QPoint(x - 4, 0), QPoint(x + 4, 0), QPoint(x, 7) });
    painter.setPen(Qt::NoPen);
    painter.setBrush(kPlayhead);
    painter.drawPolygon(handle);
}

QRect TimelineWidget::vbarRect(bool video) const
{
    const TrackLayout lay = trackLayout();
    return video ? QRect(contentRight(), kRulerHeight, kVBarWidth, lay.dividerY - kRulerHeight)
                 : QRect(contentRight(), lay.dividerY, kVBarWidth, height() - lay.dividerY);
}

QRect TimelineWidget::vbarHandle(bool video) const
{
    const QRect bar = vbarRect(video);
    const int inset = 2;
    const double contentH = sectionContentHeight(video);
    const double viewport = sectionViewport(video);
    if (contentH <= viewport || contentH <= 0.0 || bar.height() <= 0) {
        return QRect(bar.left() + inset, bar.top() + inset, bar.width() - 2 * inset, bar.height() - 2 * inset);
    }
    const double windowTop = video ? (contentH - viewport - m_videoScroll) : m_audioScroll;
    const double f0 = std::clamp(windowTop / contentH, 0.0, 1.0);
    const double f1 = std::clamp((windowTop + viewport) / contentH, 0.0, 1.0);
    int hy0 = bar.top() + static_cast<int>(std::lround(f0 * bar.height()));
    int hy1 = bar.top() + static_cast<int>(std::lround(f1 * bar.height()));
    if (hy1 - hy0 < kVBarMinHandle) {
        hy1 = hy0 + kVBarMinHandle;
    }
    return QRect(bar.left() + inset, hy0, bar.width() - 2 * inset, hy1 - hy0);
}

void TimelineWidget::drawVBars(QPainter& painter)
{
    if (!m_project) {
        return;
    }
    const QColor groove(26, 27, 30), handleC(78, 80, 86), handleA(104, 106, 114);
    const QColor knob(176, 178, 186), knobA(214, 216, 224);

    painter.setRenderHint(QPainter::Antialiasing, true);
    auto drawBar = [&](bool video) {
        if (sectionTrackCount(video) <= 0) {
            return;
        }
        const QRect bar = vbarRect(video);
        if (bar.height() < kVBarMinHandle) {
            return;
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(groove);
        painter.drawRoundedRect(bar.adjusted(2, 2, -2, -2), 4, 4);

        const bool active = (m_vdrag != VDrag::None && m_vdragVideo == video);
        const QRect h = vbarHandle(video);
        painter.setBrush(active ? handleA : handleC);
        painter.drawRoundedRect(h, 4, 4);

        const double cx = h.center().x() + 0.5;
        const double r = h.width() / 2.0;
        painter.setBrush(active ? knobA : knob);
        painter.drawEllipse(QPointF(cx, h.top() + r), r, r);
        painter.drawEllipse(QPointF(cx, h.bottom() - r), r, r);
    };
    drawBar(true);
    drawBar(false);
    painter.setRenderHint(QPainter::Antialiasing, false);
}

void TimelineWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), kBackground);

    if (!m_project || !m_project->hasActiveSequence()) {
        painter.setPen(QColor(110, 112, 118));
        QFont f = painter.font();
        f.setPixelSize(13);
        painter.setFont(f);
        painter.drawText(rect(), Qt::AlignCenter,
                         "No active sequence\n\nDrag a clip here, or create one with File ▸ New Sequence");
        return;
    }

    syncTrackHeights();  // keep per-track heights sized to the current track list
    clampScrolls();  // track counts / size may have changed since the last paint
    drawTracks(painter);
    drawDropGhost(painter);
    drawRuler(painter);
    drawPlayhead(painter);
    drawVBars(painter);
}

}  // namespace hopline

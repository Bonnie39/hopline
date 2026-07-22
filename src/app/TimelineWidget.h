#pragma once

#include <QPoint>
#include <QRect>
#include <QString>
#include <QWidget>

#include <vector>

#include "model/Clip.h"
#include "model/Time.h"

class QMimeData;

namespace hopline {

class Project;
class PreviewCache;
struct Clip;

// Renders the sequence, the playhead, and the selection, and turns mouse
// gestures into edit intents (signals). It never mutates the model itself —
// MainWindow owns that and issues the commands.
//
// Interaction map:
//   ruler (top strip)   -> drag the playhead
//   clip body           -> select; drag to move
//   clip edge           -> drag to trim that edge
//   empty track area    -> clear selection
//   right-click a clip   -> context menu (unlink / label / delete)
class TimelineWidget : public QWidget {
    Q_OBJECT

public:
    explicit TimelineWidget(QWidget* parent = nullptr);

    void setProject(const Project* project);
    void setPreviewCache(const PreviewCache* cache) { m_preview = cache; }
    void setPlayhead(Tick time);
    Tick playhead() const { return m_playhead; }

    ClipId selected() const { return m_selected; }
    void clearSelection();

    void zoomToFit();

    // Horizontal view state, in seconds, for the timeline scroll/zoom bar.
    double viewStart() const;   // left edge (m_scrollSeconds)
    double viewSpan() const;    // visible span
    double viewTotal() const;   // total scrollable extent
    void setView(double start, double span);

signals:
    void playheadDragged(Tick time);
    void viewChanged();  // scroll or zoom changed (for the scroll bar to track)
    void selectionChanged(ClipId clip);
    void clipMoved(std::size_t fromTrack, ClipId clip, int levelDelta, Tick newStart);
    void clipTrimmed(std::size_t trackIndex, ClipId clip, bool trimHead, Tick delta);
    void unlinkRequested(ClipId clip);
    void clipLabelRequested(std::size_t trackIndex, ClipId clip, int label);
    void deleteRequested(std::size_t trackIndex, ClipId clip);
    void addTrackRequested(bool video);
    void deleteTrackRequested(std::size_t trackIndex);
    void mediaDropped(MediaId media, Tick start, int level);
    void fileDropped(const QString& path, Tick start, int level);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    enum class Drag { None, Scrub, Move, TrimHead, TrimTail, Pan };
    // A vertical zoom bar drag: scroll the section, or zoom (resize track height)
    // from either end.
    enum class VDrag { None, Scroll, ZoomTop, ZoomBottom };

    struct Hit {
        bool onRuler = false;
        bool onClip = false;
        std::size_t trackIndex = 0;
        ClipId clip = kInvalidClip;
        int edge = -1;  // -1 body, 0 head, 1 tail
    };

    // Tracks stack outward from a center divider: video above (V1 at the divider,
    // higher tracks up), audio below (A1 at the divider, higher tracks down).
    struct TrackLayout {
        int dividerY = 0;
        int videoBottom = 0;  // bottom edge of the video block (V1's bottom)
        int audioTop = 0;     // top edge of the audio block (A1's top)
    };
    TrackLayout trackLayout() const;

    int xForTick(Tick time) const;
    Tick tickForX(int x) const;
    double minPixelsPerSecond() const;  // zoom-out floor: fit the whole timeline
    int contentRight() const;  // right edge of the track content (before the vbar gutter)

    // Track heights are per-track (view state), so individual tracks can be resized.
    void syncTrackHeights();                 // keep m_trackH sized to the track count
    int trackHeightAt(std::size_t index) const;
    int trackAtLevel(bool video, int level) const;  // track index of the level-th of that kind, or -1
    int heightOfLevel(bool video, int level) const;  // that level's height, default if it has no track
    int trackTop(std::size_t index) const;

    // Per-section vertical scroll/zoom. Video grows up from the divider, audio down;
    // each section scrolls independently when its tracks overflow the viewport.
    int sectionTrackCount(bool video) const;
    int sectionContentHeight(bool video) const;  // px needed for all tracks of that kind
    int sectionViewport(bool video) const;        // px available in that section
    double maxScroll(bool video) const;
    void clampScrolls();
    QRect vbarRect(bool video) const;
    QRect vbarHandle(bool video) const;
    int trackAtY(int y) const;  // sequence track index under y, or -1
    // Level = a track's position among same-kind tracks (0 = V1/A1). These map a
    // level to/from a y even for levels beyond the existing tracks (for cross-track
    // moves that create new tracks).
    int levelOfTrack(std::size_t index) const;
    int levelToY(int level, bool video) const;
    int levelForY(int y, bool video) const;
    Hit hitTest(const QPoint& pos) const;

    // Grabbable dividers on the header column: the A/V split, or a single track's edge.
    enum class DividerKind { None, AV, Track };
    struct DividerHit {
        DividerKind kind = DividerKind::None;
        std::size_t trackIndex = 0;
    };
    DividerHit dividerHitTest(const QPoint& pos) const;
    void updateDividerDrag(int y);

    void scrubTo(int x);
    Tick snapDelta(Tick raw) const;
    std::vector<ClipId> affectedByDrag() const;
    Tick clampMoveDelta(Tick delta) const;
    Tick clampTrimDelta(Tick delta, bool trimHead) const;
    void updateHoverCursor(const QPoint& pos);
    void updateVDrag(int y);  // apply a vertical-bar scroll/zoom drag
    void commitDrag();

    void drawRuler(QPainter& painter);
    void drawTracks(QPainter& painter);
    void drawDropGhost(QPainter& painter);
    void drawPlayhead(QPainter& painter);
    void drawVBars(QPainter& painter);
    void updateDropGhost(const QPoint& pos, const QMimeData* mime);
    int firstTrackOfKind(bool video) const;
    // Content is mapped by fraction along the clip so it stays aligned under a
    // move/trim preview. srcStart/srcSpan are the source seconds the box covers.
    void drawThumbnails(QPainter& painter, const Clip& clip, const QRect& box, int x0, int fullWidth,
                        double srcStart, double srcSpan);
    void drawWaveform(QPainter& painter, const Clip& clip, const QRect& box, int x0, int fullWidth,
                      double srcStart, double srcSpan);

    const Project* m_project = nullptr;
    const PreviewCache* m_preview = nullptr;
    Tick m_playhead = 0;
    double m_pixelsPerSecond = 100.0;
    double m_scrollSeconds = 0.0;

    ClipId m_selected = kInvalidClip;

    Drag m_drag = Drag::None;
    std::size_t m_dragTrack = 0;
    ClipId m_dragClip = kInvalidClip;
    Tick m_dragOrigStart = 0;
    Tick m_dragOrigDuration = 0;
    Tick m_previewDelta = 0;
    int m_dragLevelDelta = 0;  // vertical track-levels the move drag has crossed
    int m_pressX = 0;
    bool m_dragMoved = false;
    int m_panStartX = 0;          // middle-mouse pan anchor
    double m_panStartScroll = 0.0;

    // Vertical layout state. Per-track heights (indexed by track); the A/V divider
    // position as a fraction of the track area.
    std::vector<int> m_trackH;
    double m_dividerFrac = 0.5;
    double m_videoScroll = 0.0;  // px scrolled up into the video stack
    double m_audioScroll = 0.0;  // px scrolled down into the audio stack

    VDrag m_vdrag = VDrag::None;
    bool m_vdragVideo = false;
    int m_vPressY = 0;
    double m_vPressScroll = 0.0;
    std::vector<int> m_vPressHeights;  // section track heights at press, for proportional zoom
    int m_vPressHandleTop = 0;         // handle extent at press, for zoom anchoring
    int m_vPressHandleBottom = 0;

    // A/V-divider / track-edge drag on the header column.
    DividerKind m_divDrag = DividerKind::None;
    std::size_t m_divTrack = 0;
    int m_divPressY = 0;
    int m_divPressHeight = 0;
    double m_divPressFrac = 0.5;

    int m_lastPlayheadX = -1;  // repaint only when the playhead crosses a pixel

    // Where a dragged media item would land, shown as a ghost during drag-over.
    bool m_dropActive = false;
    Tick m_dropStart = 0;
    Tick m_dropDuration = 0;
    bool m_dropVideo = false;
    bool m_dropAudio = false;
    int m_dropLevel = 0;  // target track level (same for the V and A halves — mirrored)
};

}  // namespace hopline

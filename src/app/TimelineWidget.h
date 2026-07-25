#pragma once

#include <QCursor>
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
    // Active editing tool. Select is the default; Blade splits clips at the cursor.
    // Text is a palette placeholder (no behavior yet).
    enum class Tool { Select, Blade, Text };

    explicit TimelineWidget(QWidget* parent = nullptr);

    void setTool(Tool tool);

    void setProject(const Project* project);
    void setPreviewCache(const PreviewCache* cache) { m_preview = cache; }
    void setPlayhead(Tick time);
    Tick playhead() const { return m_playhead; }

    ClipId selected() const { return m_selected; }  // primary (for the effect panel)
    const std::vector<ClipId>& selection() const { return m_selection; }  // full set (rubber-band)
    void clearSelection();

    void zoomToFit();

    // Horizontal view state, in seconds, for the timeline scroll/zoom bar.
    double viewStart() const;   // left edge (m_scrollSeconds)
    double viewSpan() const;    // visible span
    double viewTotal() const;   // total scrollable extent
    void setView(double start, double span);

signals:
    void playheadDragged(Tick time);
    void scrubStarted();  // ruler press: begin a playhead scrub gesture
    void scrubEnded();    // ruler release: end it
    void viewChanged();  // scroll or zoom changed (for the scroll bar to track)
    void selectionChanged(ClipId clip);
    void clipMoved(std::size_t fromTrack, ClipId clip, int levelDelta, Tick newStart, bool duplicate);
    // A multi-clip move (or an Alt-drag duplicate): every clip shifts by the same time delta.
    void clipsMoved(const std::vector<ClipId>& clips, Tick delta, bool duplicate);
    void clipTrimmed(std::size_t trackIndex, ClipId clip, bool trimHead, Tick delta);
    void splitRequested(std::size_t trackIndex, ClipId clip, Tick at);
    void clipRolled(std::size_t trackIndex, ClipId left, ClipId right, Tick delta);  // roll edit
    void unlinkRequested(ClipId clip);
    void linkRequested(const std::vector<ClipId>& clips);  // link a 2+ overlapping selection
    void clipLabelRequested(std::size_t trackIndex, ClipId clip, int label);
    void deleteRequested(std::size_t trackIndex, ClipId clip);
    void deleteSelectionRequested();  // delete the whole multi-selection
    void addTrackRequested(bool video);
    void deleteTrackRequested(std::size_t trackIndex);
    void mediaDropped(MediaId media, Tick start, int level);
    void fileDropped(const QString& path, Tick start, int level);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    enum class Drag { None, Scrub, Move, TrimHead, TrimTail, Pan, Band, Roll };

    // A shared boundary between two butt-joined clips on one track (for a roll edit).
    struct RollHit {
        bool valid = false;
        std::size_t track = 0;
        ClipId left = kInvalidClip;
        ClipId right = kInvalidClip;
        Tick boundary = 0;
    };
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
    int sectionContentHeight(bool video) const;      // px needed for all tracks of that kind
    int sectionMinContentHeight(bool video) const;   // px with every track at kMinTrackHeight
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
    // Snap the drag's moving edges to nearby clip ends / 0 / playhead, within a few
    // frames. head/tail pick which edges move (both for a move). Records the snap for
    // the indicator, so it's not const.
    Tick snapEdges(Tick delta, bool head, bool tail);
    Tick snapDrop(Tick start, Tick duration);   // snap a to-be-dropped clip's edges to clip ends
    void updateBladeHover(const QPoint& pos);  // blade tool: set the cut-preview position
    std::vector<ClipId> clipsInRect(const QRect& r) const;  // clips whose boxes intersect a rubber-band
    std::vector<ClipId> affectedByDrag() const;
    Tick clampMoveDelta(Tick delta) const;
    Tick clampTrimDelta(Tick delta, bool trimHead) const;
    RollHit rollHitTest(const QPoint& pos) const;  // shared boundary under the cursor
    void buildRollPairs();                         // base pair + linked-partner boundaries
    Tick clampRollDelta(Tick delta) const;         // limited by whichever side is shorter
    void updateHoverCursor(const QPoint& pos);
    void updateVDrag(int y);  // apply a vertical-bar scroll/zoom drag
    void commitDrag();

    void drawRuler(QPainter& painter);
    void drawTracks(QPainter& painter);
    void drawBladeHover(QPainter& painter);
    void drawSnapIndicator(QPainter& painter);
    void drawBand(QPainter& painter);
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

    ClipId m_selected = kInvalidClip;      // primary selection (effect panel, move/trim)
    std::vector<ClipId> m_selection;       // full selected set (single click or rubber-band)
    QPoint m_bandOrigin;                   // rubber-band anchor
    QRect m_bandRect;                      // rubber-band rectangle while Drag::Band

    // Active tool + blade-tool hover state (the clip under the cursor and the cut point).
    Tool m_tool = Tool::Select;
    QCursor m_bladeCursor;
    ClipId m_bladeClip = kInvalidClip;
    std::size_t m_bladeTrack = 0;
    Tick m_bladeAt = 0;
    bool m_bladeOnPlayhead = false;  // cut snapped to the playhead → extra indicators

    int m_hoverX = -1;  // mouse x for the ruler hover dash (-1 = none / scrubbing)

    // Set by snapEdges while a move/trim is snapping, for the snap indicator line.
    bool m_snapActive = false;
    Tick m_snapTick = 0;

    Drag m_drag = Drag::None;
    std::size_t m_dragTrack = 0;
    ClipId m_dragClip = kInvalidClip;
    Tick m_dragOrigStart = 0;
    Tick m_dragOrigDuration = 0;
    Tick m_previewDelta = 0;
    int m_dragLevelDelta = 0;  // vertical track-levels the move drag has crossed
    bool m_multiMove = false;   // the move affects the whole selection (time-only)
    bool m_dragDuplicate = false;  // Alt-drag: leave the originals and place copies

    // Roll-edit state: the dragged boundary, plus any linked-partner boundaries that roll with it.
    std::size_t m_rollTrack = 0;
    ClipId m_rollLeft = kInvalidClip;
    ClipId m_rollRight = kInvalidClip;
    Tick m_rollBoundary = 0;
    std::vector<RollHit> m_rollPairs;
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

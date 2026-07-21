#pragma once

#include <QPoint>
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

signals:
    void playheadDragged(Tick time);
    void selectionChanged(ClipId clip);
    void clipMoved(std::size_t trackIndex, ClipId clip, Tick newStart);
    void clipTrimmed(std::size_t trackIndex, ClipId clip, bool trimHead, Tick delta);
    void unlinkRequested(ClipId clip);
    void clipLabelRequested(std::size_t trackIndex, ClipId clip, int label);
    void deleteRequested(std::size_t trackIndex, ClipId clip);
    void mediaDropped(MediaId media, Tick start);
    void fileDropped(const QString& path, Tick start);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    enum class Drag { None, Scrub, Move, TrimHead, TrimTail };

    struct Hit {
        bool onRuler = false;
        bool onClip = false;
        std::size_t trackIndex = 0;
        ClipId clip = kInvalidClip;
        int edge = -1;  // -1 body, 0 head, 1 tail
    };

    int xForTick(Tick time) const;
    Tick tickForX(int x) const;
    int trackTop(std::size_t index) const;
    Hit hitTest(const QPoint& pos) const;

    void scrubTo(int x);
    Tick snapDelta(Tick raw) const;
    std::vector<ClipId> affectedByDrag() const;
    Tick clampMoveDelta(Tick delta) const;
    Tick clampTrimDelta(Tick delta, bool trimHead) const;
    void updateHoverCursor(const QPoint& pos);
    void commitDrag();

    void drawRuler(QPainter& painter);
    void drawTracks(QPainter& painter);
    void drawDropGhost(QPainter& painter);
    void drawPlayhead(QPainter& painter);
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
    int m_pressX = 0;
    bool m_dragMoved = false;

    int m_lastPlayheadX = -1;  // repaint only when the playhead crosses a pixel

    // Where a dragged media item would land, shown as a ghost during drag-over.
    bool m_dropActive = false;
    Tick m_dropStart = 0;
    Tick m_dropDuration = 0;
    bool m_dropVideo = false;
    bool m_dropAudio = false;
};

}  // namespace hopline

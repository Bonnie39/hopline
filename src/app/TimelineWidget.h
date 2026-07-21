#pragma once

#include <QWidget>

#include "model/Time.h"

namespace hopline {

class Project;

// Renders the sequence and the playhead. Read-only for now: it shows the model
// and scrubs, but doesn't edit it.
class TimelineWidget : public QWidget {
    Q_OBJECT

public:
    explicit TimelineWidget(QWidget* parent = nullptr);

    void setProject(const Project* project);
    void setPlayhead(Tick time);
    Tick playhead() const { return m_playhead; }

    void zoomToFit();

signals:
    void playheadDragged(Tick time);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    int xForTick(Tick time) const;
    Tick tickForX(int x) const;
    void scrubTo(int x);
    void drawRuler(QPainter& painter);
    void drawTracks(QPainter& painter);
    void drawPlayhead(QPainter& painter);

    const Project* m_project = nullptr;
    Tick m_playhead = 0;
    double m_pixelsPerSecond = 100.0;
    double m_scrollSeconds = 0.0;
    bool m_scrubbing = false;
    int m_lastPlayheadX = -1;  // repaint only when the playhead actually moves a pixel
};

}  // namespace hopline

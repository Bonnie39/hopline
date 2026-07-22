#pragma once

#include <QWidget>

namespace hopline {

// A movable strip of editing-tool buttons (selection / razor / hand / zoom).
// Exclusive selection; emits toolSelected. The tools aren't wired to timeline
// behavior yet — this is the palette shell.
class ToolboxWidget : public QWidget {
    Q_OBJECT

public:
    enum Tool { Select, Razor, Hand, Zoom };

    explicit ToolboxWidget(QWidget* parent = nullptr);

signals:
    void toolSelected(int tool);
};

}  // namespace hopline

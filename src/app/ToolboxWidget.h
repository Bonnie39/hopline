#pragma once

#include <QWidget>

class QButtonGroup;

namespace hopline {

// A movable strip of editing-tool buttons (selection / blade / text). Exclusive
// selection; emits toolSelected on click. setCurrentTool checks a button without
// emitting (for keyboard shortcuts driving the selection).
class ToolboxWidget : public QWidget {
    Q_OBJECT

public:
    enum Tool { Select, Blade, Text };

    explicit ToolboxWidget(QWidget* parent = nullptr);

    void setCurrentTool(int tool);

signals:
    void toolSelected(int tool);

private:
    QButtonGroup* m_group = nullptr;
};

}  // namespace hopline

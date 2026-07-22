#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

#include "app/MainWindow.h"

namespace {

// A single dark palette over the Fusion style so every stock widget — menus,
// docks, dialogs, the bin tree, scrollbars — matches the custom-painted views.
void applyDarkTheme(QApplication& app)
{
    app.setStyle(QStyleFactory::create("Fusion"));

    // Window doubles as panel-container background (and read-only text-edit bg), so
    // it must match the panel shades or those panels look "extra dark". The darker,
    // more-defined look comes from the dock separators, not from darkening this.
    const QColor window(26, 27, 29);
    const QColor base(24, 25, 27);
    const QColor altBase(30, 31, 34);
    const QColor panel(45, 46, 49);
    const QColor text(221, 221, 221);
    const QColor muted(130, 130, 130);
    const QColor accent(74, 127, 200);
    const QColor select(58, 61, 67);  // neutral selection (text/unstyled), not blue

    QPalette p;
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, altBase);
    p.setColor(QPalette::ToolTipBase, panel);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, panel);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Link, accent);
    p.setColor(QPalette::Highlight, select);
    p.setColor(QPalette::HighlightedText, QColor(240, 240, 240));
    p.setColor(QPalette::PlaceholderText, muted);
    p.setColor(QPalette::Disabled, QPalette::Text, muted);
    p.setColor(QPalette::Disabled, QPalette::WindowText, muted);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, muted);
    app.setPalette(p);

    // One consistent language: white-tint rounded fills for every hover/press/
    // selection (no separate edge color), a single 6px radius, neutral (non-blue)
    // highlights, and thin modern scrollbars.
    app.setStyleSheet(
        "QToolTip { color: #dddddd; background: #232428; border: 1px solid #34363b; padding: 3px 5px; }"

        // Dock chrome + gaps
        "QDockWidget::title { background: #202024; padding: 4px 8px; }"
        "QMainWindow::separator { background: #0d0e10; width: 4px; height: 4px; }"
        "QSplitter::handle { background: #0d0e10; }"

        // Menu bar + menus
        "QMenuBar { background: #202024; }"
        "QMenuBar::item { padding: 4px 9px; background: transparent; }"
        "QMenuBar::item:selected { background: rgba(255,255,255,0.11); border-radius: 4px; }"
        "QMenu { background: #232428; border: 1px solid #34363b; padding: 4px; }"
        "QMenu::item { padding: 5px 18px; border-radius: 5px; }"
        "QMenu::item:selected { background: rgba(255,255,255,0.14); }"
        "QMenu::separator { height: 1px; background: #34363b; margin: 4px 10px; }"

        // Tool buttons — full rounded fill, matching the transport IconButtons
        "QToolButton { background: transparent; border: none; border-radius: 6px; padding: 3px; }"
        "QToolButton:hover { background: rgba(255,255,255,0.11); }"
        "QToolButton:pressed { background: rgba(255,255,255,0.20); }"
        "QToolButton:checked { background: rgba(255,255,255,0.16); }"
        // Embedded line-edit buttons (e.g. the filter's clear button) stay compact.
        "QLineEdit QToolButton { padding: 0px; border-radius: 3px; }"

        // Item views — neutral rounded row highlight
        "QTreeView, QListView { background: #18191b; border: none; outline: none; }"
        "QTreeView::item { padding: 3px; border-radius: 4px; }"
        "QTreeView::item:hover { background: rgba(255,255,255,0.06); }"
        "QTreeView::item:selected { background: rgba(255,255,255,0.14); color: #f0f0f0; }"
        "QHeaderView::section { background: #202024; color: #9a9a9a; border: none;"
        " border-right: 1px solid #2c2d31; padding: 4px 6px; }"

        // Line edits
        "QLineEdit { background: #17181a; border: 1px solid #2c2d31; border-radius: 6px;"
        " padding: 3px 7px; color: #dddddd; selection-background-color: #3a3d44; }"
        "QLineEdit:focus { border: 1px solid #45484f; }"

        // Media Info
        "QPlainTextEdit { background: #1b1c1f; border: none; }"

        // Thin modern scrollbars
        "QScrollBar:vertical { background: transparent; width: 11px; margin: 2px; }"
        "QScrollBar::handle:vertical { background: rgba(255,255,255,0.13); border-radius: 4px; min-height: 26px; }"
        "QScrollBar::handle:vertical:hover { background: rgba(255,255,255,0.22); }"
        "QScrollBar:horizontal { background: transparent; height: 11px; margin: 2px; }"
        "QScrollBar::handle:horizontal { background: rgba(255,255,255,0.13); border-radius: 4px; min-width: 26px; }"
        "QScrollBar::handle:horizontal:hover { background: rgba(255,255,255,0.22); }"
        "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }"
        "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }"

        // Status bar
        "QStatusBar { background: #202024; color: #8a8a8a; }"
        "QStatusBar::item { border: none; }");
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("hopline");
    QApplication::setOrganizationName("hopline");

    applyDarkTheme(app);

    hopline::MainWindow window;
    if (argc > 1) {
        window.load(QString::fromLocal8Bit(argv[1]));
    }
    window.showMaximized();

    return app.exec();
}

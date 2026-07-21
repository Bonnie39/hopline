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

    const QColor window(32, 33, 36);
    const QColor base(24, 25, 27);
    const QColor panel(45, 46, 49);
    const QColor text(221, 221, 221);
    const QColor muted(130, 130, 130);
    const QColor accent(74, 127, 200);

    QPalette p;
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::ToolTipBase, panel);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, panel);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Link, accent);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::PlaceholderText, muted);
    p.setColor(QPalette::Disabled, QPalette::Text, muted);
    p.setColor(QPalette::Disabled, QPalette::WindowText, muted);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, muted);
    app.setPalette(p);

    app.setStyleSheet(
        "QToolTip { color: #dddddd; background: #2d2e31; border: 1px solid #3a3b3e; }"
        "QDockWidget::title { background: #2a2b2e; padding: 4px 8px; }"
        "QMenuBar { background: #26272a; }"
        "QMenuBar::item:selected { background: #3a3b3e; }"
        "QMenu { background: #2a2b2e; border: 1px solid #3a3b3e; }"
        "QMenu::item:selected { background: #4a7fc8; }"
        "QSplitter::handle { background: #3a3b3e; }");
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

#include <QApplication>

#include "app/MainWindow.h"

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("hopline");
    QApplication::setOrganizationName("hopline");

    hopline::MainWindow window;
    if (argc > 1) {
        window.load(QString::fromLocal8Bit(argv[1]));
    }
    window.show();

    return app.exec();
}

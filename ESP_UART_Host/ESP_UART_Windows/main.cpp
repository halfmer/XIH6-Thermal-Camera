#include <QApplication>
#include <QFile>
#include <QIcon>
#include <QStyleFactory>
#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // Use Fusion style as base — consistent cross-platform look
    app.setStyle(QStyleFactory::create("Fusion"));

    // Load dark theme QSS
    QFile qss(":/style.qss");
    if (qss.open(QIODevice::ReadOnly | QIODevice::Text)) {
        app.setStyleSheet(qss.readAll());
        qss.close();
    }

    // Set application icon
    app.setWindowIcon(QIcon(":/icon.png"));
    app.setApplicationName("UART Host");
    app.setApplicationVersion("0.2.0");

    MainWindow window;
    window.show();

    return app.exec();
}

#include <QApplication>
///home/pum/下载/RTSP_Server/build/rtsp_server
#include "MainWindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    MainWindow window;
    window.show();
    return app.exec();
}

#include "../include/TcpServer.h"
#include "../include/MainWindow.h" 
#include <QApplication>            
#include <thread>                  
#include <atomic>                  
#include <iostream>
#include <csignal>                 

// 全局标志位
std::atomic<bool> g_running(true);
std::atomic<int> g_server_port_allocator(6970); 

void signalHandler(int signum) {
    std::cout << "\n🛑 收到信号 (" << signum << ")，正在收摊..." << std::endl;
    g_running = false;
    if (qApp) {
        QMetaObject::invokeMethod(qApp, "quit", Qt::QueuedConnection);
    }
}

int main(int argc, char *argv[]) {
    signal(SIGINT, signalHandler);

    QApplication a(argc, argv);
    
    // 1. 💥 提前实例化服务器和老板娘（必须都在主线程创建，信号槽才能跨线程工作）
    MainWindow w;
    TcpServer server;

    // 2. 🔌 核心电线对接：把界面的按钮圣旨，直接传给服务器的总闸！
    QObject::connect(&w, &MainWindow::requestPauseServer, &server, &TcpServer::stopAllStreams);
    QObject::connect(&w, &MainWindow::requestResumeServer, &server, &TcpServer::resumeAllStreams);
    QObject::connect(&server, &TcpServer::dataSent, &w, &MainWindow::addTraffic);
    std::thread serverThread([&server]() {
        std::cout << ">>> 🚀 RTSP 服务器在子线程启动 (Port: 8554) <<<" << std::endl;
        if (!server.start(8554)) {
            std::cerr << "💥 服务器启动失败！" << std::endl;
            if (qApp) QMetaObject::invokeMethod(qApp, "quit", Qt::QueuedConnection);
        }
    });

    // 🌟 在退出时通知服务线程
    QObject::connect(&a, &QApplication::aboutToQuit, [&]() {
        g_running = false;
    });

    // 4. 老板娘登场
    w.setWindowTitle("RTSP Gateway Monitor | 欲萌专属版");
    w.show(); 

    int ret = a.exec(); 

    // 🌟 优雅退出：等待子线程收工
    if (serverThread.joinable()) {
        serverThread.join();
    }

    return ret; 
}
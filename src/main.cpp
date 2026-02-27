#include "../include/TcpServer.h"
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
}

int main(int argc, char *argv[]) {
    signal(SIGINT, signalHandler);

    // 1. 💥 实例化服务器（无界面模式）
    TcpServer server;

    std::thread serverThread([&server]() {
        std::cout << ">>> 🚀 RTSP 服务器在子线程启动 (Port: 8554) <<<" << std::endl;
        if (!server.start(8554)) {
            std::cerr << "💥 服务器启动失败！" << std::endl;
            g_running = false;
        }
    });

    std::cout << "\n✅ [Headless Mode] 服务器已在后台纯净运行！现在可以去 VLC 拉流了！按 Ctrl+C 退出。" << std::endl;

    // 主线程死循环等待，直到收到 Ctrl+C (g_running 变为 false)
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::cout << "🛑 主线程开始回收资源..." << std::endl;
    // 🌟 优雅退出：等待子线程收工
    if (serverThread.joinable()) {
        serverThread.join();
    }

    return 0; 
}
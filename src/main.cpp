#include "../include/TcpServer.h"
#include "../include/CameraCapture.h"
#include <thread>
#include <atomic>
#include <iostream>
#include <csignal>

std::atomic<bool> g_running(true);
std::atomic<int>  g_server_port_allocator(6970);

void signalHandler(int signum) {
    std::cout << "\n🛑 收到信号 (" << signum << ")，正在收摊..." << std::endl;
    g_running = false;
}

int main() {
    signal(SIGINT,  signalHandler);
    signal(SIGTERM, signalHandler);

    TcpServer server;

    // 1️⃣ 在子线程里启动 RTSP 服务（epoll 事件循环）
    std::thread serverThread([&server]() {
        std::cout << ">>> 🚀 RTSP 服务器在子线程启动 (Port: 8554) <<<" << std::endl;
        if (!server.start(8554)) {
            std::cerr << "💥 服务器启动失败！" << std::endl;
            g_running = false;
        }
    });

    // 2️⃣ 在子线程里启动摄像头采集+编码
    //    onNalu 回调：把每一帧 NALU 透过 ThreadPool 异步分发给所有客户端
    CameraCapture camera;
    std::thread captureThread([&camera, &server]() {
        std::cout << ">>> 📸 摄像头采集线程启动 <<<" << std::endl;
        camera.startCaptureAndEncode(&g_running, [&server](uint8_t* data, int size, uint32_t ts) {
            server.dispatchNalu(data, size, ts);
        });
    });

    std::cout << "\n✅ [EdgeGateway] 系统运行中！用 VLC 打开 rtsp://<本机IP>:8554/ 即可观看。\n"
              << "   按 Ctrl+C 退出。\n" << std::endl;

    // 主线程等待退出信号
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::cout << "🛑 主线程开始回收资源..." << std::endl;

    if (captureThread.joinable()) captureThread.join();
    if (serverThread.joinable())  serverThread.join();

    std::cout << "👋 程序退出" << std::endl;
    return 0;
}
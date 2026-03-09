#include "CameraCapture.h"
#include "TcpServer.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

std::atomic<bool> g_running(true);

namespace {
struct AppConfig {
    int port = 8554;
    std::string input = "camera";
    std::string file;
    CaptureOptions capture;
};

void printUsage(const char* prog) {
    std::cout
        << "Usage: " << prog << " [options] [file]\n"
        << "Options:\n"
        << "  --port <int>             RTSP port (default: 8554)\n"
        << "  --input <camera|file>    Input mode (default: camera)\n"
        << "  --file <path>            H264 file path for file mode\n"
        << "  --device <path>          Camera device (default: /dev/video0)\n"
        << "  --width <int>            Capture width (default: 1280)\n"
        << "  --height <int>           Capture height (default: 720)\n"
        << "  --fps <int>              Capture fps (default: 30)\n"
        << "  --help                   Show help\n";
}

bool parseIntArg(const char* value, int& out) {
    if (!value) return false;
    char* endptr = nullptr;
    long parsed = strtol(value, &endptr, 10);
    if (*value == '\0' || (endptr && *endptr != '\0')) {
        return false;
    }
    out = static_cast<int>(parsed);
    return true;
}

bool parseArgs(int argc, char** argv, AppConfig& cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto requireValue = [&](int& index) -> const char* {
            if (index + 1 >= argc) return nullptr;
            index++;
            return argv[index];
        };

        if (arg == "--help") {
            printUsage(argv[0]);
            return false;
        }
        if (arg == "--port") {
            const char* v = requireValue(i);
            if (!v || !parseIntArg(v, cfg.port)) return false;
            continue;
        }
        if (arg == "--input") {
            const char* v = requireValue(i);
            if (!v) return false;
            cfg.input = v;
            continue;
        }
        if (arg == "--file") {
            const char* v = requireValue(i);
            if (!v) return false;
            cfg.file = v;
            continue;
        }
        if (arg == "--device") {
            const char* v = requireValue(i);
            if (!v) return false;
            cfg.capture.device = v;
            continue;
        }
        if (arg == "--width") {
            const char* v = requireValue(i);
            if (!v || !parseIntArg(v, cfg.capture.width)) return false;
            continue;
        }
        if (arg == "--height") {
            const char* v = requireValue(i);
            if (!v || !parseIntArg(v, cfg.capture.height)) return false;
            continue;
        }
        if (arg == "--fps") {
            const char* v = requireValue(i);
            if (!v || !parseIntArg(v, cfg.capture.fps)) return false;
            continue;
        }

        if (!arg.empty() && arg[0] != '-') {
            // 兼容旧用法：单个位置参数视为文件输入。
            cfg.input = "file";
            cfg.file = arg;
            continue;
        }

        return false;
    }
    return true;
}
} // namespace

void signalHandler(int signum) {
    std::cout << "\n[INFO] 收到信号(" << signum << ")，准备退出..." << std::endl;
    g_running = false;
}

int main(int argc, char** argv) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    AppConfig config;
    if (!parseArgs(argc, argv, config)) {
        printUsage(argv[0]);
        return 1;
    }

    if (config.input != "camera" && config.input != "file") {
        std::cerr << "[ERROR] --input 仅支持 camera 或 file" << std::endl;
        return 1;
    }

    if (config.input == "file" && config.file.empty()) {
        std::cerr << "[ERROR] file 模式必须提供 --file <path> 或位置参数" << std::endl;
        return 1;
    }

    std::cout << "[INFO] RTSP server start: port=" << config.port
              << " input=" << config.input << std::endl;

    TcpServer server;

    std::thread serverThread([&server, &config]() {
        if (!server.start(config.port)) {
            std::cerr << "[ERROR] 服务器启动失败" << std::endl;
            g_running = false;
        }
    });

    CameraCapture camera;
    std::thread captureThread([&]() {
        if (config.input == "file") {
            std::cout << "[INFO] 文件推流模式: " << config.file << std::endl;
            camera.startCaptureFromFile(config.file, &g_running,
                                        [&server](uint8_t* data, int size, uint32_t ts) {
                                            server.dispatchNalu(data, size, ts);
                                        });
        } else {
            std::cout << "[INFO] 摄像头模式: device=" << config.capture.device
                      << " " << config.capture.width << "x" << config.capture.height
                      << "@" << config.capture.fps << std::endl;
            camera.startCaptureAndEncode(config.capture, &g_running,
                                         [&server](uint8_t* data, int size, uint32_t ts) {
                                             server.dispatchNalu(data, size, ts);
                                         });
        }
    });

    std::cout << "[INFO] 运行中，VLC 打开: rtsp://<ip>:" << config.port << "/" << std::endl;

    auto last_stat = std::chrono::steady_clock::now();
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto now = std::chrono::steady_clock::now();
        if (now - last_stat >= std::chrono::seconds(5)) {
            last_stat = now;
            std::cout << "[STAT] clients=" << server.onlineClients()
                      << " traffic_bytes=" << server.totalTrafficBytes() << std::endl;
        }
    }

    std::cout << "[INFO] 等待线程退出..." << std::endl;
    if (captureThread.joinable()) captureThread.join();
    if (serverThread.joinable()) serverThread.join();

    std::cout << "[INFO] 程序退出" << std::endl;
    return 0;
}

#pragma once
#include <functional>
#include <cstdint>
#include <atomic>
#include <string>

// NALU 回调类型：(数据指针, 数据长度, 时间戳)
using NaluCallback = std::function<void(uint8_t*, int, uint32_t)>;

struct CaptureOptions {
    std::string device = "/dev/video0";
    int width = 1280;
    int height = 720;
    int fps = 30;
    std::string input_format = "yuyv422";
};

class CameraCapture {
public:
    CameraCapture();
    ~CameraCapture();

    // 启动采集+编码，内部自己跑死循环，通过 onNalu 往外推数据
    void startCaptureAndEncode(const CaptureOptions& options, std::atomic<bool>* running_flag, NaluCallback onNalu);

    // 从本地 H264 Annex-B 文件推流（用于没有 /dev/video0 的测试环境）
    void startCaptureFromFile(const std::string& filename, std::atomic<bool>* running_flag, NaluCallback onNalu);
};

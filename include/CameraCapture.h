#pragma once
#include <functional>
#include <cstdint>
#include <atomic>

// NALU 回调类型：(数据指针, 数据长度, 时间戳)
using NaluCallback = std::function<void(uint8_t*, int, uint32_t)>;

class CameraCapture {
public:
    CameraCapture();
    ~CameraCapture();

    // 启动采集+编码，内部自己跑死循环，通过 onNalu 往外推数据
    void startCaptureAndEncode(std::atomic<bool>* running_flag, NaluCallback onNalu);
};

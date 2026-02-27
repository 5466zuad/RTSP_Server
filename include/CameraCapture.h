#ifndef CAMERA_CAPTURE_H
#define CAMERA_CAPTURE_H

#include <atomic>
#include <functional>
#include <stdint.h>

class CameraCapture {
public:
    // 回调函数：返回 NALU 数据指针、大小、以及时间戳
    using NaluCallback = std::function<void(uint8_t* nalu_data, int nalu_size, uint32_t timestamp)>;

    CameraCapture();
    ~CameraCapture();

    // 开始采集与编码，直到 running_flag 为 false
    void startCaptureAndEncode(std::atomic<bool>* running_flag, NaluCallback onNalu);
};

#endif

#pragma once
#include <functional>
#include <cstdint>
#include <atomic>
#include <memory>
#include "video/VideoSource.h"
#include "video/FrameScaler.h"
#include "video/H264Encoder.h"

class CameraCapture {
public:
    CameraCapture();
    ~CameraCapture();

    void startCaptureAndEncode(std::atomic<bool>* running_flag, NaluCallback onNalu);

private:
    std::unique_ptr<VideoSource> source;
    std::unique_ptr<FrameScaler> scaler;
    std::unique_ptr<H264Encoder> encoder;
};

#pragma once
#include <string>

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
}

// 封装针对 V4L2 或者本地文件的采集类
class VideoSource {
public:
    VideoSource();
    ~VideoSource();

    bool openDevice(const std::string& device_path);
    void closeDevice();

    int readFrame(AVPacket* pkt);

    AVFormatContext* getFormatContext() const { return fmt_ctx; }
    int getVideoStreamIndex() const { return video_stream_idx; }

private:
    AVFormatContext* fmt_ctx = nullptr;
    int video_stream_idx = -1;
};

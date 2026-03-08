#include "VideoSource.h"
#include <iostream>

VideoSource::VideoSource() {
    avdevice_register_all();
}

VideoSource::~VideoSource() {
    closeDevice();
}

bool VideoSource::openDevice(const std::string& device_path) {
    fmt_ctx = avformat_alloc_context();
    const AVInputFormat* iformat = av_find_input_format("video4linux2");
    
    int ret = avformat_open_input(&fmt_ctx, device_path.c_str(), iformat, nullptr);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        std::cerr << "❌ 打开摄像头 " << device_path << " 失败: " << err << std::endl;
        return false;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << "❌ 无法获取摄像头流信息" << std::endl;
        return false;
    }

    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }

    if (video_stream_idx < 0) {
        std::cerr << "❌ 摄像头中没有找到视频流" << std::endl;
        return false;
    }

    AVCodecParameters* codecpar = fmt_ctx->streams[video_stream_idx]->codecpar;
    int width  = codecpar->width;
    int height = codecpar->height;
    AVPixelFormat src_pix_fmt = (AVPixelFormat)codecpar->format;

    std::cout << "✅ 摄像头已打开: " << width << "x" << height
              << " (格式=" << av_get_pix_fmt_name(src_pix_fmt) << ")" << std::endl;
    return true;
}

void VideoSource::closeDevice() {
    if (fmt_ctx) {
        avformat_close_input(&fmt_ctx);
        fmt_ctx = nullptr;
    }
}

int VideoSource::readFrame(AVPacket* pkt) {
    if (!fmt_ctx) return -1;
    return av_read_frame(fmt_ctx, pkt);
}

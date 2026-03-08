#pragma once
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

// 负责将不同硬件格式转换/解码为标准 YUV420P
class FrameScaler {
public:
    FrameScaler();
    ~FrameScaler();

    bool init(AVCodecParameters* codecpar);
    
    // 输入一个解复用的 packet，输出将缩放/解码好的数据写入目标平面(比如x264的平面)
    // 成功返回 true
    bool processPacket(AVPacket* pkt, uint8_t* dst_planes[4], int dst_strides[4]);

private:
    void cleanup();

    AVCodecContext* dec_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;
    AVFrame* raw_frame = nullptr;

    bool need_decode = false;
    int width = 0;
    int height = 0;
    AVPixelFormat src_pix_fmt = AV_PIX_FMT_NONE;
};

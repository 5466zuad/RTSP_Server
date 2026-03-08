#include "video/FrameScaler.h"
#include <iostream>

FrameScaler::FrameScaler() {}

FrameScaler::~FrameScaler() {
    cleanup();
}

void FrameScaler::cleanup() {
    if (dec_ctx) avcodec_free_context(&dec_ctx);
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (raw_frame) av_frame_free(&raw_frame);
    dec_ctx = nullptr;
    sws_ctx = nullptr;
    raw_frame = nullptr;
}

bool FrameScaler::init(AVCodecParameters* codecpar) {
    cleanup();

    width = codecpar->width;
    height = codecpar->height;
    src_pix_fmt = (AVPixelFormat)codecpar->format;
    need_decode = (codecpar->codec_id != AV_CODEC_ID_RAWVIDEO);

    if (need_decode) {
        const AVCodec* decoder = avcodec_find_decoder(codecpar->codec_id);
        if (!decoder) return false;

        dec_ctx = avcodec_alloc_context3(decoder);
        avcodec_parameters_to_context(dec_ctx, codecpar);
        if (avcodec_open2(dec_ctx, decoder, nullptr) < 0) {
            return false;
        }
        std::cout << "✅ 已启用解码器: " << decoder->name << std::endl;
    }

    raw_frame = av_frame_alloc();
    
    // 初始化 sws_ctx
    sws_ctx = sws_getContext(
        width, height, need_decode ? dec_ctx->pix_fmt : src_pix_fmt,
        width, height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    return sws_ctx != nullptr;
}

bool FrameScaler::processPacket(AVPacket* pkt, uint8_t* dst_planes[4], int dst_strides[4]) {
    if (need_decode) {
        if (avcodec_send_packet(dec_ctx, pkt) < 0) return false;
        
        bool frame_found = false;
        while (avcodec_receive_frame(dec_ctx, raw_frame) >= 0) {
            sws_scale(sws_ctx,
                      raw_frame->data, raw_frame->linesize,
                      0, height,
                      dst_planes, dst_strides);
            av_frame_unref(raw_frame);
            frame_found = true;
        }
        return frame_found;
    } else {
        uint8_t* src_data[4] = {nullptr};
        int src_linesize[4] = {0};
        av_image_fill_arrays(src_data, src_linesize,
                             pkt->data, src_pix_fmt, width, height, 1);
        sws_scale(sws_ctx,
                  src_data, src_linesize,
                  0, height,
                  dst_planes, dst_strides);
        return true;
    }
}

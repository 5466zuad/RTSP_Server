// CameraCapture.cpp
// 功能：直接用 libavdevice (v4l2) 从 /dev/video0 采集画面，
//       经 sws_scale 转 YUV420P，再用 x264 压缩成 H264 NALU，
//       通过 onNalu 回调异步推出去。
#include "../include/CameraCapture.h"
#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <x264.h>
}

CameraCapture::CameraCapture() {}
CameraCapture::~CameraCapture() {}

void CameraCapture::startCaptureAndEncode(std::atomic<bool>* running_flag, NaluCallback onNalu) {

    // ─────────────────────────────────────────────────────────
    // 1️⃣  初始化：注册所有硬件采集驱动
    // ─────────────────────────────────────────────────────────
    avdevice_register_all();

    // 打开本地视频文件
    AVFormatContext* fmt_ctx = avformat_alloc_context();
    int ret = avformat_open_input(&fmt_ctx, "download/test.mp4", nullptr, nullptr);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        std::cerr << "❌ 打开视频文件 download/test.mp4 失败: " << err << std::endl;
        std::cerr << "请确保项目根目录下有 download/test.mp4 文件！" << std::endl;
        return;
    }

    // 读取流信息（得到宽高等参数）
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << "❌ 无法获取摄像头流信息" << std::endl;
        avformat_close_input(&fmt_ctx);
        return;
    }

    // 找到视频流索引
    int video_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }
    if (video_stream_idx < 0) {
        std::cerr << "❌ 摄像头中没有找到视频流" << std::endl;
        avformat_close_input(&fmt_ctx);
        return;
    }

    AVCodecParameters* codecpar = fmt_ctx->streams[video_stream_idx]->codecpar;
    int width  = codecpar->width;
    int height = codecpar->height;
    AVPixelFormat src_pix_fmt = (AVPixelFormat)codecpar->format;

    std::cout << "✅ 摄像头已打开: " << width << "x" << height
              << " (格式=" << av_get_pix_fmt_name(src_pix_fmt) << ")" << std::endl;

    // ─────────────────────────────────────────────────────────
    // 2️⃣  如果摄像头吐出压缩格式（如 MJPEG），需要解码器
    // ─────────────────────────────────────────────────────────
    const AVCodec* decoder = avcodec_find_decoder(codecpar->codec_id);
    AVCodecContext* dec_ctx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(dec_ctx, codecpar);
    bool need_decode = (codecpar->codec_id != AV_CODEC_ID_RAWVIDEO);
    if (need_decode) {
        if (avcodec_open2(dec_ctx, decoder, nullptr) < 0) {
            std::cerr << "❌ 解码器打开失败" << std::endl;
        } else {
            std::cout << "✅ 已启用解码器: " << decoder->name << std::endl;
        }
    }

    // ─────────────────────────────────────────────────────────
    // 3️⃣  sws_scale 转换上下文：任意格式 → YUV420P
    // ─────────────────────────────────────────────────────────
    SwsContext* sws_ctx = sws_getContext(
        width, height, need_decode ? dec_ctx->pix_fmt : src_pix_fmt,
        width, height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    if (!sws_ctx) {
        std::cerr << "❌ sws_getContext 失败" << std::endl;
        avformat_close_input(&fmt_ctx);
        return;
    }

    //（内存已由 x264_picture_alloc 自动分配并在最终自动释放，无需手动额外开辟 YUV_data 副本）

    // ─────────────────────────────────────────────────────────
    // 4️⃣  x264 编码器初始化
    // ─────────────────────────────────────────────────────────
    x264_param_t param;
    x264_param_default_preset(&param, "ultrafast", "zerolatency");
    param.i_threads    = 0;     // 自动选线程数
    param.i_width      = width;
    param.i_height     = height;
    param.i_fps_num    = 30;
    param.i_fps_den    = 1;
    param.i_keyint_max = 60;
    param.b_intra_refresh = 0;
    param.rc.i_rc_method  = X264_RC_CRF;
    param.rc.f_rf_constant = 20.0f;
    x264_param_apply_profile(&param, "main");

    x264_t* encoder = x264_encoder_open(&param);
    if (!encoder) {
        std::cerr << "❌ x264 编码器初始化失败" << std::endl;
        sws_freeContext(sws_ctx);
        avformat_close_input(&fmt_ctx);
        return;
    }
    std::cout << "✅ x264 编码器初始化成功" << std::endl;

    x264_picture_t pic_in, pic_out;
    x264_picture_alloc(&pic_in, X264_CSP_I420, width, height);

    // ─────────────────────────────────────────────────────────
    // 5️⃣  主采集循环
    // ─────────────────────────────────────────────────────────
    AVPacket* pkt = av_packet_alloc();
    AVFrame*  raw_frame = av_frame_alloc(); // 解码后的原始帧

    uint32_t pts_counter = 0;
    const uint32_t pts_step = 90000 / 30; // 90kHz 时钟，每帧 3000

    std::cout << "🎬 开始采集循环..." << std::endl;

    auto next_frame_time = std::chrono::steady_clock::now();
    const auto frame_duration = std::chrono::milliseconds(33); // ~30 fps

    while (running_flag && *running_flag) {

        // 从文件读取一个数据包
        int ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                std::cout << "🔄 视频播放结束，自动重新循环播放..." << std::endl;
                avio_seek(fmt_ctx->pb, 0, SEEK_SET);
                avformat_seek_file(fmt_ctx, -1, INT64_MIN, 0, INT64_MAX, 0);
            } else {
                std::cerr << "⚠️  av_read_frame 读取失败" << std::endl;
            }
            av_packet_unref(pkt);
            continue;
        }

        if (pkt->stream_index != video_stream_idx) {
            av_packet_unref(pkt);
            continue;
        }

        // 等待直到下一帧的时间点，保持相对恒定的平滑帧率
        next_frame_time += frame_duration;
        std::this_thread::sleep_until(next_frame_time);

        // ── 格式清洗：直接转换并在 x264 的 pic_in 分配的内存中输出 ──
        uint8_t* src_data[4]     = {nullptr};
        int      src_linesize[4] = {0};
        
        bool frame_ready = false;

        if (need_decode) {
            // MJPEG / 其他压缩格式需要先解码
            if (avcodec_send_packet(dec_ctx, pkt) >= 0) {
                while (avcodec_receive_frame(dec_ctx, raw_frame) >= 0) {
                    // 用解码出来的帧做 sws_scale，直接输出到 pic_in
                    sws_scale(sws_ctx,
                              raw_frame->data, raw_frame->linesize,
                              0, height,
                              pic_in.img.plane, pic_in.img.i_stride);
                    av_frame_unref(raw_frame);
                    frame_ready = true;
                }
            }
        } else {
            // 原始格式（YUYV 等）：直接填 src_data 做 sws_scale，输出到 pic_in
            av_image_fill_arrays(src_data, src_linesize,
                                 pkt->data, src_pix_fmt, width, height, 1);
            sws_scale(sws_ctx,
                      src_data, src_linesize,
                      0, height,
                      pic_in.img.plane, pic_in.img.i_stride);
            frame_ready = true;
        }

        av_packet_unref(pkt);

        if (!frame_ready) {
            continue;
        }

        pic_in.i_pts = pts_counter;

        // ── 编码 ──
        x264_nal_t* nals     = nullptr;
        int         num_nals = 0;
        int frame_size = x264_encoder_encode(encoder, &nals, &num_nals, &pic_in, &pic_out);

        if (frame_size > 0 && onNalu) {
            for (int i = 0; i < num_nals; ++i) {
                onNalu(nals[i].p_payload, nals[i].i_payload, pts_counter);
            }
        }

        pts_counter += pts_step;
    }

    // ─────────────────────────────────────────────────────────
    // 6️⃣  清理资源
    // ─────────────────────────────────────────────────────────
    std::cout << "🛑 采集线程正在清理资源..." << std::endl;
    x264_picture_clean(&pic_in);
    x264_encoder_close(encoder);
    av_frame_free(&raw_frame);
    av_packet_free(&pkt);
    sws_freeContext(sws_ctx);
    if (need_decode) avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    std::cout << "✅ 采集线程已安全退出" << std::endl;
}

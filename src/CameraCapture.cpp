// CameraCapture.cpp
// 功能：直接用 libavdevice (v4l2) 从 /dev/video0 采集画面，
//       经 sws_scale 转 YUV420P，再用 x264 压缩成 H264 NALU，
//       通过 onNalu 回调异步推出去。
#include "../include/CameraCapture.h"
#include <iostream>
#include <cstring>

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

    // 找到 video4linux2 "输入格式"
    AVInputFormat* input_fmt = av_find_input_format("video4linux2");
    if (!input_fmt) {
        std::cerr << "❌ 找不到 v4l2 驱动，请确认内核支持 v4l2" << std::endl;
        return;
    }

    // 设置采集参数字典：分辨率 + 帧率
    AVDictionary* options = nullptr;
    av_dict_set(&options, "video_size", "1280x720", 0);
    av_dict_set(&options, "framerate",  "30",       0);
    av_dict_set(&options, "input_format", "yuyv422", 0); // 大多数 USB 摄像头支持

    // 打开摄像头（独占 /dev/video0）
    AVFormatContext* fmt_ctx = avformat_alloc_context();
    int ret = avformat_open_input(&fmt_ctx, "/dev/video0", input_fmt, &options);
    av_dict_free(&options);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        std::cerr << "❌ 打开摄像头失败: " << err << std::endl;
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

    // 申请 YUV 输出帧的内存（plane[0]=Y, plane[1]=U, plane[2]=V）
    uint8_t* yuv_data[4] = {nullptr};
    int      yuv_linesize[4];
    av_image_alloc(yuv_data, yuv_linesize, width, height, AV_PIX_FMT_YUV420P, 32);

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
        av_freep(&yuv_data[0]);
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

    while (running_flag && *running_flag) {

        // 从摄像头读取一个数据包
        if (av_read_frame(fmt_ctx, pkt) < 0) {
            std::cerr << "⚠️  av_read_frame 失败，尝试重新读取..." << std::endl;
            av_packet_unref(pkt);
            continue;
        }

        if (pkt->stream_index != video_stream_idx) {
            av_packet_unref(pkt);
            continue;
        }

        // ── 格式清洗：把原始/压缩帧统一转换成 YUV420P ──
        uint8_t* src_data[4]     = {nullptr};
        int      src_linesize[4] = {0};

        if (need_decode) {
            // MJPEG / 其他压缩格式需要先解码
            if (avcodec_send_packet(dec_ctx, pkt) >= 0) {
                if (avcodec_receive_frame(dec_ctx, raw_frame) >= 0) {
                    // 用解码出来的帧做 sws_scale
                    sws_scale(sws_ctx,
                              raw_frame->data, raw_frame->linesize,
                              0, height,
                              yuv_data, yuv_linesize);
                    av_frame_unref(raw_frame);
                }
            }
        } else {
            // 原始格式（YUYV 等）：直接填 src_data 做 sws_scale
            av_image_fill_arrays(src_data, src_linesize,
                                 pkt->data, src_pix_fmt, width, height, 1);
            sws_scale(sws_ctx,
                      src_data, src_linesize,
                      0, height,
                      yuv_data, yuv_linesize);
        }

        av_packet_unref(pkt);

        // ── 填充 x264 输入结构体 ──
        int y_size = width * height;
        memcpy(pic_in.img.plane[0], yuv_data[0], y_size);
        memcpy(pic_in.img.plane[1], yuv_data[1], y_size / 4);
        memcpy(pic_in.img.plane[2], yuv_data[2], y_size / 4);
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
    av_freep(&yuv_data[0]);
    sws_freeContext(sws_ctx);
    if (need_decode) avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    std::cout << "✅ 采集线程已安全退出" << std::endl;
}

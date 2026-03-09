// CameraCapture.cpp
// 功能：直接用 libavdevice (v4l2) 从 /dev/video0 采集画面，
//       经 sws_scale 转 YUV420P，再用 x264 压缩成 H264 NALU，
//       通过 onNalu 回调异步推出去。
#include "CameraCapture.h"
#include <iostream>
#include <cstring>
#include <fstream>
#include <vector>
#include <thread>
#include <chrono>

namespace {
struct NaluRange {
    size_t offset;
    size_t size;
};

bool findAnnexBStartCode(const std::vector<uint8_t>& buffer,
                         size_t from,
                         size_t& code_pos,
                         size_t& code_len) {
    if (buffer.size() < 3 || from >= buffer.size()) {
        return false;
    }

    for (size_t i = from; i + 2 < buffer.size(); ++i) {
        // Prefer 4-byte start code matching first to avoid ambiguity.
        if (i + 3 < buffer.size() &&
            buffer[i] == 0 && buffer[i + 1] == 0 && buffer[i + 2] == 0 && buffer[i + 3] == 1) {
            code_pos = i;
            code_len = 4;
            return true;
        }
        if (buffer[i] == 0 && buffer[i + 1] == 0 && buffer[i + 2] == 1) {
            code_pos = i;
            code_len = 3;
            return true;
        }
    }
    return false;
}

std::vector<NaluRange> extractAnnexBNalus(const std::vector<uint8_t>& buffer) {
    std::vector<NaluRange> nalus;
    size_t search_pos = 0;

    size_t start_code_pos = 0;
    size_t start_code_len = 0;
    while (findAnnexBStartCode(buffer, search_pos, start_code_pos, start_code_len)) {
        const size_t nalu_start = start_code_pos + start_code_len;

        size_t next_code_pos = 0;
        size_t next_code_len = 0;
        if (findAnnexBStartCode(buffer, nalu_start, next_code_pos, next_code_len)) {
            if (next_code_pos > nalu_start) {
                nalus.push_back({nalu_start, next_code_pos - nalu_start});
            }
            search_pos = next_code_pos;
            continue;
        }

        if (nalu_start < buffer.size()) {
            nalus.push_back({nalu_start, buffer.size() - nalu_start});
        }
        break;
    }

    return nalus;
}
} // namespace

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

void CameraCapture::startCaptureAndEncode(const CaptureOptions& options, std::atomic<bool>* running_flag, NaluCallback onNalu) {

    const int fps = (options.fps > 0) ? options.fps : 30;
    const std::string video_size = std::to_string(options.width) + "x" + std::to_string(options.height);

    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* dec_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;
    uint8_t* yuv_data[4] = {nullptr};
    int yuv_linesize[4] = {0};
    bool yuv_allocated = false;
    x264_t* encoder = nullptr;
    x264_picture_t pic_in;
    x264_picture_t pic_out;
    bool pic_allocated = false;
    AVPacket* pkt = nullptr;
    AVFrame* raw_frame = nullptr;
    bool need_decode = false;
    int video_stream_idx = -1;
    int width = 0;
    int height = 0;
    AVPixelFormat src_pix_fmt = AV_PIX_FMT_NONE;
    uint32_t pts_counter = 0;
    uint32_t pts_step = 0;
    int consecutive_read_failures = 0;
    AVCodecParameters* codecpar = nullptr;
    const AVCodec* decoder = nullptr;

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
    AVDictionary* ff_options = nullptr;
    av_dict_set(&ff_options, "video_size", video_size.c_str(), 0);
    av_dict_set(&ff_options, "framerate", std::to_string(fps).c_str(), 0);
    av_dict_set(&ff_options, "input_format", options.input_format.c_str(), 0);

    // 打开摄像头（独占 /dev/video0）
    fmt_ctx = avformat_alloc_context();
    int ret = avformat_open_input(&fmt_ctx, options.device.c_str(), input_fmt, &ff_options);
    av_dict_free(&ff_options);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        std::cerr << "❌ 打开摄像头失败: " << options.device << " err=" << err << std::endl;
        goto cleanup;
    }

    // 读取流信息（得到宽高等参数）
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << "❌ 无法获取摄像头流信息" << std::endl;
        goto cleanup;
    }

    // 找到视频流索引
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }
    if (video_stream_idx < 0) {
        std::cerr << "❌ 摄像头中没有找到视频流" << std::endl;
        goto cleanup;
    }

    codecpar = fmt_ctx->streams[video_stream_idx]->codecpar;
    width = codecpar->width;
    height = codecpar->height;
    src_pix_fmt = (AVPixelFormat)codecpar->format;

    std::cout << "✅ 摄像头已打开: " << width << "x" << height
              << " (格式=" << av_get_pix_fmt_name(src_pix_fmt) << ")" << std::endl;

    // ─────────────────────────────────────────────────────────
    // 2️⃣  如果摄像头吐出压缩格式（如 MJPEG），需要解码器
    // ─────────────────────────────────────────────────────────
    decoder = avcodec_find_decoder(codecpar->codec_id);
    if (!decoder && codecpar->codec_id != AV_CODEC_ID_RAWVIDEO) {
        std::cerr << "❌ 找不到对应解码器" << std::endl;
        goto cleanup;
    }

    dec_ctx = avcodec_alloc_context3(decoder);
    if (!dec_ctx) {
        std::cerr << "❌ 分配解码器上下文失败" << std::endl;
        goto cleanup;
    }

    if (avcodec_parameters_to_context(dec_ctx, codecpar) < 0) {
        std::cerr << "❌ 解码器参数填充失败" << std::endl;
        goto cleanup;
    }

    need_decode = (codecpar->codec_id != AV_CODEC_ID_RAWVIDEO);
    if (need_decode) {
        if (avcodec_open2(dec_ctx, decoder, nullptr) < 0) {
            std::cerr << "❌ 解码器打开失败" << std::endl;
            goto cleanup;
        }
        std::cout << "✅ 已启用解码器: " << decoder->name << std::endl;
    }

    // ─────────────────────────────────────────────────────────
    // 3️⃣  sws_scale 转换上下文：任意格式 → YUV420P
    // ─────────────────────────────────────────────────────────
    sws_ctx = sws_getContext(
        width, height, need_decode ? dec_ctx->pix_fmt : src_pix_fmt,
        width, height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    if (!sws_ctx) {
        std::cerr << "❌ sws_getContext 失败" << std::endl;
        goto cleanup;
    }

    // 申请 YUV 输出帧的内存（plane[0]=Y, plane[1]=U, plane[2]=V）
    if (av_image_alloc(yuv_data, yuv_linesize, width, height, AV_PIX_FMT_YUV420P, 32) < 0) {
        std::cerr << "❌ YUV 缓冲区分配失败" << std::endl;
        goto cleanup;
    }
    yuv_allocated = true;

    // ─────────────────────────────────────────────────────────
    // 4️⃣  x264 编码器初始化
    // ─────────────────────────────────────────────────────────
    x264_param_t param;
    x264_param_default_preset(&param, "ultrafast", "zerolatency");
    param.i_threads    = 0;     // 自动选线程数
    param.i_width      = width;
    param.i_height     = height;
    param.i_fps_num    = fps;
    param.i_fps_den    = 1;
    param.i_keyint_max = 60;
    param.b_intra_refresh = 0;
    param.rc.i_rc_method  = X264_RC_CRF;
    param.rc.f_rf_constant = 20.0f;
    x264_param_apply_profile(&param, "main");

    encoder = x264_encoder_open(&param);
    if (!encoder) {
        std::cerr << "❌ x264 编码器初始化失败" << std::endl;
        goto cleanup;
    }
    std::cout << "✅ x264 编码器初始化成功" << std::endl;

    x264_picture_alloc(&pic_in, X264_CSP_I420, width, height);
    pic_allocated = true;

    // ─────────────────────────────────────────────────────────
    // 5️⃣  主采集循环
    // ─────────────────────────────────────────────────────────
    pkt = av_packet_alloc();
    raw_frame = av_frame_alloc(); // 解码后的原始帧
    if (!pkt || !raw_frame) {
        std::cerr << "❌ 采集缓存申请失败" << std::endl;
        goto cleanup;
    }

    // 初始化时间戳相关变量
    pts_counter = 0;
    pts_step = 90000 / static_cast<uint32_t>(fps);
    consecutive_read_failures = 0;

    std::cout << "🎬 开始采集循环..." << std::endl;

    while (running_flag && *running_flag) {

        // 从摄像头读取一个数据包
        if (av_read_frame(fmt_ctx, pkt) < 0) {
            consecutive_read_failures++;
            av_packet_unref(pkt);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (consecutive_read_failures > 100) {
                std::cerr << "❌ 摄像头读取连续失败超过阈值，停止推流" << std::endl;
                if (running_flag) {
                    *running_flag = false;
                }
                break;
            }
            continue;
        }
        consecutive_read_failures = 0;

        if (pkt->stream_index != video_stream_idx) {
            av_packet_unref(pkt);
            continue;
        }

        // ── 格式清洗：把原始/压缩帧统一转换成 YUV420P ──
        uint8_t* src_data[4]     = {nullptr};
        int      src_linesize[4] = {0};

        if (need_decode && dec_ctx) {
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

cleanup:
    // ─────────────────────────────────────────────────────────
    // 6️⃣  清理资源
    // ─────────────────────────────────────────────────────────
    std::cout << "🛑 采集线程正在清理资源..." << std::endl;
    if (pic_allocated) {
        x264_picture_clean(&pic_in);
    }
    if (encoder) {
        x264_encoder_close(encoder);
    }
    if (raw_frame) {
        av_frame_free(&raw_frame);
    }
    if (pkt) {
        av_packet_free(&pkt);
    }
    if (yuv_allocated) {
        av_freep(&yuv_data[0]);
    }
    if (sws_ctx) {
        sws_freeContext(sws_ctx);
    }
    if (dec_ctx) {
        avcodec_free_context(&dec_ctx);
    }
    if (fmt_ctx) {
        avformat_close_input(&fmt_ctx);
    }
    std::cout << "✅ 采集线程已安全退出" << std::endl;
}

// 从本地 H264 文件读取（Annex-B，带 start-code 0x00000001），并以指定帧率推送 NALU
void CameraCapture::startCaptureFromFile(const std::string& filename, std::atomic<bool>* running_flag, NaluCallback onNalu) {
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "❌ 无法打开 H264 文件: " << filename << std::endl;
        return;
    }

    std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (buffer.empty()) {
        std::cerr << "❌ H264 文件为空: " << filename << std::endl;
        return;
    }

    std::cout << "📁 使用本地 H264 文件推流: " << filename << std::endl;

    // 预解析 Annex-B，兼容 00 00 01 与 00 00 00 01 两种 start code。
    const auto nalus = extractAnnexBNalus(buffer);
    if (nalus.empty()) {
        std::cerr << "❌ 未在文件中找到有效 Annex-B NALU: " << filename << std::endl;
        return;
    }
    std::cout << "✅ 检测到 " << nalus.size() << " 个 NALU，开始循环推流" << std::endl;

    size_t nalu_index = 0;
    uint32_t pts_counter = 0;
    const uint32_t pts_step = 90000 / 30; // 模拟 30fps
    const auto frame_interval = std::chrono::milliseconds(33);

    while (running_flag && *running_flag) {
        const auto& nalu = nalus[nalu_index];
        if (nalu.size == 0 || nalu.offset + nalu.size > buffer.size()) {
            nalu_index = (nalu_index + 1) % nalus.size();
            continue;
        }

        if (onNalu) {
            onNalu(buffer.data() + nalu.offset, static_cast<int>(nalu.size), pts_counter);
        }

        pts_counter += pts_step;
        nalu_index = (nalu_index + 1) % nalus.size();
        std::this_thread::sleep_for(frame_interval);
    }

    std::cout << "✅ H264 文件推流线程退出" << std::endl;
}

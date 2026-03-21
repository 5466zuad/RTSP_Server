#include "RtspPlayer.h"

#include <QMetaType>
#include <QString>

#include <chrono>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

RtspPlayer::RtspPlayer(QObject* parent) : QObject(parent) {
    qRegisterMetaType<QImage>("QImage");
}

RtspPlayer::~RtspPlayer() {
    stop();
}

bool RtspPlayer::isRunning() const {
    return m_running.load(std::memory_order_relaxed);
}

void RtspPlayer::start(const std::string& url) {
    stop();
    m_running = true;
    m_thread = std::thread(&RtspPlayer::run, this, url);
}

void RtspPlayer::stop() {
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void RtspPlayer::run(std::string url) {
    avformat_network_init();

    while (m_running) {
        AVFormatContext* fmt_ctx = nullptr;
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "rtsp_transport", "udp", 0);
        av_dict_set(&opts, "fflags", "nobuffer", 0);
        av_dict_set(&opts, "flags", "low_delay", 0);
        av_dict_set(&opts, "max_delay", "0", 0);
        av_dict_set(&opts, "stimeout", "5000000", 0);

        if (avformat_open_input(&fmt_ctx, url.c_str(), nullptr, &opts) < 0) {
            emit error(QString("Failed to open RTSP: %1").arg(QString::fromStdString(url)));
            av_dict_free(&opts);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        av_dict_free(&opts);

        if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
            emit error("Failed to find stream info");
            avformat_close_input(&fmt_ctx);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        int video_stream = -1;
        for (unsigned int i = 0; i < fmt_ctx->nb_streams; ++i) {
            if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                video_stream = static_cast<int>(i);
                break;
            }
        }

        if (video_stream < 0) {
            emit error("No video stream found");
            avformat_close_input(&fmt_ctx);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        AVCodecParameters* codecpar = fmt_ctx->streams[video_stream]->codecpar;
        const AVCodec* decoder = avcodec_find_decoder(codecpar->codec_id);
        if (!decoder) {
            emit error("No decoder found");
            avformat_close_input(&fmt_ctx);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        AVCodecContext* dec_ctx = avcodec_alloc_context3(decoder);
        if (!dec_ctx) {
            emit error("Failed to allocate decoder context");
            avformat_close_input(&fmt_ctx);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        if (avcodec_parameters_to_context(dec_ctx, codecpar) < 0) {
            emit error("Failed to copy decoder params");
            avcodec_free_context(&dec_ctx);
            avformat_close_input(&fmt_ctx);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        if (avcodec_open2(dec_ctx, decoder, nullptr) < 0) {
            emit error("Failed to open decoder");
            avcodec_free_context(&dec_ctx);
            avformat_close_input(&fmt_ctx);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        AVFrame* frame = av_frame_alloc();
        AVFrame* rgb_frame = av_frame_alloc();
        AVPacket* packet = av_packet_alloc();
        if (!frame || !rgb_frame || !packet) {
            emit error("Failed to allocate frame/packet");
            if (packet) av_packet_free(&packet);
            if (frame) av_frame_free(&frame);
            if (rgb_frame) av_frame_free(&rgb_frame);
            avcodec_free_context(&dec_ctx);
            avformat_close_input(&fmt_ctx);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        const int width = dec_ctx->width;
        const int height = dec_ctx->height;
        uint8_t* rgb_data[4] = {nullptr};
        int rgb_linesize[4] = {0};
        if (av_image_alloc(rgb_data, rgb_linesize, width, height, AV_PIX_FMT_RGB24, 1) < 0) {
            emit error("Failed to allocate RGB buffer");
            av_packet_free(&packet);
            av_frame_free(&frame);
            av_frame_free(&rgb_frame);
            avcodec_free_context(&dec_ctx);
            avformat_close_input(&fmt_ctx);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        SwsContext* sws_ctx = sws_getContext(
            width, height, dec_ctx->pix_fmt,
            width, height, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_ctx) {
            emit error("Failed to create swscale context");
            av_freep(&rgb_data[0]);
            av_packet_free(&packet);
            av_frame_free(&frame);
            av_frame_free(&rgb_frame);
            avcodec_free_context(&dec_ctx);
            avformat_close_input(&fmt_ctx);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        rgb_frame->data[0] = rgb_data[0];
        rgb_frame->linesize[0] = rgb_linesize[0];

        while (m_running) {
            const int read_ret = av_read_frame(fmt_ctx, packet);
            if (read_ret < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (packet->stream_index == video_stream) {
                if (avcodec_send_packet(dec_ctx, packet) >= 0) {
                    while (avcodec_receive_frame(dec_ctx, frame) >= 0) {
                        sws_scale(sws_ctx, frame->data, frame->linesize, 0, height,
                                  rgb_frame->data, rgb_frame->linesize);

                        QImage image(rgb_frame->data[0], width, height, rgb_frame->linesize[0],
                                     QImage::Format_RGB888);
                        emit frameReady(image.copy());
                    }
                }
            }

            av_packet_unref(packet);
        }

        av_freep(&rgb_data[0]);
        sws_freeContext(sws_ctx);
        av_packet_free(&packet);
        av_frame_free(&frame);
        av_frame_free(&rgb_frame);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
    }
}

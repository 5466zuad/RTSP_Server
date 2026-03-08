#include "../include/CameraCapture.h"
#include <iostream>
#include <chrono>
#include <thread>

CameraCapture::CameraCapture() 
    : source(std::make_unique<VideoSource>()),
      scaler(std::make_unique<FrameScaler>()),
      encoder(std::make_unique<H264Encoder>()) {}

CameraCapture::~CameraCapture() {}

void CameraCapture::startCaptureAndEncode(std::atomic<bool>* running_flag, NaluCallback onNalu) {
    
    // 1. 打开摄像头设备
    if (!source->openDevice("/dev/video0")) {
        return;
    }

    // 2. 初始化洗画面和编码组件
    AVCodecParameters* codecpar = source->getFormatContext()->streams[source->getVideoStreamIndex()]->codecpar;
    if (!scaler->init(codecpar)) {
        std::cerr << "❌ 画面转换器(Scaler)初始化失败" << std::endl;
        return;
    }

    if (!encoder->init(codecpar->width, codecpar->height, 30)) {
        std::cerr << "❌ x264编码器初始化失败" << std::endl;
        return;
    }

    // 3. 开始主循环
    AVPacket* pkt = av_packet_alloc();
    uint32_t pts_counter = 0;
    const uint32_t pts_step = 90000 / 30;

    std::cout << "🎬 [模块化模式] 开始采集推流..." << std::endl;

    while (running_flag && *running_flag) {
        if (source->readFrame(pkt) < 0) {
            std::cerr << "⚠️ 读取摄像头画面失败" << std::endl;
            av_packet_unref(pkt);
            break;
        }

        if (pkt->stream_index != source->getVideoStreamIndex()) {
            av_packet_unref(pkt);
            continue;
        }

        // 4. 处理画面并编码
        if (scaler->processPacket(pkt, encoder->getInImgPlanes(), encoder->getInImgStrides())) {
            encoder->encode(pts_counter, onNalu);
            pts_counter += pts_step;
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    std::cout << "🛑 采集模块已安全退出" << std::endl;
}

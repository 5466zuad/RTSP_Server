#include "../include/CameraCapture.h"
#include <iostream>
#include <opencv2/opencv.hpp>
#include <x264.h>
#include <cstring>
#include <vector>

extern std::atomic<bool> g_running;

CameraCapture::CameraCapture() {
}

CameraCapture::~CameraCapture() {
}

void CameraCapture::startCaptureAndEncode(std::atomic<bool>* running_flag, NaluCallback onNalu) {
    // 1️⃣ 通过 UDP 接收 Windows 端 FFmpeg 推流（强制 FFmpeg 后端）
    cv::VideoCapture cap;
    cap.open("udp://@:5000?overrun_nonfatal=1&fifo_size=50000000", cv::CAP_FFMPEG);
    if (!cap.isOpened()) {
        std::cout << "❌ 无法接收 UDP 推流！" << std::endl;
        return;
    }
    int w = (int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
    int h = (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    if (w <= 0) w = 640;
    if (h <= 0) h = 480;
    cap.set(cv::CAP_PROP_FPS, 30);

    // 2️⃣ 初始化 x264 编码器（速度优先 + 画质平衡）
    x264_param_t param;
    x264_param_default_preset(&param, "ultrafast", "zerolatency");
    param.i_threads = 0;
    param.i_width = w;
    param.i_height = h;
    param.i_fps_num = 30;
    param.i_fps_den = 1;
    param.i_keyint_max = 60;
    param.b_intra_refresh = 0;
    param.rc.i_rc_method = X264_RC_CRF;
    param.rc.f_rf_constant = 20;
    x264_param_apply_profile(&param, "main");

    x264_t* encoder = x264_encoder_open(&param);
    if (!encoder) {
        std::cout << "❌ x264 编码器初始化失败！" << std::endl;
        return;
    }

    x264_picture_t pic_in, pic_out;
    x264_picture_alloc(&pic_in, X264_CSP_I420, param.i_width, param.i_height);

    uint32_t timestamp = 0;
    cv::Mat frame;
    cv::Mat yuv_frame;

    // 3️⃣ 主循环抓包
    while (g_running && running_flag && *running_flag) { 
        cap >> frame; // 📸 OpenCV 抓图！
        if (frame.empty()) continue;

        // BGR 转 YUV (x264 需要 YUV420P)
        cv::cvtColor(frame, yuv_frame, cv::COLOR_BGR2YUV_I420);
        
        // 填充 x264 picture
        int y_size = param.i_width * param.i_height;
        memcpy(pic_in.img.plane[0], yuv_frame.data, y_size);
        memcpy(pic_in.img.plane[1], yuv_frame.data + y_size, y_size / 4);
        memcpy(pic_in.img.plane[2], yuv_frame.data + y_size + y_size / 4, y_size / 4);
        pic_in.i_pts = timestamp;

        x264_nal_t* nals;
        int num_nals = 0;
        int frame_size = x264_encoder_encode(encoder, &nals, &num_nals, &pic_in, &pic_out);

        if (frame_size <= 0) continue;

        // 4️⃣ 将编码出的 NALU 回调出去
        for (int i = 0; i < num_nals; ++i) {
            uint8_t* nalu_data = nals[i].p_payload;
            int nalu_size = nals[i].i_payload;
            
            if (onNalu) {
                onNalu(nalu_data, nalu_size, timestamp);
            }
        }
        
        // OpenCV 的 cap>>frame 天然会按照 30 fps 进行等待 (33ms 左右)
        // 此处的 timestamp 是按照 90000 Hz 的时钟，每帧占 3000
        timestamp += 3000; 
    }

    x264_picture_clean(&pic_in);
    x264_encoder_close(encoder);
}

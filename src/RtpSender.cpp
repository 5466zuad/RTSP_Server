#include "../include/RtpSender.h"
#include <iostream>
#include <sys/socket.h> 
#include <netinet/in.h> 
#include <arpa/inet.h>  
#include <unistd.h>     
#include <cstring>      
#include <vector>       
#include <fstream>      
#include <atomic> 
#include <csignal> 
#include <opencv2/opencv.hpp>
#include <x264.h>
#include "../include/CameraCapture.h" // 使用新的采集模块

// 引用全局信号 (作为双重保险)
extern std::atomic<bool> g_running;

// 🔥 构造函数：全副武装
RtpSender::RtpSender(std::string ip, int port, int local_port, std::atomic<bool>* flag) 
    : dest_ip(ip), dest_port(port), local_port(local_port), running_flag(flag) 
{
    udp_socket = socket(AF_INET, SOCK_DGRAM, 0); 
    if (udp_socket < 0) {
        perror("❌ [RtpSender] socket 创建失败");
        return;
    }
    
    // 1. 缓冲区扩容 (防花屏神器)
    int send_buf_size = 4 * 1024 * 1024; // 4MB
    setsockopt(udp_socket, SOL_SOCKET, SO_SNDBUF, &send_buf_size, sizeof(send_buf_size));

    // 2. 绑定指定的本地端口 (解决端口冲突)
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr)); 
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;    
    local_addr.sin_port = htons(local_port); 

    if (bind(udp_socket, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
        perror("⚠️ [RtpSender] 警告：UDP 绑定失败！端口可能被占用");
        close(udp_socket);
        udp_socket = -1; 
    } else {
        std::cout << "🔒 [Port:" << local_port << "] 绑定本地端口成功" << std::endl;
    }
}

RtpSender::~RtpSender() {
    if (udp_socket >= 0) {
        close(udp_socket); 
        std::cout << "🔓 [Port:" << local_port << "] UDP Socket 已释放" << std::endl;
    }
}

void RtpSender::sendVideo(const std::string& filename) {
    if (udp_socket < 0) {
        std::cout << "❌ [Port:" << local_port << "] Socket 未就绪，无法推流" << std::endl;
        return;
    }

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_addr.s_addr = inet_addr(dest_ip.c_str()); 
    dest_addr.sin_port = htons(dest_port); 

    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cout << "❌ 找不到视频文件：" << filename << std::endl;
        return;
    }
    std::streamsize size = file.tellg(); 
    file.seekg(0, std::ios::beg);         

    std::vector<uint8_t> buffer(size);
    if (!file.read((char*)buffer.data(), size)) return;

    std::cout << "🎬 [Port:" << local_port << "] 开始推流 → " << dest_ip << ":" << dest_port << std::endl;

    uint16_t seq = 0;
    uint32_t timestamp = 0;
    int accumulated_bytes = 0; // 🌟 累计发送字节数，避免高频上报卡死 UI
    
    while (g_running && running_flag && *running_flag) { 
        int pos = 0;
        int start_code_len = 0;

        while (pos < size - 3) {
            if (buffer[pos] == 0 && buffer[pos+1] == 0 && buffer[pos+2] == 0 && buffer[pos+3] == 1) {
                start_code_len = 4; break;
            }
            if (buffer[pos] == 0 && buffer[pos+1] == 0 && buffer[pos+2] == 1) {
                start_code_len = 3; break;
            }
            pos++;
        }

        while (pos < size) {
            if (!g_running || (running_flag && !(*running_flag))) {
                std::cout << "⏸️  [Port:" << local_port << "] 收到停止信号，推流中断" << std::endl;
                goto cleanup; 
            }

            int next_pos = pos + start_code_len;
            int next_start_code_len = 0;
            
            while (next_pos < size) {
                if (next_pos <= size - 4 && buffer[next_pos] == 0 && buffer[next_pos+1] == 0 && buffer[next_pos+2] == 0 && buffer[next_pos+3] == 1) {
                    next_start_code_len = 4; break;
                }
                if (next_pos <= size - 3 && buffer[next_pos] == 0 && buffer[next_pos+1] == 0 && buffer[next_pos+2] == 1) {
                    next_start_code_len = 3; break;
                }
                next_pos++;
            }

            int nalu_size = next_pos - pos - start_code_len;
            if (nalu_size > 0) {
                uint8_t* nalu_data = buffer.data() + pos + start_code_len;
                uint8_t nalu_type = nalu_data[0] & 0x1F;

                if (nalu_size <= 1400) {
                    // 🟢 场景 1：单包发送
                    uint8_t rtp_packet[1500];
                    rtp_packet[0] = 0x80; rtp_packet[1] = 0xE0; 
                    rtp_packet[2] = seq >> 8; rtp_packet[3] = seq & 0xFF;
                    rtp_packet[4] = (timestamp >> 24) & 0xFF; rtp_packet[5] = (timestamp >> 16) & 0xFF;
                    rtp_packet[6] = (timestamp >> 8) & 0xFF; rtp_packet[7] = timestamp & 0xFF;
                    rtp_packet[8] = 0x88; rtp_packet[9] = 0x88; rtp_packet[10] = 0x88; rtp_packet[11] = 0x88;
                    memcpy(rtp_packet + 12, nalu_data, nalu_size);
                    
                    // ✦ 核心逻辑：获取发出的字节，通过小探头送给老板娘！
                    int bytes_sent = sendto(udp_socket, rtp_packet, nalu_size + 12, 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
                    if (bytes_sent > 0 && onTraffic) {
                        accumulated_bytes += bytes_sent;
                        if (accumulated_bytes >= 65536) { // 🌟 攒够 64KB 上报一次
                            onTraffic(accumulated_bytes);
                            accumulated_bytes = 0;
                        }
                    }
                    
                    seq++;
                } else {
                    // 🟢 场景 2：FU-A 切片发送
                    int payload_size = nalu_size - 1; 
                    int nalu_payload_pos = 1;
                    uint8_t fu_indicator = (nalu_data[0] & 0xE0) | 28; 
                    bool is_first = true;

                    while (payload_size > 0) {
                        int chunk_size = (payload_size > 1400) ? 1400 : payload_size;
                        bool is_last = (chunk_size == payload_size);
                        uint8_t fu_header = nalu_type;
                        if (is_first) fu_header |= 0x80; 
                        if (is_last)  fu_header |= 0x40; 
                        
                        uint8_t rtp_packet[1500];
                        rtp_packet[0] = 0x80; 
                        rtp_packet[1] = (is_last ? 0xE0 : 0x60); 
                        rtp_packet[2] = seq >> 8; rtp_packet[3] = seq & 0xFF;
                        rtp_packet[4] = (timestamp >> 24) & 0xFF; rtp_packet[5] = (timestamp >> 16) & 0xFF;
                        rtp_packet[6] = (timestamp >> 8) & 0xFF; rtp_packet[7] = timestamp & 0xFF;
                        rtp_packet[8] = 0x88; rtp_packet[9] = 0x88; rtp_packet[10] = 0x88; rtp_packet[11] = 0x88;
                        rtp_packet[12] = fu_indicator;
                        rtp_packet[13] = fu_header;

                        memcpy(rtp_packet + 14, nalu_data + nalu_payload_pos, chunk_size);
                        
                        // ✦ 核心逻辑：切片包同样要抓取字节数上报！
                        int bytes_sent = sendto(udp_socket, rtp_packet, chunk_size + 14, 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
                        if (bytes_sent > 0 && onTraffic) {
                            accumulated_bytes += bytes_sent;
                            if (accumulated_bytes >= 65536) { // 🌟 攒够 64KB 上报一次
                                onTraffic(accumulated_bytes);
                                accumulated_bytes = 0;
                            }
                        }
                        
                        usleep(10); 

                        seq++;
                        payload_size -= chunk_size;
                        nalu_payload_pos += chunk_size;
                        is_first = false;
                    }
                }

                if (nalu_type >= 1 && nalu_type <= 5) {
                    timestamp += 3600; 
                    usleep(40000); 
                } else {
                    usleep(1000); 
                }
            }
            pos = next_pos;
            start_code_len = next_start_code_len;
            if (start_code_len == 0) break; 
        }
    }

cleanup:
    if (accumulated_bytes > 0 && onTraffic) {
        onTraffic(accumulated_bytes); // 🌟 发出剩余残余字节
    }
    std::cout << "🏁 [End] 推流任务结束" << std::endl;
    return;
}

void RtpSender::sendLiveCamera() {
    if (udp_socket < 0) {
        std::cout << "❌ [Port:" << local_port << "] Socket 未就绪，无法推流" << std::endl;
        return;
    }

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_addr.s_addr = inet_addr(dest_ip.c_str()); 
    dest_addr.sin_port = htons(dest_port); 

    // 1️⃣ 打开摄像头
    cv::VideoCapture cap(0);
    if (!cap.isOpened()) {
        std::cout << "❌ 找不到摄像头或摄像头被占用！" << std::endl;
        return;
    }
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
    cap.set(cv::CAP_PROP_FPS, 30);

    // 2️⃣ 初始化 x264 编码器
    x264_param_t param;
    x264_param_default_preset(&param, "ultrafast", "zerolatency"); // 零延迟直播预设
    param.i_threads = 1; 
    param.i_width = 640;
    param.i_height = 480;
    param.i_fps_num = 30;
    param.i_fps_den = 1;
    param.i_keyint_max = 30; // 关键帧间隔：1秒1个I帧
    param.b_intra_refresh = 1; 
    param.rc.i_rc_method = X264_RC_CRF;
    param.rc.f_rf_constant = 25; // 画质配置
    x264_param_apply_profile(&param, "baseline");

    x264_t* encoder = x264_encoder_open(&param);
    if (!encoder) {
        std::cout << "❌ x264 编码器初始化失败！" << std::endl;
        return;
    }

    x264_picture_t pic_in, pic_out;
    x264_picture_alloc(&pic_in, X264_CSP_I420, param.i_width, param.i_height);

    std::cout << "🎬 [Port:" << local_port << "] 摄像头实时推流开始 → " << dest_ip << ":" << dest_port << std::endl;

    uint16_t seq = 0;
    uint32_t timestamp = 0;
    int accumulated_bytes = 0; 
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

        // 4️⃣ RTP 发包流程 (和文件发送一致)
        for (int i = 0; i < num_nals; ++i) {
            uint8_t* nalu_data = nals[i].p_payload;
            int nalu_size = nals[i].i_payload;

            // 跳过 start code
            int start_code_len = 4;
            if (nalu_data[0] == 0 && nalu_data[1] == 0 && nalu_data[2] == 1) {
                start_code_len = 3;
            }
            nalu_data += start_code_len;
            nalu_size -= start_code_len;

            uint8_t nalu_type = nalu_data[0] & 0x1F;

            if (nalu_size <= 1400) {
                // 单包发送
                uint8_t rtp_packet[1500];
                rtp_packet[0] = 0x80; rtp_packet[1] = 0xE0; // Marker 固定为 1 (虽然不精确，但 VLC 兼容)
                rtp_packet[2] = seq >> 8; rtp_packet[3] = seq & 0xFF;
                rtp_packet[4] = (timestamp >> 24) & 0xFF; rtp_packet[5] = (timestamp >> 16) & 0xFF;
                rtp_packet[6] = (timestamp >> 8) & 0xFF; rtp_packet[7] = timestamp & 0xFF;
                rtp_packet[8] = 0x88; rtp_packet[9] = 0x88; rtp_packet[10] = 0x88; rtp_packet[11] = 0x88;
                memcpy(rtp_packet + 12, nalu_data, nalu_size);
                
                int bytes_sent = sendto(udp_socket, rtp_packet, nalu_size + 12, 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
                if (bytes_sent > 0 && onTraffic) {
                    accumulated_bytes += bytes_sent;
                    if (accumulated_bytes >= 65536) { 
                        onTraffic(accumulated_bytes); accumulated_bytes = 0;
                    }
                }
                seq++;
            } else {
                // FU-A 切片发送
                int payload_size = nalu_size - 1; 
                int nalu_payload_pos = 1;
                uint8_t fu_indicator = (nalu_data[0] & 0xE0) | 28; 
                bool is_first = true;

                while (payload_size > 0) {
                    int chunk_size = (payload_size > 1400) ? 1400 : payload_size;
                    bool is_last = (chunk_size == payload_size);
                    uint8_t fu_header = nalu_type;
                    if (is_first) fu_header |= 0x80; 
                    if (is_last)  fu_header |= 0x40; 
                    
                    uint8_t rtp_packet[1500];
                    rtp_packet[0] = 0x80; 
                    rtp_packet[1] = (is_last ? 0xE0 : 0x60); 
                    rtp_packet[2] = seq >> 8; rtp_packet[3] = seq & 0xFF;
                    rtp_packet[4] = (timestamp >> 24) & 0xFF; rtp_packet[5] = (timestamp >> 16) & 0xFF;
                    rtp_packet[6] = (timestamp >> 8) & 0xFF; rtp_packet[7] = timestamp & 0xFF;
                    rtp_packet[8] = 0x88; rtp_packet[9] = 0x88; rtp_packet[10] = 0x88; rtp_packet[11] = 0x88;
                    rtp_packet[12] = fu_indicator;
                    rtp_packet[13] = fu_header;

                    memcpy(rtp_packet + 14, nalu_data + nalu_payload_pos, chunk_size);
                    
                    int bytes_sent = sendto(udp_socket, rtp_packet, chunk_size + 14, 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
                    if (bytes_sent > 0 && onTraffic) {
                        accumulated_bytes += bytes_sent;
                        if (accumulated_bytes >= 65536) { 
                            onTraffic(accumulated_bytes); accumulated_bytes = 0;
                        }
                    }
                    
                    // 直播时不 usleep 切片间隔，尽量发送
                    seq++;
                    payload_size -= chunk_size;
                    nalu_payload_pos += chunk_size;
                    is_first = false;
                }
            }
        }
        
        // OpenCV 的 cap>>frame 天然会按照 30 fps 进行等待 (33ms 左右)
        // 此处的 timestamp 是按照 90000 Hz 的时钟，每帧占 3000
        timestamp += 3000; 
    }

    if (accumulated_bytes > 0 && onTraffic) {
        onTraffic(accumulated_bytes); 
    }
    x264_picture_clean(&pic_in);
    x264_encoder_close(encoder);
    std::cout << "🏁 [End] 摄像头实时推流任务结束" << std::endl;
}
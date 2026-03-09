#include "RtpSender.h"
#include <iostream>
#include <sys/socket.h> 
#include <netinet/in.h> 
#include <arpa/inet.h>  
#include <unistd.h>     
#include <cstring>      
#include <random>

RtpSender::RtpSender(std::string ip, int port, int local_port, std::atomic<bool>* flag) 
    : dest_ip(ip), dest_port(port), local_port(local_port), running_flag(flag), seq(0), timestamp(0), accumulated_bytes(0) {

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint16_t> seq_dis(0, 0xFFFF);
    std::uniform_int_distribution<uint32_t> ssrc_dis(1, 0xFFFFFFFF);
    seq = seq_dis(gen);
    ssrc = ssrc_dis(gen);
    
    udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket < 0) {
        std::cerr << "❌ [Port:" << local_port << "] 创建 UDP Socket 失败" << std::endl;
        return;
    }

    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(local_port);

    if (bind(udp_socket, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
        std::cerr << "❌ [Port:" << local_port << "] 绑定本地端口失败！端口可能被占用" << std::endl;
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

void RtpSender::sendNalu(uint8_t* nalu_data, int nalu_size, uint32_t ts, bool is_last_nalu) {
    if (!running_flag || !(*running_flag)) return;

    // 采集线程传入的是每帧的绝对 90kHz 时间戳（pts），这里直接使用该时间戳
    this->timestamp = ts;

    uint8_t rtp_packet[1500];
    
    rtp_packet[0] = 0x80; // V=2, P=0, X=0, CC=0
    rtp_packet[1] = 96;   // M=0 initially, PT=96
    
    rtp_packet[2] = (this->seq >> 8) & 0xFF;
    rtp_packet[3] = this->seq & 0xFF;
    
    rtp_packet[4] = (this->timestamp >> 24) & 0xFF;
    rtp_packet[5] = (this->timestamp >> 16) & 0xFF;
    rtp_packet[6] = (this->timestamp >> 8) & 0xFF;
    rtp_packet[7] = this->timestamp & 0xFF;
    
    rtp_packet[8] = (ssrc >> 24) & 0xFF;
    rtp_packet[9] = (ssrc >> 16) & 0xFF;
    rtp_packet[10] = (ssrc >> 8) & 0xFF;
    rtp_packet[11] = ssrc & 0xFF;

    uint8_t nal_header = nalu_data[0];
    
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_addr.s_addr = inet_addr(dest_ip.c_str()); 
    dest_addr.sin_port = htons(dest_port); 

    if (nalu_size <= 1400) {
        if (is_last_nalu) rtp_packet[1] |= 0x80; // Mark bit = 1
        memcpy(rtp_packet + 12, nalu_data, nalu_size);
        int sent = sendto(udp_socket, rtp_packet, 12 + nalu_size, 0, 
                          (struct sockaddr*)&dest_addr, sizeof(dest_addr));
        if (sent > 0) this->accumulated_bytes += sent;
        if (sent < 0) failed_packets.fetch_add(1, std::memory_order_relaxed);
        this->seq++;
    } else {
        uint8_t fu_indicator = (nal_header & 0xE0) | 28;
        uint8_t fu_header = nal_header & 0x1F;
        
        int offset = 1; 
        int remaining = nalu_size - 1;
        bool is_first = true;
        
        while (remaining > 0) {
            int chunk_size = (remaining > 1400) ? 1400 : remaining;
            bool is_last_chunk = (remaining == chunk_size);
            
            rtp_packet[1] = 96;
            if (is_last_chunk && is_last_nalu) rtp_packet[1] |= 0x80; 
            
            rtp_packet[2] = (this->seq >> 8) & 0xFF;
            rtp_packet[3] = this->seq & 0xFF;
            
            rtp_packet[12] = fu_indicator;
            rtp_packet[13] = fu_header;
            
            if (is_first) {
                rtp_packet[13] |= 0x80; 
                is_first = false;
            } else if (is_last_chunk) {
                rtp_packet[13] |= 0x40; 
            } else {
                rtp_packet[13] &= ~0xC0; 
            }
            
            memcpy(rtp_packet + 14, nalu_data + offset, chunk_size);
            int sent = sendto(udp_socket, rtp_packet, 14 + chunk_size, 0, 
                              (struct sockaddr*)&dest_addr, sizeof(dest_addr));
            if (sent > 0) this->accumulated_bytes += sent;
            if (sent < 0) failed_packets.fetch_add(1, std::memory_order_relaxed);
            
            this->seq++;
            offset += chunk_size;
            remaining -= chunk_size;
        }
    }

    if (this->accumulated_bytes > 10 * 1024) {
        if (onTraffic) {
            onTraffic(this->accumulated_bytes);
        }
        this->accumulated_bytes = 0;
    }
}

uint32_t RtpSender::getFailedPackets() const {
    return failed_packets.load(std::memory_order_relaxed);
}

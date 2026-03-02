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

RtpSender::RtpSender(std::string ip, int port, int local_port, std::atomic<bool>* flag) 
    : dest_ip(ip), dest_port(port), local_port(local_port), running_flag(flag), seq(0), timestamp(0), accumulated_bytes(0) {
    
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

    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "❌ 无法打开视频文件: " << filename << std::endl;
        return;
    }

    std::cout << "🎬 [Port:" << local_port << "] 本地文件推流开始 → " << dest_ip << ":" << dest_port << std::endl;

    uint8_t rtp_packet[1500];
    int rtp_header_size = 12;

    rtp_packet[0] = 0x80;
    rtp_packet[1] = 96; 

    rtp_packet[8] = 0x12; rtp_packet[9] = 0x34; rtp_packet[10] = 0x56; rtp_packet[11] = 0x78;

    std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    size_t pos = 0;

    int accumulated_bytes = 0; 

    while (pos < buffer.size() && running_flag && *running_flag) {
        size_t start_code_pos = pos;
        while (start_code_pos < buffer.size() - 4) {
            if (buffer[start_code_pos] == 0 && buffer[start_code_pos+1] == 0 && 
                buffer[start_code_pos+2] == 0 && buffer[start_code_pos+3] == 1) {
                break;
            }
            start_code_pos++;
        }

        if (start_code_pos >= buffer.size() - 4) break;

        size_t next_start_code_pos = start_code_pos + 4;
        while (next_start_code_pos < buffer.size() - 4) {
            if (buffer[next_start_code_pos] == 0 && buffer[next_start_code_pos+1] == 0 && 
                buffer[next_start_code_pos+2] == 0 && buffer[next_start_code_pos+3] == 1) {
                break;
            }
            next_start_code_pos++;
        }

        size_t nalu_size = next_start_code_pos - (start_code_pos + 4);
        uint8_t* nalu_data = &buffer[start_code_pos + 4];

        uint8_t nalu_type = nalu_data[0] & 0x1F;

        if (nalu_size <= 1400) {
            rtp_packet[1] |= 0x80;
            rtp_packet[2] = seq >> 8; rtp_packet[3] = seq & 0xFF;
            rtp_packet[4] = (timestamp >> 24) & 0xFF; rtp_packet[5] = (timestamp >> 16) & 0xFF;
            rtp_packet[6] = (timestamp >> 8) & 0xFF; rtp_packet[7] = timestamp & 0xFF;

            memcpy(rtp_packet + 12, nalu_data, nalu_size);
            
            int bytes_sent = sendto(udp_socket, rtp_packet, nalu_size + 12, 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
            if (bytes_sent > 0 && onTraffic) {
                accumulated_bytes += bytes_sent;
                if (accumulated_bytes >= 65536) { 
                    onTraffic(accumulated_bytes);
                    accumulated_bytes = 0;
                }
            }
            seq++;
        } else {
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
                
                rtp_packet[1] = (is_last ? 0xE0 : 0x60); 
                rtp_packet[2] = seq >> 8; rtp_packet[3] = seq & 0xFF;
                rtp_packet[4] = (timestamp >> 24) & 0xFF; rtp_packet[5] = (timestamp >> 16) & 0xFF;
                rtp_packet[6] = (timestamp >> 8) & 0xFF; rtp_packet[7] = timestamp & 0xFF;
                rtp_packet[12] = fu_indicator;
                rtp_packet[13] = fu_header;

                memcpy(rtp_packet + 14, nalu_data + nalu_payload_pos, chunk_size);
                
                int bytes_sent = sendto(udp_socket, rtp_packet, chunk_size + 14, 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
                if (bytes_sent > 0 && onTraffic) {
                    accumulated_bytes += bytes_sent;
                    if (accumulated_bytes >= 65536) { 
                        onTraffic(accumulated_bytes);
                        accumulated_bytes = 0;
                    }
                }
                
                usleep(500);
                seq++;
                payload_size -= chunk_size;
                nalu_payload_pos += chunk_size;
                is_first = false;
            }
        }

        timestamp += 3000;
        pos = next_start_code_pos;
        usleep(33000); 
    }
}

void RtpSender::sendNalu(uint8_t* nalu_data, int nalu_size, uint32_t ts_increment, bool is_last_nalu) {
    if (!running_flag || !(*running_flag)) return;

    this->timestamp += ts_increment;

    uint8_t rtp_packet[1500];
    int rtp_header_size = 12;
    
    rtp_packet[0] = 0x80; // V=2, P=0, X=0, CC=0
    rtp_packet[1] = 96;   // M=0 initially, PT=96
    
    rtp_packet[2] = (this->seq >> 8) & 0xFF;
    rtp_packet[3] = this->seq & 0xFF;
    
    rtp_packet[4] = (this->timestamp >> 24) & 0xFF;
    rtp_packet[5] = (this->timestamp >> 16) & 0xFF;
    rtp_packet[6] = (this->timestamp >> 8) & 0xFF;
    rtp_packet[7] = this->timestamp & 0xFF;
    
    rtp_packet[8] = 0x12;
    rtp_packet[9] = 0x34;
    rtp_packet[10] = 0x56;
    rtp_packet[11] = 0x78;

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
            
            this->seq++;
            offset += chunk_size;
            remaining -= chunk_size;
        }
    }

    if (this->accumulated_bytes > 100 * 1024) {
        if (onTraffic) {
            onTraffic(this->accumulated_bytes);
        }
        this->accumulated_bytes = 0;
    }
}

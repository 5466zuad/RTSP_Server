#ifndef RTP_SENDER_H
#define RTP_SENDER_H

#include <string>
#include <cstdint>
#include <atomic>
#include <functional> // 👈 新增：为了使用现代 C++ 的回调函数魔法

class RtpSender {
private:
    int udp_socket;         
    std::string dest_ip;    
    int dest_port;          
    int local_port;         
    
    // 状态维护
    uint16_t seq = 0;
    uint32_t ssrc = 0;
    uint32_t timestamp = 0;
    int accumulated_bytes = 0;
    std::atomic<uint32_t> failed_packets{0};
    
    // 🛑 个人专属遥控器
    std::atomic<bool>* running_flag;

public:
    // ✨ 新增：流量汇报口。只要有数据发出去，就通过它喊一声
    std::function<void(int)> onTraffic;

    RtpSender(std::string ip, int port, int local_port, std::atomic<bool>* flag);
    ~RtpSender();
    
    // ✨ 将 NALU 打包为 RTP 发送出去（不涉及任何采集和编码）
    // ts: 90kHz 时钟的绝对时间戳（pts）
    void sendNalu(uint8_t* nalu_data, int nalu_size, uint32_t ts, bool is_last_nalu = true);
    uint32_t getFailedPackets() const;
};

#endif
#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <map>
#include <mutex>
#include <string>
#include <atomic>
#include <memory>
#include <functional>
#include <vector>
#include "RtpSender.h"

enum class SessionState {
    INIT,
    READY,
    PLAYING,
    PAUSED,
    CLOSED
};

struct ClientContext {
    std::string ip;
    int client_port = 0;
    int server_rtp_port = 0;
    int cseq = 0;
    std::string session_id;
    std::shared_ptr<std::atomic<bool>> is_playing;
    std::shared_ptr<RtpSender> rtp_sender;
    bool need_headers = true; // 新连接需要等到 IDR 帧才开始推
    SessionState state = SessionState::INIT;
    int server_rtcp_port = 0;

    ClientContext() {
        is_playing = std::make_shared<std::atomic<bool>>(false);
        need_headers = true;
    }
};

class TcpServer {
public:
    explicit TcpServer(int rtp_port_base = 6970, int rtp_pair_count = 256);
    bool start(int port);

    // 分发一帧 NALU 给所有正在播放的客户端（由采集线程回调触发）
    void dispatchNalu(uint8_t* data, int size, uint32_t timestamp);

    void stopAllStreams();
    void resumeAllStreams();
    int onlineClients() const;
    uint64_t totalTrafficBytes() const;

private:
    int allocateServerRtpPort();
    void releaseServerRtpPort(int port);

    std::map<int, std::shared_ptr<ClientContext>> m_clients;
    mutable std::mutex m_clientsMutex;
    std::atomic<uint64_t> m_totalTrafficBytes{0};
    mutable std::mutex m_portPoolMutex;
    std::vector<int> m_freeRtpPorts;
};

#endif
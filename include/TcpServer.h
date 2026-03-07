#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <map>
#include <mutex>
#include <string>
#include <atomic>
#include <memory>
#include <functional>
#include "ThreadPool.h"
#include "RtpSender.h"

struct ClientContext {
    std::string ip;
    int client_port = 0;
    int server_rtp_port = 0;
    int cseq = 0;
    std::string session_id;
    std::shared_ptr<std::atomic<bool>> is_playing;
    std::shared_ptr<RtpSender> rtp_sender;
    bool need_headers = true; // 新连接需要等到 IDR 帧才开始推

    ClientContext() {
        is_playing = std::make_shared<std::atomic<bool>>(false);
        need_headers = true;
    }
};

class TcpServer {
public:
    TcpServer() : pool(8) {}  // 线程池 8 个工作线程
    bool start(int port);

    // 分发一帧 NALU 给所有正在播放的客户端（由采集线程回调触发）
    void dispatchNalu(uint8_t* data, int size, uint32_t timestamp);

    void stopAllStreams();
    void resumeAllStreams();

private:
    ThreadPool pool;
    std::map<int, std::shared_ptr<ClientContext>> m_clients;
    std::mutex m_clientsMutex;
};

#endif
#include "../include/TcpServer.h"
#include <chrono>
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <thread>
#include <cstdio>
#include <vector>
#include <sstream>
#include <sys/epoll.h>
#include <fcntl.h>
#include <atomic>
#include <csignal>
#include <random>

extern std::atomic<bool> g_running;
extern std::atomic<int>  g_server_port_allocator;

// ─── 辅助函数 ───────────────────────────────────────────────
std::string generateSessionId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(10000000, 99999999);
    return std::to_string(dis(gen));
}

void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

std::string getLocalIP() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return "127.0.0.1";
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("8.8.8.8");
    addr.sin_port = htons(53);
    connect(sock, (const struct sockaddr*)&addr, sizeof(addr));
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    getsockname(sock, (struct sockaddr*)&local, &len);
    std::string ip = inet_ntoa(local.sin_addr);
    close(sock);
    return ip;
}

// ─── dispatchNalu：采集线程回调到这里，异步分发给所有客户端 ───
void TcpServer::dispatchNalu(uint8_t* data, int size, uint32_t timestamp) {
    if (size <= 0 || !data) return;

    uint8_t nal_type = data[0] & 0x1F;
    bool is_idr = (nal_type == 5 || nal_type == 7 || nal_type == 8);

    // 为了跨线程安全，先把这帧数据拷贝一份
    std::vector<uint8_t> nalu_copy(data, data + size);

    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto& pair : m_clients) {
        auto ctx = pair.second;
        if (!ctx || !*(ctx->is_playing) || !ctx->rtp_sender) continue;

        // 新观众必须等到关键帧才开始推，避免黑屏
        if (ctx->need_headers) {
            if (!is_idr) continue;
            ctx->need_headers = false;
        }

        // 把 send 动作丢进线程池，编码线程立刻返回，不等待网络 I/O
        auto sender  = ctx->rtp_sender;
        auto ts      = timestamp;
        auto is_last = (nal_type != 7 && nal_type != 8); // SPS/PPS 后面还有更多 NALU

        pool.enqueue([sender, nalu_copy, ts, is_last]() mutable {
            sender->sendNalu(const_cast<uint8_t*>(nalu_copy.data()),
                             (int)nalu_copy.size(), ts, is_last);
        });
    }
}

// ─── 核心服务器启动逻辑（纯 epoll，无 Qt）───────────────────
bool TcpServer::start(int port) {
    std::cout << "🎬 Epoll 服务器启动中..." << std::endl;
    std::string my_ip = getLocalIP();
    std::cout << "🌐 本机 IP: " << my_ip << std::endl;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return false;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed"); close(server_fd); return false;
    }
    if (listen(server_fd, 100) < 0) {
        perror("Listen failed"); close(server_fd); return false;
    }

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) { perror("Epoll create failed"); return false; }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    const int MAX_EVENTS = 20;
    struct epoll_event events[MAX_EVENTS];

    std::cout << "👂 等待 RTSP 客户端连接 (Port: " << port << ")..." << std::endl;

    while (g_running) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 200);
        if (n == -1) {
            if (errno == EINTR) continue;
            perror("Epoll wait error"); break;
        }
        if (n == 0) continue;

        for (int i = 0; i < n; i++) {
            int curr_fd = events[i].data.fd;

            // ── 新客户端连接 ──
            if (curr_fd == server_fd) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
                if (client_fd < 0) continue;
                setNonBlocking(client_fd);

                ev.events  = EPOLLIN;
                ev.data.fd = client_fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);

                std::string ip = inet_ntoa(client_addr.sin_addr);
                {
                    std::lock_guard<std::mutex> lock(m_clientsMutex);
                    m_clients[client_fd] = std::make_shared<ClientContext>();
                    m_clients[client_fd]->ip = ip;
                }
                std::cout << "🎉 新连接: " << ip << " (FD=" << client_fd << ")" << std::endl;
            }
            // ── 已有客户端发来 RTSP 指令 ──
            else {
                char buffer[2048] = {0};
                int bytes_read = recv(curr_fd, buffer, sizeof(buffer) - 1, 0);

                // 客户端断开
                if (bytes_read <= 0) {
                    std::shared_ptr<ClientContext> dead;
                    {
                        std::lock_guard<std::mutex> lock(m_clientsMutex);
                        auto it = m_clients.find(curr_fd);
                        if (it != m_clients.end()) {
                            dead = it->second;
                            m_clients.erase(it);
                        }
                    }
                    if (dead) {
                        if (dead->is_playing) *(dead->is_playing) = false;
                        dead->rtp_sender.reset();
                        std::cout << "👋 客户端断开 (Session: " << dead->session_id
                                  << " FD=" << curr_fd << ")" << std::endl;
                    }
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, curr_fd, nullptr);
                    close(curr_fd);
                    continue;
                }

                // 解析 RTSP 方法
                std::shared_ptr<ClientContext> ctx;
                {
                    std::lock_guard<std::mutex> lock(m_clientsMutex);
                    if (!m_clients[curr_fd])
                        m_clients[curr_fd] = std::make_shared<ClientContext>();
                    ctx = m_clients[curr_fd];
                }

                char method[16] = {}, url[128] = {}, version[16] = {};
                sscanf(buffer, "%15s %127s %15s", method, url, version);

                const char* cseq_ptr = strstr(buffer, "CSeq: ");
                if (cseq_ptr) sscanf(cseq_ptr, "CSeq: %d", &ctx->cseq);

                char response[2048] = {};

                // OPTIONS
                if (strcmp(method, "OPTIONS") == 0) {
                    snprintf(response, sizeof(response),
                        "RTSP/1.0 200 OK\r\nCSeq: %d\r\n"
                        "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN\r\n\r\n",
                        ctx->cseq);
                    send(curr_fd, response, strlen(response), 0);
                }
                // DESCRIBE
                else if (strcmp(method, "DESCRIBE") == 0) {
                    std::stringstream sdp;
                    sdp << "v=0\r\n"
                        << "o=- 0 0 IN IP4 " << my_ip << "\r\n"
                        << "s=EdgeGateway Live\r\n"
                        << "c=IN IP4 " << my_ip << "\r\n"
                        << "t=0 0\r\n"
                        << "m=video 0 RTP/AVP 96\r\n"
                        << "a=rtpmap:96 H264/90000\r\n"
                        << "a=fmtp:96 packetization-mode=1\r\n"
                        << "a=control:track0\r\n";
                    std::string sdp_str = sdp.str();
                    snprintf(response, sizeof(response),
                        "RTSP/1.0 200 OK\r\nCSeq: %d\r\n"
                        "Content-Type: application/sdp\r\nContent-Length: %zu\r\n\r\n%s",
                        ctx->cseq, sdp_str.size(), sdp_str.c_str());
                    send(curr_fd, response, strlen(response), 0);
                }
                // SETUP
                else if (strcmp(method, "SETUP") == 0) {
                    // 拒绝 TCP 封装（只支持 UDP RTP）
                    const char* transport_ptr = strstr(buffer, "Transport:");
                    if (transport_ptr && strstr(transport_ptr, "RTP/AVP/TCP")) {
                        snprintf(response, sizeof(response),
                            "RTSP/1.0 461 Unsupported Transport\r\nCSeq: %d\r\n\r\n", ctx->cseq);
                        send(curr_fd, response, strlen(response), 0);
                        continue;
                    }

                    const char* port_ptr = strstr(buffer, "client_port=");
                    if (port_ptr) {
                        int rtcp_port = 0;
                        if (sscanf(port_ptr, "client_port=%d-%d", &ctx->client_port, &rtcp_port) == 0)
                            sscanf(port_ptr, "client_port=%d", &ctx->client_port);
                    }

                    ctx->server_rtp_port = g_server_port_allocator.fetch_add(2);
                    ctx->session_id = generateSessionId();

                    snprintf(response, sizeof(response),
                        "RTSP/1.0 200 OK\r\nCSeq: %d\r\n"
                        "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\n"
                        "Session: %s\r\n\r\n",
                        ctx->cseq,
                        ctx->client_port, ctx->client_port + 1,
                        ctx->server_rtp_port, ctx->server_rtp_port + 1,
                        ctx->session_id.c_str());
                    send(curr_fd, response, strlen(response), 0);
                    std::cout << "📋 [SETUP] Session: " << ctx->session_id << std::endl;
                }
                // PLAY
                else if (strcmp(method, "PLAY") == 0) {
                    if (ctx->client_port == 0) {
                        snprintf(response, sizeof(response),
                            "RTSP/1.0 454 Session Not Found\r\nCSeq: %d\r\n\r\n", ctx->cseq);
                        send(curr_fd, response, strlen(response), 0);
                        continue;
                    }

                    // 创建该客户端专属的 RTP 发送器
                    ctx->rtp_sender = std::make_shared<RtpSender>(
                        ctx->ip, ctx->client_port,
                        ctx->server_rtp_port,
                        ctx->is_playing.get()
                    );
                    *(ctx->is_playing) = true;
                    ctx->need_headers  = true; // 重置，等待 IDR

                    snprintf(response, sizeof(response),
                        "RTSP/1.0 200 OK\r\nCSeq: %d\r\n"
                        "Session: %s\r\nRange: npt=0.000-\r\n\r\n",
                        ctx->cseq, ctx->session_id.c_str());
                    send(curr_fd, response, strlen(response), 0);
                    std::cout << "🚀 [PLAY] " << ctx->session_id
                              << " → " << ctx->ip << ":" << ctx->client_port << std::endl;
                }
                // PAUSE
                else if (strcmp(method, "PAUSE") == 0) {
                    *(ctx->is_playing) = false;
                    snprintf(response, sizeof(response),
                        "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: %s\r\n\r\n",
                        ctx->cseq, ctx->session_id.c_str());
                    send(curr_fd, response, strlen(response), 0);
                    std::cout << "⏸️  [PAUSE] " << ctx->session_id << std::endl;
                }
                // TEARDOWN
                else if (strcmp(method, "TEARDOWN") == 0) {
                    *(ctx->is_playing) = false;
                    snprintf(response, sizeof(response),
                        "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: %s\r\n\r\n",
                        ctx->cseq, ctx->session_id.c_str());
                    send(curr_fd, response, strlen(response), 0);
                    std::cout << "⏹️  [TEARDOWN] " << ctx->session_id << std::endl;
                }
            }
        }
    }

    // 清理
    std::cout << "\n🛑 [Cleanup] 开始清理客户端连接..." << std::endl;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto& pair : m_clients) {
            if (pair.second && pair.second->is_playing)
                *(pair.second->is_playing) = false;
            close(pair.first);
        }
        m_clients.clear();
    }
    close(server_fd);
    close(epoll_fd);
    std::cout << "✅ 服务器已优雅关闭" << std::endl;
    return true;
}

void TcpServer::stopAllStreams() {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto& pair : m_clients)
        if (pair.second && pair.second->is_playing)
            *(pair.second->is_playing) = false;
    std::cout << "🚨 全服断流" << std::endl;
}

void TcpServer::resumeAllStreams() {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto& pair : m_clients) {
        auto ctx = pair.second;
        if (ctx && !*(ctx->is_playing) && ctx->client_port > 0) {
            ctx->rtp_sender = std::make_shared<RtpSender>(
                ctx->ip, ctx->client_port,
                ctx->server_rtp_port,
                ctx->is_playing.get()
            );
            *(ctx->is_playing) = true;
            ctx->need_headers  = true;
        }
    }
    std::cout << "🔥 全服恢复推流" << std::endl;
}

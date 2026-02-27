#include "../include/TcpServer.h"
#include "../include/RtpSender.h"
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
extern std::atomic<int> g_server_port_allocator;

// ➤ 辅助函数：生成随机 Session ID
std::string generateSessionId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(10000000, 99999999);
    return std::to_string(dis(gen));
}

// ➤ 辅助函数：设置非阻塞
void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// ➤ 辅助函数：获取本机局域网 IP
std::string getLocalIP() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return "127.0.0.1"; 
    struct sockaddr_in google_addr;
    memset(&google_addr, 0, sizeof(google_addr));
    google_addr.sin_family = AF_INET;
    google_addr.sin_addr.s_addr = inet_addr("8.8.8.8");
    google_addr.sin_port = htons(53);
    connect(sock, (const struct sockaddr*)&google_addr, sizeof(google_addr));
    struct sockaddr_in local_addr;
    socklen_t addr_len = sizeof(local_addr);
    getsockname(sock, (struct sockaddr*)&local_addr, &addr_len);
    std::string local_ip = inet_ntoa(local_addr.sin_addr);
    close(sock);
    return local_ip;
}

// ◈ 核心服务器启动逻辑
bool TcpServer::start(int port) {
    std::cout << "🎬 Epoll 高并发服务器启动中..." << std::endl;
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

    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1) {
        perror("Epoll add failed"); return false;
    }

    const int MAX_EVENTS = 20;
    struct epoll_event events[MAX_EVENTS];

    std::cout << "👂 Epoll 监控中心已建立，等待连接..." << std::endl;

    while (g_running) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 200);

        if (n == -1) {
            if (errno == EINTR) continue; 
            else { perror("Epoll wait error"); break; }
        }
        
        if (n == 0) continue; 

        for (int i = 0; i < n; i++) {
            int curr_fd = events[i].data.fd;

            // ➤ 迎宾员接到新客人
            if (curr_fd == server_fd) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
                
                if (client_fd == -1) continue;
                setNonBlocking(client_fd);

                event.events = EPOLLIN; // 💡 移除 EPOLLET 边缘触发，避免数据漏读卡死
                event.data.fd = client_fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event);

                std::string ip_str = inet_ntoa(client_addr.sin_addr);
                
                // 🔒 进门登记：必须加锁！
                {
                    std::lock_guard<std::mutex> lock(m_clientsMutex);
                    if (!m_clients[client_fd]) {
                        m_clients[client_fd] = std::make_shared<ClientContext>();
                    }
                    m_clients[client_fd]->ip = ip_str;
                }

                std::cout << "🎉 新连接: " << ip_str << " (FD=" << client_fd << ")" << std::endl;
            }
            // ➤ 服务员处理老客人的请求
            else {
                char buffer[2048] = {0};
                int bytes_read = recv(curr_fd, buffer, sizeof(buffer)-1, 0);

                // 🛑 客人跑路了
                if (bytes_read <= 0) {
                    std::lock_guard<std::mutex> lock(m_clientsMutex); // 🔒 销户加锁
                    if (m_clients.count(curr_fd) && m_clients[curr_fd]) {
                        auto ctx = m_clients[curr_fd];
                        std::cout << "👋 客户端断开 - Session: " << ctx->session_id 
                                  << " | Port: " << ctx->server_rtp_port 
                                  << " | FD: " << curr_fd << std::endl;
                        if (ctx->is_playing) *(ctx->is_playing) = false;
                        m_clients.erase(curr_fd); 
                    } else {
                        std::cout << "👋 客户端断开 (FD=" << curr_fd << ")" << std::endl;
                    }

                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, curr_fd, NULL);
                    close(curr_fd);
                } 
                // 💬 客人说话了
                else {
                    m_clientsMutex.lock(); // 🔒 查户口时加锁保护 map 结构
                    if (!m_clients[curr_fd]) {
                         m_clients[curr_fd] = std::make_shared<ClientContext>();
                    }
                    auto ctx = m_clients[curr_fd];
                    m_clientsMutex.unlock(); // 🔓 获取到引用后即释放锁，提升并发性能！
                    
                    char method[16] = {0}; char url[128] = {0}; char version[16] = {0}; 
                    sscanf(buffer, "%15s %127s %15s", method, url, version);
                    
                    const char* cseq_ptr = strstr(buffer, "CSeq: ");
                    if (cseq_ptr) sscanf(cseq_ptr, "CSeq: %d", &ctx->cseq);

                    char response[2048] = {0};
                    int sent_bytes = 0;

                    // ➤ 处理 OPTIONS
                    if (strcmp(method, "OPTIONS") == 0) {
                        snprintf(response, sizeof(response), "RTSP/1.0 200 OK\r\nCSeq: %d\r\nPublic: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n\r\n", ctx->cseq);
                        sent_bytes = send(curr_fd, response, strlen(response), 0);
                        if (sent_bytes > 0) emit dataSent(sent_bytes); // 📢 触发 UI 信号
                    } 
                    // ➤ 处理 DESCRIBE
                    else if (strcmp(method, "DESCRIBE") == 0) {
                        std::stringstream sdp_stream;
                        sdp_stream << "v=0\r\n"
                                   << "o=- 0 0 IN IP4 " << my_ip << "\r\n"
                                   << "s=No Name\r\n"
                                   << "c=IN IP4 " << my_ip << "\r\n"
                                   << "t=0 0\r\n"
                                   << "m=video 0 RTP/AVP 96\r\n"
                                   << "a=rtpmap:96 H264/90000\r\n"
                                   << "a=fmtp:96 packetization-mode=1\r\n"
                                   << "a=control:track0\r\n";
                        std::string sdp = sdp_stream.str();
                        snprintf(response, sizeof(response), "RTSP/1.0 200 OK\r\nCSeq: %d\r\nContent-Type: application/sdp\r\nContent-Length: %zu\r\n\r\n%s", ctx->cseq, sdp.length(), sdp.c_str());
                        sent_bytes = send(curr_fd, response, strlen(response), 0);
                        if (sent_bytes > 0) emit dataSent(sent_bytes); 
                    }
                    // ➤ 处理 SETUP
                    else if (strcmp(method, "SETUP") == 0) {
                        const char* transport_ptr = strstr(buffer, "Transport:");
                        if (transport_ptr && strstr(transport_ptr, "RTP/AVP/TCP")) {
                             snprintf(response, sizeof(response), "RTSP/1.0 461 Unsupported Transport\r\nCSeq: %d\r\n\r\n", ctx->cseq);
                             sent_bytes = send(curr_fd, response, strlen(response), 0);
                             if (sent_bytes > 0) emit dataSent(sent_bytes);
                             continue;
                        }
                        
                        const char* port_ptr = strstr(buffer, "client_port=");
                        if (port_ptr) {
                            int rtcp_port = 0;
                            int matched = sscanf(port_ptr, "client_port=%d-%d", &ctx->client_port, &rtcp_port);
                            if (matched == 0) sscanf(port_ptr, "client_port=%d", &ctx->client_port);
                        }
                        if (ctx->client_port <= 0) ctx->client_port = 0;
                        
                        ctx->server_rtp_port = g_server_port_allocator.fetch_add(2);
                        ctx->session_id = generateSessionId();

                        snprintf(response, sizeof(response), 
                            "RTSP/1.0 200 OK\r\nCSeq: %d\r\nTransport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\nSession: %s\r\n\r\n", 
                            ctx->cseq, ctx->client_port, ctx->client_port+1, ctx->server_rtp_port, ctx->server_rtp_port+1, ctx->session_id.c_str());
                        sent_bytes = send(curr_fd, response, strlen(response), 0);
                        if (sent_bytes > 0) emit dataSent(sent_bytes); 
                        
                        std::cout << "📋 [SETUP] Session 建立: " << ctx->session_id << std::endl;
                    }
                    // ➤ 处理 PLAY
                    else if (strcmp(method, "PLAY") == 0) {
                        if (ctx->client_port == 0) {
                             snprintf(response, sizeof(response), "RTSP/1.0 454 Session Not Found\r\nCSeq: %d\r\n\r\n", ctx->cseq);
                             sent_bytes = send(curr_fd, response, strlen(response), 0);
                             if (sent_bytes > 0) emit dataSent(sent_bytes);
                             continue;
                        }
                        std::cout << "🚀 [PLAY] 开始播放: " << ctx->session_id << std::endl;
                        snprintf(response, sizeof(response), "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: %s\r\nRange: npt=0.000-\r\n\r\n", ctx->cseq, ctx->session_id.c_str());
                        sent_bytes = send(curr_fd, response, strlen(response), 0);
                        if (sent_bytes > 0) emit dataSent(sent_bytes);

                        *(ctx->is_playing) = true;
                        auto flag_ptr = ctx->is_playing;
                        std::string target_ip = ctx->ip;
                        int target_port = ctx->client_port;
                        int my_local_port = ctx->server_rtp_port;

                        // 🔥 把推流任务扔进线程池，并绑好 UI 信号！
                        this->pool.enqueue([this, target_ip, target_port, my_local_port, flag_ptr]() {
                            RtpSender sender(target_ip, target_port, my_local_port, flag_ptr.get());
                            
                            sender.onTraffic = [this](int bytes) {
                                emit this->dataSent(bytes);
                            };

                            // 判断你要播文件还是直播 
                            sender.sendVideo("../download/test_raw.h264");  // 使用转换后的裸流 H264
                            // sender.sendLiveCamera(); // 已隐藏，避免 WSL 找不到摄像头
                            
                            std::cout << "✅ [Port:" << my_local_port << "] 推流线程结束" << std::endl;
                        });
                    }
                    // ➤ 你的专属 PAUSE 逻辑
                    else if (strcmp(method, "PAUSE") == 0) {
                        snprintf(response, sizeof(response), "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: %s\r\n\r\n", ctx->cseq, ctx->session_id.c_str());
                        sent_bytes = send(curr_fd, response, strlen(response), 0);
                        if (sent_bytes > 0) emit dataSent(sent_bytes); 
                        
                        // 🛑 掐断数据流
                        *(ctx->is_playing) = false; 
                        
                        std::cout << "⏸️  [PAUSE] 收到 VLC 暂停指令，已掐断数据流: " << ctx->session_id << std::endl;
                    }
                    // ➤ 处理 TEARDOWN
                    else if (strcmp(method, "TEARDOWN") == 0) {
                        snprintf(response, sizeof(response), "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: %s\r\n\r\n", ctx->cseq, ctx->session_id.c_str());
                        sent_bytes = send(curr_fd, response, strlen(response), 0);
                        if (sent_bytes > 0) emit dataSent(sent_bytes); 
                        *(ctx->is_playing) = false;
                        std::cout << "⏹️  [TEARDOWN] 停止播放" << std::endl;
                    }
                }
            }
        }
    }

    std::cout << "\n🛑 [Cleanup] 收到退出信号，开始清理战场..." << std::endl;
    int count = 0;
    
    // 🔒 临走前锁门大扫除
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto& pair : m_clients) {
            int fd = pair.first;
            auto ctx = pair.second;
            if (ctx && ctx->is_playing) *(ctx->is_playing) = false;
            close(fd);
            count++;
        }
        m_clients.clear();
    }
    
    std::cout << "🧹 已断开 " << count << " 个客户端连接。" << std::endl;

    std::cout << "⏳ 等待推流线程归位..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); 

    close(server_fd);
    close(epoll_fd);
    std::cout << "✨ 服务器已优雅关闭。下次见，赵大官人～ ❤️" << std::endl;
    return true;
}

// 💥 终极大招 1：UI 触发的一键断流
void TcpServer::stopAllStreams() {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    std::cout << "🚨 [Admin] 收到界面指令：全服断流！" << std::endl;
    for (auto& pair : m_clients) {
        if (pair.second && pair.second->is_playing) {
            *(pair.second->is_playing) = false; 
        }
    }
}

// 💥 终极大招 2：UI 触发的秽土转生
void TcpServer::resumeAllStreams() {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    std::cout << "🔥 [Admin] 收到界面指令：全服恢复推流！" << std::endl;
    for (auto& pair : m_clients) {
        auto ctx = pair.second;
        if (ctx && !*(ctx->is_playing) && ctx->client_port > 0) {
            *(ctx->is_playing) = true;
            auto flag_ptr = ctx->is_playing;
            std::string target_ip = ctx->ip;
            int target_port = ctx->client_port;
            int my_local_port = ctx->server_rtp_port;

            this->pool.enqueue([this, target_ip, target_port, my_local_port, flag_ptr]() {
                RtpSender sender(target_ip, target_port, my_local_port, flag_ptr.get());
                sender.onTraffic = [this](int bytes) {
                    emit this->dataSent(bytes);
                };
                sender.sendVideo("../download/test_raw.h264"); // ✨ 恢复推流使用裸流 H264
                std::cout << "✅ [Port:" << my_local_port << "] 恢复推流线程结束" << std::endl;
            });
        }
    }
}
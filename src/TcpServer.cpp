#include "../include/TcpServer.h"
#include "../include/RtpSender.h"
#include <chrono>
#include <ctime>
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

    // 🔥 启动全局采集线程
    std::thread([this]() {
        this->liveStreamThread();
    }).detach();

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
                    std::shared_ptr<ClientContext> ctx_to_delete;
                    {
                        std::lock_guard<std::mutex> lock(m_clientsMutex); // 🔒 严格销户加锁
                        if (m_clients.count(curr_fd) && m_clients[curr_fd]) {
                            ctx_to_delete = m_clients[curr_fd];
                            auto it = m_clients.find(curr_fd);
                            if (it != m_clients.end()) {
                                m_clients.erase(it); 
                            }
                        }
                    } // 锁释放，接下来慢慢销毁对象
                    
                    if (ctx_to_delete) {
                        std::cout << "👋 客户端断开 - Session: " << ctx_to_delete->session_id 
                                  << " | Port: " << ctx_to_delete->server_rtp_port 
                                  << " | FD: " << curr_fd << std::endl;
                        if (ctx_to_delete->is_playing) *(ctx_to_delete->is_playing) = false;
                        ctx_to_delete->rtp_sender.reset(); // 主动安全释放
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
                        
                        // ✨ 为该客户端创建专属的 Rtp 发送器
                        ctx->rtp_sender = std::make_shared<RtpSender>(
                            ctx->ip, 
                            ctx->client_port, 
                            ctx->server_rtp_port, 
                            ctx->is_playing.get()
                        );
                        
                        ctx->rtp_sender->onTraffic = [this](int bytes) {
                            emit this->dataSent(bytes);
                        };

                        std::cout << "✅ [Port:" << ctx->server_rtp_port << "] 客户端已加入直播分发群" << std::endl;
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
            
            // ✨ 恢复推流，只需重建 Sender 对象，不需要开新线程
            ctx->rtp_sender = std::make_shared<RtpSender>(
                ctx->ip, 
                ctx->client_port, 
                ctx->server_rtp_port, 
                ctx->is_playing.get()
            );
            
            ctx->rtp_sender->onTraffic = [this](int bytes) {
                emit this->dataSent(bytes);
            };
            
            std::cout << "✅ [Port:" << ctx->server_rtp_port << "] 客户端已重新加入直播分发群" << std::endl;
        }
    }
}

// 🚀 终极大招：零延迟 TCP 直通采集线程
// 该线程在本地 6000 端口开设 TCP 监听，坐等 Windows FFmpeg 把最高清、最完美的 H264 裸流推送过来
// 收到数据后，绝不解码、绝不二次压缩，而是直接在内存里“切果冻”，切出一个 NALU 就秒发给所有连上的观众！
void TcpServer::liveStreamThread() {
    int ingest_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ingest_fd < 0) {
        perror("Ingest socket failed");
        return;
    }

    int opt = 1;
    setsockopt(ingest_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in ingest_addr;
    memset(&ingest_addr, 0, sizeof(ingest_addr));
    ingest_addr.sin_family = AF_INET;
    ingest_addr.sin_addr.s_addr = INADDR_ANY;
    ingest_addr.sin_port = htons(6000); // 监听 6000 端口接收 FFmpeg 推流

    if (bind(ingest_fd, (struct sockaddr*)&ingest_addr, sizeof(ingest_addr)) < 0) {
        perror("Ingest bind failed");
        close(ingest_fd);
        return;
    }

    if (listen(ingest_fd, 1) < 0) {
        perror("Ingest listen failed");
        close(ingest_fd);
        return;
    }

    uint32_t timestamp_increment = 90000 / 30; // 假设 30 FPS 的时间戳增量

    while (g_running) {
        std::cout << "\n🚰 [管道工] TCP 吞吐口已在端口 6000 准备就绪，等待 Windows FFmpeg 连接推流..." << std::endl;
        
        struct sockaddr_in ffmpeg_addr;
        socklen_t ffmpeg_len = sizeof(ffmpeg_addr);
        int ffmpeg_fd = accept(ingest_fd, (struct sockaddr*)&ffmpeg_addr, &ffmpeg_len);
        
        if (ffmpeg_fd < 0) {
            if (!g_running) break;
            continue;
        }
        
        std::cout << "✅ [管道工] 采集端已接入 (" << inet_ntoa(ffmpeg_addr.sin_addr) 
                  << ")！启动极速内存分发引警，零延迟推流中！" << std::endl;

        // 建立一个大缓冲区用来拼接 TCP 碎片
        const int BUF_SIZE = 1024 * 512; // 512KB
        std::vector<uint8_t> buffer(BUF_SIZE);
        int data_len = 0;

        while (g_running) {
            // 一次大口吸入数据
            int bytes_read = recv(ffmpeg_fd, buffer.data() + data_len, BUF_SIZE - data_len, 0);
            if (bytes_read <= 0) {
                std::cout << "🛑 [管道工] Windows FFmpeg 断开了连接，画面中止。" << std::endl;
                break; // 退出内层循环，重新等待下一次推流连入
            }
            
            data_len += bytes_read;

            // 在缓冲区中寻找 NALU 起始码 (0x00 00 00 01)
            int pos = 0;
            while (pos < data_len - 4) {
                // 找当前 NALU 头部
                if (buffer[pos] == 0 && buffer[pos+1] == 0 && buffer[pos+2] == 0 && buffer[pos+3] == 1) {
                    
                    // 顺藤摸瓜找下一个 NALU 头部，就能知道当前这块 NALU 有多大
                    int next_pos = -1;
                    for (int i = pos + 4; i < data_len - 3; ++i) {
                        if (buffer[i] == 0 && buffer[i+1] == 0 && buffer[i+2] == 0 && buffer[i+3] == 1) {
                            next_pos = i;
                            break;
                        }
                    }

                    // 如果没找到下一个头，说明当前这块 NALU 还没接收完整，等下一次 recv 补齐
                    if (next_pos == -1) {
                        break; 
                    }

                    // 完美切出一块 NALU
                    uint8_t* nalu_data = buffer.data() + pos;
                    int nalu_size = next_pos - pos;
                    
                    // 剥掉起始码衣服 (4字节) 送进 RTP 管道
                    nalu_data += 4;
                    nalu_size -= 4;

                    uint8_t nal_type = nalu_data[0] & 0x1F;
                    bool has_idr = (nal_type == 5 || nal_type == 7 || nal_type == 8); // I帧或参数集

                    std::lock_guard<std::mutex> lock(m_clientsMutex);
                    for (auto& pair : m_clients) {
                        auto ctx = pair.second;
                        if (ctx && *(ctx->is_playing) && ctx->rtp_sender) {
                            // 必须保证新来的观众第一眼看到的是关键帧，否则黑屏
                            if (ctx->need_headers) {
                                if (has_idr) {
                                    ctx->need_headers = false;
                                    ctx->rtp_sender->sendNalu(nalu_data, nalu_size, timestamp_increment, true);
                                }
                            } else {
                                ctx->rtp_sender->sendNalu(nalu_data, nalu_size, timestamp_increment, true);
                            }
                        }
                    }

                    // 前进到下一块 NALU
                    pos = next_pos;
                } else {
                    pos++;
                }
            } // end while 找 NALU

            // 把没处理完的尾巴残缺数据，挪到缓冲区的最前面，等下次 recv 拼接
            if (pos > 0 && pos < data_len) {
                memmove(buffer.data(), buffer.data() + pos, data_len - pos);
                data_len -= pos;
            } else if (pos == data_len) {
                data_len = 0; // 刚好切得干干净净
            }

        } // end while 接收数据

        close(ffmpeg_fd);
    } // end while 等待采集端

    close(ingest_fd);
    std::cout << "🏁 [管道工] 线程已全部退出" << std::endl;
}

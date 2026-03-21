#include "TcpServer.h"
#include <arpa/inet.h>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <random>
#include <sstream>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
std::mutex g_sps_pps_mutex;
std::vector<uint8_t> g_cached_sps;
std::vector<uint8_t> g_cached_pps;

std::string base64Encode(const uint8_t* data, size_t len) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3) {
        const uint32_t octet_a = data[i];
        const uint32_t octet_b = (i + 1 < len) ? data[i + 1] : 0;
        const uint32_t octet_c = (i + 2 < len) ? data[i + 2] : 0;
        const uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        out.push_back(kTable[(triple >> 18) & 0x3F]);
        out.push_back(kTable[(triple >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? kTable[(triple >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? kTable[triple & 0x3F] : '=');
    }

    return out;
}

std::string buildH264FmtpLine() {
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    {
        std::lock_guard<std::mutex> lock(g_sps_pps_mutex);
        sps = g_cached_sps;
        pps = g_cached_pps;
    }

    if (sps.empty() || pps.empty()) {
        return "a=fmtp:96 packetization-mode=1\r\n";
    }

    char profile_level_id[7] = {0};
    if (sps.size() >= 4) {
        snprintf(profile_level_id, sizeof(profile_level_id), "%02X%02X%02X",
                 sps[1], sps[2], sps[3]);
    } else {
        snprintf(profile_level_id, sizeof(profile_level_id), "42E01F");
    }

    return "a=fmtp:96 packetization-mode=1;profile-level-id=" +
           std::string(profile_level_id) +
           ";sprop-parameter-sets=" +
           base64Encode(sps.data(), sps.size()) + "," +
           base64Encode(pps.data(), pps.size()) + "\r\n";
}

std::string generateSessionId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(10000000, 99999999);
    return std::to_string(dis(gen));
}

void setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
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

std::string normalizeStreamPath(const std::string& input) {
    std::string path = input;
    while (!path.empty() && std::isspace(static_cast<unsigned char>(path.front()))) {
        path.erase(path.begin());
    }
    while (!path.empty() && std::isspace(static_cast<unsigned char>(path.back()))) {
        path.pop_back();
    }
    if (path.empty()) return "";
    if (path[0] != '/') {
        path.insert(path.begin(), '/');
    }
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

std::string extractPathFromUrl(const std::string& url) {
    if (url.empty()) return "/";
    const std::string scheme = "rtsp://";
    size_t path_pos = std::string::npos;
    if (url.rfind(scheme, 0) == 0) {
        const size_t host_end = url.find('/', scheme.size());
        if (host_end != std::string::npos) {
            path_pos = host_end;
        }
    } else if (url[0] == '/') {
        path_pos = 0;
    }

    if (path_pos == std::string::npos) {
        return "/";
    }

    std::string path = url.substr(path_pos);
    const size_t query_pos = path.find('?');
    if (query_pos != std::string::npos) {
        path = path.substr(0, query_pos);
    }
    if (path.empty()) return "/";
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

bool pathMatches(const std::string& request_path, const std::string& stream_path) {
    if (stream_path.empty() || stream_path == "/") {
        return true;
    }
    if (request_path == stream_path) {
        return true;
    }
    const std::string prefix = stream_path + "/";
    return request_path.rfind(prefix, 0) == 0;
}

bool extractHeader(const char* request, const char* key, std::string& value) {
    if (!request || !key) return false;
    std::string needle = std::string("\r\n") + key + ":";
    const char* begin = strstr(request, needle.c_str());
    if (!begin) {
        if (strncmp(request, key, strlen(key)) == 0 && strstr(request, ":")) {
            begin = request;
        } else {
            return false;
        }
    } else {
        begin += 2;
    }

    const char* colon = strchr(begin, ':');
    if (!colon) return false;
    colon++;
    while (*colon == ' ') colon++;
    const char* end = strstr(colon, "\r\n");
    if (!end) end = colon + strlen(colon);
    value.assign(colon, end - colon);
    return !value.empty();
}

bool sessionMatches(const std::string& expect, const char* request) {
    if (expect.empty()) return true;
    std::string session_header;
    if (!extractHeader(request, "Session", session_header)) return false;
    return session_header == expect;
}

bool parseContentLength(const std::string& header, size_t& out_len) {
    const char* keys[] = {"Content-Length:", "Content-length:"};
    for (const char* key : keys) {
        size_t pos = header.find(key);
        if (pos == std::string::npos) continue;
        pos += strlen(key);
        while (pos < header.size() && header[pos] == ' ') pos++;
        size_t end = pos;
        while (end < header.size() && std::isdigit(static_cast<unsigned char>(header[end]))) {
            end++;
        }
        if (end == pos) return false;
        out_len = static_cast<size_t>(std::strtoul(header.c_str() + pos, nullptr, 10));
        return true;
    }
    return false;
}

constexpr size_t kMaxRecvBufferBytes = 64 * 1024;
} // namespace

TcpServer::TcpServer(int rtp_port_base, int rtp_pair_count) {
    if (rtp_pair_count < 1) {
        rtp_pair_count = 1;
    }
    if (rtp_port_base % 2 != 0) {
        rtp_port_base++;
    }

    m_freeRtpPorts.reserve(static_cast<size_t>(rtp_pair_count));
    for (int i = 0; i < rtp_pair_count; ++i) {
        m_freeRtpPorts.push_back(rtp_port_base + i * 2);
    }
}

int TcpServer::allocateServerRtpPort() {
    std::lock_guard<std::mutex> lock(m_portPoolMutex);
    if (m_freeRtpPorts.empty()) {
        return 0;
    }
    const int port = m_freeRtpPorts.back();
    m_freeRtpPorts.pop_back();
    return port;
}

void TcpServer::releaseServerRtpPort(int port) {
    if (port <= 0) return;
    std::lock_guard<std::mutex> lock(m_portPoolMutex);
    m_freeRtpPorts.push_back(port);
}

int TcpServer::onlineClients() const {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    int count = 0;
    for (const auto& pair : m_clients) {
        const auto& ctx = pair.second;
        if (!ctx) continue;
        if (ctx->state == SessionState::READY ||
            ctx->state == SessionState::PLAYING ||
            ctx->state == SessionState::PAUSED) {
            count++;
        }
    }
    return count;
}

uint64_t TcpServer::totalTrafficBytes() const {
    return m_totalTrafficBytes.load(std::memory_order_relaxed);
}

void TcpServer::dispatchNalu(uint8_t* data, int size, uint32_t timestamp) {
    if (size <= 0 || !data) return;

    const uint8_t nal_type = data[0] & 0x1F;
    const bool is_idr = (nal_type == 5);

    if (nal_type == 7 || nal_type == 8) {
        std::lock_guard<std::mutex> cache_lock(g_sps_pps_mutex);
        if (nal_type == 7) {
            g_cached_sps.assign(data, data + size);
        } else {
            g_cached_pps.assign(data, data + size);
        }
    }

    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto& pair : m_clients) {
        auto ctx = pair.second;
        if (!ctx || ctx->state != SessionState::PLAYING || !ctx->rtp_sender || !*(ctx->is_playing)) {
            continue;
        }

        if (ctx->need_headers) {
            if (!is_idr) {
                continue;
            }

            std::vector<uint8_t> sps;
            std::vector<uint8_t> pps;
            {
                std::lock_guard<std::mutex> cache_lock(g_sps_pps_mutex);
                sps = g_cached_sps;
                pps = g_cached_pps;
            }

            if (!sps.empty()) {
                ctx->rtp_sender->sendNalu(sps.data(), static_cast<int>(sps.size()), timestamp, false);
            }
            if (!pps.empty()) {
                ctx->rtp_sender->sendNalu(pps.data(), static_cast<int>(pps.size()), timestamp, false);
            }
            ctx->need_headers = false;
        }

        const bool is_last = (nal_type != 7 && nal_type != 8);
        ctx->rtp_sender->sendNalu(data, size, timestamp, is_last);
    }
}

bool TcpServer::start(const std::string& bind_ip, int port, const std::string& stream_path, std::atomic<bool>* running_flag) {
    std::cout << "[INFO] Epoll RTSP server starting..." << std::endl;
    const std::string my_ip = getLocalIP();
    std::cout << "[INFO] Host IP: " << my_ip << std::endl;

    m_streamPath = normalizeStreamPath(stream_path);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return false;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    if (bind_ip.empty() || bind_ip == "0.0.0.0") {
        server_addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, bind_ip.c_str(), &server_addr.sin_addr) != 1) {
            std::cerr << "[ERROR] Invalid bind IP: " << bind_ip << std::endl;
            close(server_fd);
            return false;
        }
    }
    server_addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_fd);
        return false;
    }
    if (listen(server_fd, 100) < 0) {
        perror("Listen failed");
        close(server_fd);
        return false;
    }

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("Epoll create failed");
        close(server_fd);
        return false;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    const int MAX_EVENTS = 20;
    struct epoll_event events[MAX_EVENTS];

    std::cout << "[INFO] Waiting RTSP clients on port " << port << std::endl;

    while (running_flag && *running_flag) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 200);
        if (n == -1) {
            if (errno == EINTR) continue;
            perror("Epoll wait error");
            break;
        }
        if (n == 0) continue;

        for (int i = 0; i < n; i++) {
            int curr_fd = events[i].data.fd;

            if (curr_fd == server_fd) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
                if (client_fd < 0) continue;
                setNonBlocking(client_fd);

                ev.events = EPOLLIN;
                ev.data.fd = client_fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);

                std::string ip = inet_ntoa(client_addr.sin_addr);
                {
                    std::lock_guard<std::mutex> lock(m_clientsMutex);
                    auto client = std::make_shared<ClientContext>();
                    client->ip = ip;
                    client->connected_at = std::chrono::system_clock::now();
                    m_clients[client_fd] = client;
                }
                std::cout << "[INFO] New connection " << ip << " (FD=" << client_fd << ")" << std::endl;
                continue;
            }

            char buffer[2048] = {0};
            int bytes_read = recv(curr_fd, buffer, sizeof(buffer) - 1, 0);
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
                    *(dead->is_playing) = false;
                    dead->rtp_sender.reset();
                    releaseServerRtpPort(dead->server_rtp_port);
                    dead->server_rtp_port = 0;
                    dead->state = SessionState::CLOSED;
                    std::cout << "[INFO] Client disconnected session=" << dead->session_id
                              << " fd=" << curr_fd << std::endl;
                }
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, curr_fd, nullptr);
                close(curr_fd);
                continue;
            }

            std::shared_ptr<ClientContext> ctx;
            {
                std::lock_guard<std::mutex> lock(m_clientsMutex);
                auto& entry = m_clients[curr_fd];
                if (!entry) entry = std::make_shared<ClientContext>();
                ctx = entry;
            }

            ctx->recv_buffer.append(buffer, static_cast<size_t>(bytes_read));
            if (ctx->recv_buffer.size() > kMaxRecvBufferBytes) {
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
                    *(dead->is_playing) = false;
                    dead->rtp_sender.reset();
                    releaseServerRtpPort(dead->server_rtp_port);
                    dead->server_rtp_port = 0;
                    dead->server_rtcp_port = 0;
                    dead->state = SessionState::CLOSED;
                }
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, curr_fd, nullptr);
                close(curr_fd);
                std::cout << "[WARN] Client recv buffer exceeded limit, closing fd="
                          << curr_fd << std::endl;
                continue;
            }

            auto handleRequest = [&](const std::string& request) -> bool {
                const char* req = request.c_str();
                char method[16] = {0};
                char url[128] = {0};
                char version[16] = {0};
                sscanf(req, "%15s %127s %15s", method, url, version);
                const std::string request_url = url;
                const std::string request_path = extractPathFromUrl(request_url);
                if (!request_url.empty()) {
                    ctx->url = request_url;
                }

                const char* cseq_ptr = strstr(req, "CSeq: ");
                if (cseq_ptr) {
                    sscanf(cseq_ptr, "CSeq: %d", &ctx->cseq);
                }

                char response[2048] = {0};

                if (strcmp(method, "OPTIONS") == 0) {
                    snprintf(response, sizeof(response),
                             "RTSP/1.0 200 OK\r\nCSeq: %d\r\n"
                             "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN\r\n\r\n",
                             ctx->cseq);
                    send(curr_fd, response, strlen(response), 0);
                } else if (strcmp(method, "DESCRIBE") == 0) {
                    if (!pathMatches(request_path, m_streamPath)) {
                        snprintf(response, sizeof(response),
                                 "RTSP/1.0 404 Not Found\r\nCSeq: %d\r\n\r\n", ctx->cseq);
                        send(curr_fd, response, strlen(response), 0);
                        return false;
                    }
                    struct sockaddr_in local_addr;
                    socklen_t local_len = sizeof(local_addr);
                    getsockname(curr_fd, (struct sockaddr*)&local_addr, &local_len);
                    std::string current_ip = inet_ntoa(local_addr.sin_addr);

                    const std::string fmtp_line = buildH264FmtpLine();
                    std::stringstream sdp;
                    sdp << "v=0\r\n"
                        << "o=- 0 0 IN IP4 " << current_ip << "\r\n"
                        << "s=EdgeGateway Live\r\n"
                        << "c=IN IP4 " << current_ip << "\r\n"
                        << "t=0 0\r\n"
                        << "m=video 0 RTP/AVP 96\r\n"
                        << "a=rtpmap:96 H264/90000\r\n"
                        << fmtp_line
                        << "a=control:track0\r\n";
                    const std::string sdp_str = sdp.str();

                    std::string base_url = url;
                    if (!base_url.empty() && base_url.back() != '/') {
                        base_url += "/";
                    }

                    snprintf(response, sizeof(response),
                             "RTSP/1.0 200 OK\r\nCSeq: %d\r\n"
                             "Content-Base: %s\r\n"
                             "Content-Type: application/sdp\r\nContent-Length: %zu\r\n\r\n%s",
                             ctx->cseq, base_url.c_str(), sdp_str.size(), sdp_str.c_str());
                    send(curr_fd, response, strlen(response), 0);
                } else if (strcmp(method, "SETUP") == 0) {
                    if (!pathMatches(request_path, m_streamPath)) {
                        snprintf(response, sizeof(response),
                                 "RTSP/1.0 404 Not Found\r\nCSeq: %d\r\n\r\n", ctx->cseq);
                        send(curr_fd, response, strlen(response), 0);
                        return false;
                    }
                    if (!(ctx->state == SessionState::INIT || ctx->state == SessionState::READY || ctx->state == SessionState::PAUSED)) {
                        snprintf(response, sizeof(response),
                                 "RTSP/1.0 455 Method Not Valid in This State\r\nCSeq: %d\r\n\r\n", ctx->cseq);
                        send(curr_fd, response, strlen(response), 0);
                        return false;
                    }

                    const char* transport_ptr = strstr(req, "Transport:");
                    if (transport_ptr && strstr(transport_ptr, "RTP/AVP/TCP")) {
                        snprintf(response, sizeof(response),
                                 "RTSP/1.0 461 Unsupported Transport\r\nCSeq: %d\r\n\r\n", ctx->cseq);
                        send(curr_fd, response, strlen(response), 0);
                        return false;
                    }

                    const char* port_ptr = strstr(req, "client_port=");
                    if (!port_ptr) {
                        snprintf(response, sizeof(response),
                                 "RTSP/1.0 400 Bad Request\r\nCSeq: %d\r\n\r\n", ctx->cseq);
                        send(curr_fd, response, strlen(response), 0);
                        return false;
                    }

                    int rtcp_port = 0;
                    if (sscanf(port_ptr, "client_port=%d-%d", &ctx->client_port, &rtcp_port) <= 0) {
                        if (sscanf(port_ptr, "client_port=%d", &ctx->client_port) <= 0) {
                            snprintf(response, sizeof(response),
                                     "RTSP/1.0 400 Bad Request\r\nCSeq: %d\r\n\r\n", ctx->cseq);
                            send(curr_fd, response, strlen(response), 0);
                            return false;
                        }
                    }

                    if (ctx->server_rtp_port > 0) {
                        releaseServerRtpPort(ctx->server_rtp_port);
                        ctx->server_rtp_port = 0;
                        ctx->server_rtcp_port = 0;
                    }

                    const int allocated_port = allocateServerRtpPort();
                    if (allocated_port <= 0) {
                        snprintf(response, sizeof(response),
                                 "RTSP/1.0 453 Not Enough Bandwidth\r\nCSeq: %d\r\n\r\n", ctx->cseq);
                        send(curr_fd, response, strlen(response), 0);
                        return false;
                    }

                    ctx->server_rtp_port = allocated_port;
                    ctx->server_rtcp_port = allocated_port + 1;
                    if (ctx->session_id.empty()) {
                        ctx->session_id = generateSessionId();
                    }
                    ctx->state = SessionState::READY;

                    snprintf(response, sizeof(response),
                             "RTSP/1.0 200 OK\r\nCSeq: %d\r\n"
                             "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\n"
                             "Session: %s\r\n\r\n",
                             ctx->cseq,
                             ctx->client_port, ctx->client_port + 1,
                             ctx->server_rtp_port, ctx->server_rtcp_port,
                             ctx->session_id.c_str());
                    send(curr_fd, response, strlen(response), 0);
                    std::cout << "[INFO] SETUP session=" << ctx->session_id << std::endl;
                } else if (strcmp(method, "PLAY") == 0) {
                    if (!pathMatches(request_path, m_streamPath)) {
                        snprintf(response, sizeof(response),
                                 "RTSP/1.0 404 Not Found\r\nCSeq: %d\r\n\r\n", ctx->cseq);
                        send(curr_fd, response, strlen(response), 0);
                        return false;
                    }
                    if (!sessionMatches(ctx->session_id, req)) {
                        snprintf(response, sizeof(response),
                                 "RTSP/1.0 454 Session Not Found\r\nCSeq: %d\r\n\r\n", ctx->cseq);
                        send(curr_fd, response, strlen(response), 0);
                        return false;
                    }

                    if (!(ctx->state == SessionState::READY || ctx->state == SessionState::PAUSED)) {
                        snprintf(response, sizeof(response),
                                 "RTSP/1.0 455 Method Not Valid in This State\r\nCSeq: %d\r\n\r\n", ctx->cseq);
                        send(curr_fd, response, strlen(response), 0);
                        return false;
                    }

                    if (ctx->client_port <= 0 || ctx->server_rtp_port <= 0) {
                        snprintf(response, sizeof(response),
                                 "RTSP/1.0 454 Session Not Found\r\nCSeq: %d\r\n\r\n", ctx->cseq);
                        send(curr_fd, response, strlen(response), 0);
                        return false;
                    }

                    if (!ctx->rtp_sender) {
                        ctx->rtp_sender = std::make_shared<RtpSender>(
                            ctx->ip, ctx->client_port, ctx->server_rtp_port, ctx->is_playing.get());
                        ctx->rtp_sender->onTraffic = [this](int bytes) {
                            m_totalTrafficBytes.fetch_add(static_cast<uint64_t>(bytes), std::memory_order_relaxed);
                        };
                    }

                    *(ctx->is_playing) = true;
                    ctx->need_headers = true;
                    ctx->state = SessionState::PLAYING;

                    snprintf(response, sizeof(response),
                             "RTSP/1.0 200 OK\r\nCSeq: %d\r\n"
                             "Session: %s\r\nRange: npt=0.000-\r\n\r\n",
                             ctx->cseq, ctx->session_id.c_str());
                    send(curr_fd, response, strlen(response), 0);
                    std::cout << "[INFO] PLAY session=" << ctx->session_id
                              << " dst=" << ctx->ip << ":" << ctx->client_port << std::endl;
                } else if (strcmp(method, "PAUSE") == 0) {
                    if (!pathMatches(request_path, m_streamPath)) {
                        snprintf(response, sizeof(response),
                                 "RTSP/1.0 404 Not Found\r\nCSeq: %d\r\n\r\n", ctx->cseq);
                        send(curr_fd, response, strlen(response), 0);
                        return false;
                    }
                    if (!sessionMatches(ctx->session_id, req)) {
                        snprintf(response, sizeof(response),
                                 "RTSP/1.0 454 Session Not Found\r\nCSeq: %d\r\n\r\n", ctx->cseq);
                        send(curr_fd, response, strlen(response), 0);
                        return false;
                    }

                    if (ctx->state != SessionState::PLAYING) {
                        snprintf(response, sizeof(response),
                                 "RTSP/1.0 455 Method Not Valid in This State\r\nCSeq: %d\r\n\r\n", ctx->cseq);
                        send(curr_fd, response, strlen(response), 0);
                        return false;
                    }

                    *(ctx->is_playing) = false;
                    ctx->state = SessionState::PAUSED;
                    snprintf(response, sizeof(response),
                             "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: %s\r\n\r\n",
                             ctx->cseq, ctx->session_id.c_str());
                    send(curr_fd, response, strlen(response), 0);
                    std::cout << "[INFO] PAUSE session=" << ctx->session_id << std::endl;
                } else if (strcmp(method, "TEARDOWN") == 0) {
                    if (!pathMatches(request_path, m_streamPath)) {
                        snprintf(response, sizeof(response),
                                 "RTSP/1.0 404 Not Found\r\nCSeq: %d\r\n\r\n", ctx->cseq);
                        send(curr_fd, response, strlen(response), 0);
                        return false;
                    }
                    if (!sessionMatches(ctx->session_id, req)) {
                        snprintf(response, sizeof(response),
                                 "RTSP/1.0 454 Session Not Found\r\nCSeq: %d\r\n\r\n", ctx->cseq);
                        send(curr_fd, response, strlen(response), 0);
                        return false;
                    }

                    if (!(ctx->state == SessionState::READY || ctx->state == SessionState::PLAYING || ctx->state == SessionState::PAUSED)) {
                        snprintf(response, sizeof(response),
                                 "RTSP/1.0 455 Method Not Valid in This State\r\nCSeq: %d\r\n\r\n", ctx->cseq);
                        send(curr_fd, response, strlen(response), 0);
                        return false;
                    }

                    *(ctx->is_playing) = false;
                    ctx->rtp_sender.reset();
                    releaseServerRtpPort(ctx->server_rtp_port);
                    ctx->server_rtp_port = 0;
                    ctx->server_rtcp_port = 0;
                    ctx->state = SessionState::CLOSED;

                    snprintf(response, sizeof(response),
                             "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: %s\r\n\r\n",
                             ctx->cseq, ctx->session_id.c_str());
                    send(curr_fd, response, strlen(response), 0);

                    {
                        std::lock_guard<std::mutex> lock(m_clientsMutex);
                        m_clients.erase(curr_fd);
                    }
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, curr_fd, nullptr);
                    close(curr_fd);
                    std::cout << "[INFO] TEARDOWN session=" << ctx->session_id << std::endl;
                    return true;
                } else {
                    snprintf(response, sizeof(response),
                             "RTSP/1.0 405 Method Not Allowed\r\nCSeq: %d\r\n\r\n", ctx->cseq);
                    send(curr_fd, response, strlen(response), 0);
                }

                return false;
            };

            while (true) {
                const size_t header_end = ctx->recv_buffer.find("\r\n\r\n");
                if (header_end == std::string::npos) {
                    break;
                }

                const size_t header_len = header_end + 4;
                size_t content_len = 0;
                parseContentLength(ctx->recv_buffer.substr(0, header_len), content_len);

                if (ctx->recv_buffer.size() < header_len + content_len) {
                    break;
                }

                std::string request = ctx->recv_buffer.substr(0, header_len + content_len);
                ctx->recv_buffer.erase(0, header_len + content_len);

                if (handleRequest(request)) {
                    break;
                }
            }
        }
    }

    std::cout << "[INFO] Cleaning up client connections..." << std::endl;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto& pair : m_clients) {
            auto& ctx = pair.second;
            if (ctx && ctx->is_playing) {
                *(ctx->is_playing) = false;
            }
            if (ctx) {
                releaseServerRtpPort(ctx->server_rtp_port);
                ctx->server_rtp_port = 0;
                ctx->server_rtcp_port = 0;
                ctx->state = SessionState::CLOSED;
            }
            close(pair.first);
        }
        m_clients.clear();
    }

    close(server_fd);
    close(epoll_fd);
    std::cout << "[INFO] Server shutdown complete" << std::endl;
    return true;
}

std::vector<TcpServer::ClientInfo> TcpServer::clientInfos() const {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    std::vector<ClientInfo> infos;
    infos.reserve(m_clients.size());
    for (const auto& pair : m_clients) {
        const auto& ctx = pair.second;
        if (!ctx) continue;
        infos.push_back(ClientInfo{ctx->ip, ctx->url, ctx->connected_at, ctx->state});
    }
    return infos;
}

void TcpServer::stopAllStreams() {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto& pair : m_clients) {
        auto ctx = pair.second;
        if (ctx && ctx->is_playing) {
            *(ctx->is_playing) = false;
            if (ctx->state == SessionState::PLAYING) {
                ctx->state = SessionState::PAUSED;
            }
        }
    }
    std::cout << "[INFO] Stop all streams" << std::endl;
}

void TcpServer::resumeAllStreams() {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto& pair : m_clients) {
        auto ctx = pair.second;
        if (!ctx || ctx->client_port <= 0 || ctx->server_rtp_port <= 0) {
            continue;
        }

        if (ctx->state == SessionState::READY || ctx->state == SessionState::PAUSED) {
            if (!ctx->rtp_sender) {
                ctx->rtp_sender = std::make_shared<RtpSender>(
                    ctx->ip, ctx->client_port, ctx->server_rtp_port, ctx->is_playing.get());
                ctx->rtp_sender->onTraffic = [this](int bytes) {
                    m_totalTrafficBytes.fetch_add(static_cast<uint64_t>(bytes), std::memory_order_relaxed);
                };
            }
            *(ctx->is_playing) = true;
            ctx->need_headers = true;
            ctx->state = SessionState::PLAYING;
        }
    }
    std::cout << "[INFO] Resume all streams" << std::endl;
}

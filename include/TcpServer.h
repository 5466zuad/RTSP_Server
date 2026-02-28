#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <QObject>      // 👈 必须继承它才能用信号槽！
#include <map>
#include <mutex>
#include <string>
#include <atomic>
#include <memory>
#include "ThreadPool.h"
 // 你的线程池


#include "RtpSender.h" // 引入 RtpSender 供 ClientContext 使用

// 把客户档案提到这里，因为成员变量要用到它
struct ClientContext {
    std::string ip;
    int client_port = 0;
    int server_rtp_port = 0;
    int cseq = 0;
    std::string session_id; 
    std::shared_ptr<std::atomic<bool>> is_playing;
    std::string read_buffer;
    std::shared_ptr<RtpSender> rtp_sender; // ✨ 新增：每个客户端专属的点对点发包员

    ClientContext() {
        is_playing = std::make_shared<std::atomic<bool>>(false);
    }
};

class TcpServer : public QObject {
    Q_OBJECT // 👈 灵魂宏！加了它才能用 emit

public:
    TcpServer(QObject *parent = nullptr) : QObject(parent), pool(10) {} // 初始化10个小弟
    bool start(int port);

signals:
    // 📢 告诉老板娘：我又发了多少数据！
    void dataSent(int bytes);

public slots:
    // 📢 接收老板娘的指令：全服停工/开工！
    void stopAllStreams();
    void resumeAllStreams();

private:
    void liveStreamThread(); // ✨ 新增：全局唯一的后台采编大内总管
    
    ThreadPool pool;
    
    // 🗃️ 核心：全服客户名单与安全锁
    std::map<int, std::shared_ptr<ClientContext>> m_clients; 
    std::mutex m_clientsMutex; 
};

#endif
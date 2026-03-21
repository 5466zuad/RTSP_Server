#pragma once

#include <QImage>
#include <QObject>
#include <atomic>
#include <string>
#include <thread>

class RtspPlayer : public QObject {
    Q_OBJECT

public:
    explicit RtspPlayer(QObject* parent = nullptr);
    ~RtspPlayer() override;

    void start(const std::string& url);
    void stop();
    bool isRunning() const;

signals:
    void frameReady(const QImage& frame);
    void error(const QString& message);

private:
    void run(std::string url);

    std::atomic<bool> m_running{false};
    std::thread m_thread;
};

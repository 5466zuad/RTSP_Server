#pragma once

#include <QMainWindow>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QFile>
#include <atomic>
#include <fstream>
#include <thread>

#include "TcpServer.h"
#include "CameraCapture.h"
#include "RtspPlayer.h"
#include "VideoWidget.h"

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onStartClicked();
    void onStopClicked();
    void refreshStatus();
    void refreshConnections();
    void refreshLog();
    void onPlayerError(const QString& message);

private:
    void setupUi();
    void startServer();
    void stopServer();
    void startLogCapture();
    void stopLogCapture();

    QLineEdit* m_ipEdit = nullptr;
    QLineEdit* m_portEdit = nullptr;
    QLineEdit* m_pathEdit = nullptr;
    QPushButton* m_startButton = nullptr;
    QPushButton* m_stopButton = nullptr;
    QLabel* m_statusLabel = nullptr;
    QTableWidget* m_connectionsTable = nullptr;
    QPlainTextEdit* m_logView = nullptr;
    QTimer* m_statusTimer = nullptr;
    QTimer* m_logTimer = nullptr;
    VideoWidget* m_videoWidget = nullptr;
    RtspPlayer* m_player = nullptr;

    TcpServer m_server;
    CameraCapture m_camera;
    std::thread m_serverThread;
    std::thread m_captureThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_startFailed{false};

    std::ofstream m_logFile;
    std::streambuf* m_coutBuf = nullptr;
    std::streambuf* m_cerrBuf = nullptr;
    QFile m_logReadFile;
    qint64 m_logOffset = 0;
    std::string buildPreviewUrl() const;
};

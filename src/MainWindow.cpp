#include "MainWindow.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QTextCursor>

#include <iostream>

namespace {
const char kLogFilePath[] = "my_server_log_gui.txt";

QString formatState(SessionState state) {
    switch (state) {
        case SessionState::READY:
            return "ready";
        case SessionState::PLAYING:
            return "playing";
        case SessionState::PAUSED:
            return "paused";
        case SessionState::CLOSED:
            return "closed";
        case SessionState::INIT:
        default:
            return "init";
    }
}
} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setupUi();

    m_player = new RtspPlayer(this);
    connect(m_player, &RtspPlayer::frameReady, m_videoWidget, &VideoWidget::setFrame);
    connect(m_player, &RtspPlayer::error, this, &MainWindow::onPlayerError);

    m_statusTimer = new QTimer(this);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::refreshStatus);
    m_statusTimer->start(500);

    m_logTimer = new QTimer(this);
    connect(m_logTimer, &QTimer::timeout, this, &MainWindow::refreshLog);

    connect(m_startButton, &QPushButton::clicked, this, &MainWindow::onStartClicked);
    connect(m_stopButton, &QPushButton::clicked, this, &MainWindow::onStopClicked);

    refreshStatus();
}

MainWindow::~MainWindow() {
    stopServer();
}

void MainWindow::setupUi() {
    auto* central = new QWidget(this);
    auto* rootLayout = new QVBoxLayout();

    auto* topRow = new QHBoxLayout();
    m_ipEdit = new QLineEdit();
    m_ipEdit->setPlaceholderText("0.0.0.0");
    m_portEdit = new QLineEdit("8554");
    m_pathEdit = new QLineEdit("/live1");

    m_startButton = new QPushButton("Start Server");
    m_stopButton = new QPushButton("Stop Server");
    m_statusLabel = new QLabel("Status: stopped");

    topRow->addWidget(new QLabel("IP:"));
    topRow->addWidget(m_ipEdit);
    topRow->addWidget(new QLabel("Port:"));
    topRow->addWidget(m_portEdit);
    topRow->addWidget(new QLabel("Path:"));
    topRow->addWidget(m_pathEdit);
    topRow->addWidget(m_startButton);
    topRow->addWidget(m_stopButton);
    topRow->addWidget(m_statusLabel);

    m_connectionsTable = new QTableWidget(0, 3, this);
    m_connectionsTable->setHorizontalHeaderLabels({"Client IP", "Connected At", "URL"});
    m_connectionsTable->horizontalHeader()->setStretchLastSection(true);
    m_connectionsTable->verticalHeader()->setVisible(false);
    m_connectionsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_connectionsTable->setSelectionBehavior(QAbstractItemView::SelectRows);

    m_videoWidget = new VideoWidget(this);

    m_logView = new QPlainTextEdit(this);
    m_logView->setReadOnly(true);

    rootLayout->addLayout(topRow);
    rootLayout->addWidget(m_connectionsTable);
    rootLayout->addWidget(m_videoWidget);
    rootLayout->addWidget(m_logView);

    central->setLayout(rootLayout);
    setCentralWidget(central);
    setWindowTitle("RTSP Server");
    resize(980, 620);
}

void MainWindow::onStartClicked() {
    if (m_running) {
        return;
    }

    bool ok = false;
    const int port = m_portEdit->text().toInt(&ok);
    if (!ok || port <= 0 || port > 65535) {
        QMessageBox::warning(this, "Invalid Port", "Please enter a valid port.");
        return;
    }

    startServer();
}

void MainWindow::onStopClicked() {
    stopServer();
}

void MainWindow::startServer() {
    const QString ipText = m_ipEdit->text().trimmed();
    const QString pathText = m_pathEdit->text().trimmed();
    bool ok = false;
    const int port = m_portEdit->text().toInt(&ok);
    if (!ok) {
        return;
    }

    m_startFailed = false;
    m_running = true;
    startLogCapture();

    const std::string bind_ip = ipText.isEmpty() ? "0.0.0.0" : ipText.toStdString();
    const std::string stream_path = pathText.isEmpty() ? "" : pathText.toStdString();

    m_serverThread = std::thread([this, bind_ip, port, stream_path]() {
        if (!m_server.start(bind_ip, port, stream_path, &m_running)) {
            m_startFailed = true;
            m_running = false;
        }
    });

    m_captureThread = std::thread([this]() {
        CaptureOptions options;
        m_camera.startCaptureAndEncode(options, &m_running,
                                       [this](uint8_t* data, int size, uint32_t ts) {
                                           m_server.dispatchNalu(data, size, ts);
                                       });
    });

    const std::string preview_url = buildPreviewUrl();
    if (!preview_url.empty()) {
        QTimer::singleShot(500, this, [this, preview_url]() {
            if (m_player && m_running) {
                m_player->start(preview_url);
            }
        });
    }

    m_logTimer->start(500);
}

void MainWindow::stopServer() {
    if (!m_running && !m_serverThread.joinable() && !m_captureThread.joinable()) {
        return;
    }

    m_running = false;
    if (m_player) {
        m_player->stop();
        m_videoWidget->clear();
    }

    if (m_captureThread.joinable()) {
        m_captureThread.join();
    }
    if (m_serverThread.joinable()) {
        m_serverThread.join();
    }

    stopLogCapture();
    refreshStatus();
}

void MainWindow::refreshStatus() {
    if (m_startFailed) {
        m_statusLabel->setText("Status: error");
        return;
    }

    m_statusLabel->setText(m_running ? "Status: running" : "Status: stopped");
    refreshConnections();
}

void MainWindow::refreshConnections() {
    const auto infos = m_server.clientInfos();
    m_connectionsTable->setRowCount(static_cast<int>(infos.size()));

    for (int i = 0; i < static_cast<int>(infos.size()); ++i) {
        const auto& info = infos[static_cast<size_t>(i)];
        const qint64 ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   info.connected_at.time_since_epoch())
                                   .count();
        const QString when = QDateTime::fromMSecsSinceEpoch(ms).toString("yyyy-MM-dd HH:mm:ss");

        m_connectionsTable->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(info.ip)));
        m_connectionsTable->setItem(i, 1, new QTableWidgetItem(when));
        m_connectionsTable->setItem(i, 2, new QTableWidgetItem(QString::fromStdString(info.url)));
    }
}

void MainWindow::startLogCapture() {
    m_logView->clear();
    m_logFile.open(kLogFilePath, std::ios::out | std::ios::app);
    if (m_logFile.is_open()) {
        m_coutBuf = std::cout.rdbuf(m_logFile.rdbuf());
        m_cerrBuf = std::cerr.rdbuf(m_logFile.rdbuf());
        std::cout.setf(std::ios::unitbuf);
        std::cerr.setf(std::ios::unitbuf);
    }

    m_logReadFile.setFileName(kLogFilePath);
    m_logReadFile.open(QIODevice::ReadOnly);
    m_logOffset = 0;
}

void MainWindow::stopLogCapture() {
    if (m_coutBuf) {
        std::cout.rdbuf(m_coutBuf);
        m_coutBuf = nullptr;
    }
    if (m_cerrBuf) {
        std::cerr.rdbuf(m_cerrBuf);
        m_cerrBuf = nullptr;
    }

    if (m_logFile.is_open()) {
        m_logFile.close();
    }
    if (m_logReadFile.isOpen()) {
        m_logReadFile.close();
    }
    m_logTimer->stop();
}

void MainWindow::refreshLog() {
    if (!m_logReadFile.isOpen()) return;

    if (!m_logReadFile.seek(m_logOffset)) {
        return;
    }
    const QByteArray data = m_logReadFile.readAll();
    if (!data.isEmpty()) {
        m_logView->moveCursor(QTextCursor::End);
        m_logView->insertPlainText(QString::fromUtf8(data));
        m_logView->moveCursor(QTextCursor::End);
        m_logOffset = m_logReadFile.pos();
    }
}

void MainWindow::onPlayerError(const QString& message) {
    m_logView->appendPlainText("[PLAYER] " + message);
}

std::string MainWindow::buildPreviewUrl() const {
    const QString pathText = m_pathEdit->text().trimmed();
    QString path = pathText.isEmpty() ? "/" : pathText;
    if (!path.startsWith('/')) {
        path.prepend('/');
    }

    bool ok = false;
    const int port = m_portEdit->text().toInt(&ok);
    if (!ok || port <= 0) {
        return "";
    }

    return QString("rtsp://127.0.0.1:%1%2").arg(port).arg(path).toStdString();
}

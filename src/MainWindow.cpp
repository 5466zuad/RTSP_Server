#include "../include/MainWindow.h"
// 这里已经不需要 <cmath> 啦，假正弦波彻底被扫地出门了！

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), m_count(0), m_currentTraffic(0) {
    
    // 1. 准备图表数据
    m_series = new QLineSeries();
    m_chart = new QChart();
    m_chart->addSeries(m_series);
    m_chart->setTitle("RTSP Gateway Monitor | 真实流量监控"); 
    m_chart->legend()->hide();

    // 坐标轴设置
    QValueAxis *axisX = new QValueAxis;
    axisX->setRange(0, 100);
    axisX->setTitleText("Time (ticks)");
    m_chart->addAxis(axisX, Qt::AlignBottom);
    m_series->attachAxis(axisX);

    QValueAxis *axisY = new QValueAxis;
    // 💥 核心修改：量程放大到 200,000 字节！迎接海啸！
    axisY->setRange(0, 2000); 
    axisY->setTitleText("Network Traffic (Bytes / 50ms)");
    m_chart->addAxis(axisY, Qt::AlignLeft);
    m_series->attachAxis(axisY);

    m_chartView = new QChartView(m_chart);
    m_chartView->setRenderHint(QPainter::Antialiasing);

    // 2. 制造两个按钮实体
    m_btnStop = new QPushButton("Pause", this);
    m_btnStart = new QPushButton("Continue", this);

    // 3. 搞装修：垂直收纳盒
    QVBoxLayout *layout = new QVBoxLayout;
    layout->addWidget(m_chartView); 
    layout->addWidget(m_btnStop);  
    layout->addWidget(m_btnStart); 

    // 4. 固定在窗口中心
    QWidget *centerWidget = new QWidget;  
    centerWidget->setLayout(layout);      
    this->setCentralWidget(centerWidget); 
    this->resize(800, 600); 

    // 5. 搞定定时器和连线
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &MainWindow::updateData);
    
    // 🔌 点击暂停 -> 停图表 + 发送断流指令！
    connect(m_btnStop, &QPushButton::clicked, [this](){
        m_timer->stop();
        emit requestPauseServer(); 
    });
    
    // 🔌 点击开始 -> 动图表 + 发送复活指令！
    connect(m_btnStart, &QPushButton::clicked, [this](){
        m_timer->start(50); 
        emit requestResumeServer(); 
    });

    m_timer->start(50); // 一上来默认先跑起来
}

MainWindow::~MainWindow() {}

// 💥 新增：把后台发包的字节数攒进储钱罐
void MainWindow::addTraffic(int bytes) {
    m_currentTraffic += bytes; 
}

// 💥 修改：用真实数据画图，画完就清空！
void MainWindow::updateData() {
    static int x = 0; 
    if (x > 100) {
        m_series->clear();
        x = 0;
    }
    
    // 拔掉假波浪，把储钱罐里的真金白银画上去！
    m_series->append(x++, m_currentTraffic);

    // 🛑 核心绝杀：画完这个点，立刻把罐子清零，准备迎接下一个 50ms
    m_currentTraffic = 0; 

    m_count++;
    QString title = QString("RTSP Gateway : %1").arg(m_count);
    this->setWindowTitle(title); 
}
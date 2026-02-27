#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>
#include <QTimer>
#include <QPushButton> 
#include <QVBoxLayout> 

QT_CHARTS_USE_NAMESPACE

class MainWindow : public QMainWindow {
    Q_OBJECT 

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

signals:
    // 📢 界面向后台发出的“圣旨”
    void requestPauseServer(); 
    void requestResumeServer(); 

public slots:
    // 📢 接收后台传来的真实流量数据
    void addTraffic(int bytes);

private slots:
    void updateData();

private:
    QChart *m_chart;
    QLineSeries *m_series;
    QChartView *m_chartView;
    QTimer *m_timer;
    int m_count; 
    
    QPushButton *m_btnStop;  
    QPushButton *m_btnStart; 

    // 🗃️ 储钱罐：用来攒 50ms 内的所有流量
    int m_currentTraffic; 
};

#endif // MAINWINDOW_H
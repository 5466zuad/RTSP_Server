#pragma once

#include <QImage>
#include <QMutex>
#include <QWidget>

class VideoWidget : public QWidget {
    Q_OBJECT

public:
    explicit VideoWidget(QWidget* parent = nullptr);

public slots:
    void setFrame(const QImage& frame);
    void clear();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage m_frame;
    QMutex m_mutex;
};

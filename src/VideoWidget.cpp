#include "VideoWidget.h"

#include <QPainter>
#include <QMutexLocker>

VideoWidget::VideoWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(240);
    setAutoFillBackground(true);
}

void VideoWidget::setFrame(const QImage& frame) {
    {
        QMutexLocker locker(&m_mutex);
        m_frame = frame;
    }
    update();
}

void VideoWidget::clear() {
    {
        QMutexLocker locker(&m_mutex);
        m_frame = QImage();
    }
    update();
}

void VideoWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    QImage frame;
    {
        QMutexLocker locker(&m_mutex);
        frame = m_frame;
    }

    if (frame.isNull()) {
        return;
    }

    const QSize target = frame.size().scaled(size(), Qt::KeepAspectRatio);
    const QRect area((width() - target.width()) / 2,
                     (height() - target.height()) / 2,
                     target.width(),
                     target.height());
    painter.drawImage(area, frame);
}

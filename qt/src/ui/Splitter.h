#pragma once

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSplitter>

namespace wsc {

class Splitter : public QSplitter {
public:
    using QSplitter::QSplitter;

protected:
    QSplitterHandle *createHandle() override
    {
        return new Handle(orientation(), this);
    }

private:
    static constexpr qreal collapseThreshold = 0.2;

    class Handle : public QSplitterHandle {
    public:
        Handle(Qt::Orientation orientation, Splitter *splitter)
            : QSplitterHandle(orientation, splitter)
        {
            qApp->installEventFilter(this);
        }

    protected:
        bool eventFilter(QObject *object, QEvent *event) override
        {
            if (m_dragging && event->type() == QEvent::KeyPress &&
                static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
                m_cancelled = true;
                splitter()->setSizes(m_originalSizes);
                return true;
            }
            return QSplitterHandle::eventFilter(object, event);
        }

        void mousePressEvent(QMouseEvent *event) override
        {
            if (event->button() == Qt::LeftButton) {
                m_originalSizes = splitter()->sizes();
                m_dragging = true;
                m_cancelled = false;
            }
            QSplitterHandle::mousePressEvent(event);
        }

        void mouseMoveEvent(QMouseEvent *event) override
        {
            if (!m_cancelled)
                QSplitterHandle::mouseMoveEvent(event);
        }

        void mouseReleaseEvent(QMouseEvent *event) override
        {
            QSplitterHandle::mouseReleaseEvent(event);
            if (event->button() != Qt::LeftButton || !m_dragging)
                return;
            m_dragging = false;
            if (m_cancelled) {
                splitter()->setSizes(m_originalSizes);
                return;
            }
            if (splitter()->count() != 2)
                return;
            const QPoint point = splitter()->mapFromGlobal(event->globalPosition().toPoint());
            const int extent = orientation() == Qt::Horizontal ? splitter()->width() : splitter()->height();
            if (extent <= 0)
                return;
            const qreal ratio = qreal(orientation() == Qt::Horizontal ? point.x() : point.y()) / extent;
            if (ratio < collapseThreshold)
                splitter()->setSizes({0, extent});
            else if (ratio > 1.0 - collapseThreshold)
                splitter()->setSizes({extent, 0});
        }

        void mouseDoubleClickEvent(QMouseEvent *event) override
        {
            if (event->button() == Qt::LeftButton && splitter()->count() == 2) {
                m_dragging = false;
                const int extent = orientation() == Qt::Horizontal ? splitter()->width() : splitter()->height();
                splitter()->setSizes({extent / 2, extent / 2});
                event->accept();
                return;
            }
            QSplitterHandle::mouseDoubleClickEvent(event);
        }

    private:
        QList<int> m_originalSizes;
        bool m_dragging = false;
        bool m_cancelled = false;
    };
};

} // namespace wsc

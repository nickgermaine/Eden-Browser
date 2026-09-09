#include "core/window/windowframe.h"

#include <QEvent>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>

namespace eden::core {

    WindowFrame::WindowFrame(QObject *parent)
        : QObject(parent) {}

    QQuickWindow *WindowFrame::window() const {
        return m_window;
    }

    void WindowFrame::setWindow(QQuickWindow *window) {
        if (m_window == window) {
            return;
        }
        if (m_window) {
            m_window->removeEventFilter(this);
        }
        m_window = window;
        if (m_window) {
            m_window->setMinimumSize(QSize(900, 600));
            m_window->installEventFilter(this);
        }
        emit windowChanged();
    }

    bool WindowFrame::eventFilter(QObject *watched, QEvent *event) {
        QQuickWindow *window = m_window;
        if (!window || watched != window) {
            return QObject::eventFilter(watched, event);
        }
        if (event->type() == QEvent::ParentAboutToChange || event->type() == QEvent::Destroy) {
            window->removeEventFilter(this);
            m_window.clear();
            return QObject::eventFilter(watched, event);
        }
        if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::WindowDeactivate) {
            return QObject::eventFilter(watched, event);
        }
        QQuickItem *focusItem = window->activeFocusItem();
        if (!focusItem) {
            return QObject::eventFilter(watched, event);
        }
        bool clearFocus = event->type() == QEvent::WindowDeactivate;
        if (event->type() == QEvent::MouseButtonPress) {
            const auto *mouseEvent = static_cast<QMouseEvent *>(event);
            const QPointF localPosition = focusItem->mapFromScene(mouseEvent->position());
            clearFocus = !focusItem->contains(localPosition);
        }
        if (clearFocus && window->contentItem()) {
            focusItem->setFocus(false, Qt::MouseFocusReason);
            window->contentItem()->forceActiveFocus(Qt::MouseFocusReason);
        }
        return QObject::eventFilter(watched, event);
    }

    bool WindowFrame::startSystemMove() {
        return m_window && m_window->startSystemMove();
    }

    bool WindowFrame::startSystemResize(int edges) {
        if (!m_window || m_window->visibility() == QWindow::Maximized) {
            return false;
        }
        return m_window->startSystemResize(Qt::Edges(edges));
    }

    void WindowFrame::toggleMaximized() {
        if (!m_window) {
            return;
        }
        if (m_window->visibility() == QWindow::Maximized) {
            m_window->showNormal();
        } else {
            m_window->showMaximized();
        }
    }

    void WindowFrame::minimize() {
        if (m_window) {
            m_window->showMinimized();
        }
    }

    void WindowFrame::close() {
        if (m_window) {
            m_window->close();
        }
    }

}

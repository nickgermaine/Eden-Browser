#include "core/window/tabstripnavigator.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QTimer>
#include <QVariant>

#include <algorithm>

namespace eden::core {

    TabStripNavigator::TabStripNavigator(QObject *parent)
        : QObject(parent) {}

    QQuickItem *TabStripNavigator::view() const {
        return m_view;
    }

    void TabStripNavigator::setView(QQuickItem *view) {
        if (m_view == view) {
            return;
        }
        m_view = view;
        emit viewChanged();
    }

    void TabStripNavigator::slide(qreal distance) {
        if (!m_view) {
            return;
        }
        const qreal origin = m_view->property("originX").toReal();
        const qreal contentWidth = m_view->property("contentWidth").toReal();
        const qreal viewportWidth = m_view->width();
        const qreal contentX = m_view->property("contentX").toReal();
        const qreal maximum = std::max(origin, origin + contentWidth - viewportWidth);
        m_view->setProperty("contentX", std::clamp(contentX + distance, origin, maximum));
    }

    void TabStripNavigator::snap(int direction) {
        if (!m_view) {
            return;
        }
        const int count = m_view->property("count").toInt();
        if (count == 0) {
            return;
        }
        const qreal contentX = m_view->property("contentX").toReal();
        const qreal middleY = m_view->height() / 2;
        if (direction < 0) {
            const int firstVisible = std::max(0, indexAt(contentX + 1, middleY));
            positionViewAtIndex(std::max(0, firstVisible - 1), Beginning);
            return;
        }
        const int visibleIndex = indexAt(contentX + m_view->width() - 1, middleY);
        const int lastVisible = visibleIndex < 0 ? count - 1 : visibleIndex;
        positionViewAtIndex(std::min(count - 1, lastVisible + 1), End);
    }

    void TabStripNavigator::reveal(int index) {
        if (m_view && index >= 0) {
            positionViewAtIndex(index, Contain);
        }
    }

    void TabStripNavigator::scheduleRelayout(bool overflowing, int currentIndex) {
        m_overflowing = overflowing;
        m_currentIndex = currentIndex;
        if (m_relayoutScheduled) {
            return;
        }
        m_relayoutScheduled = true;
        QTimer::singleShot(0, QCoreApplication::instance(), [navigator = QPointer<TabStripNavigator>(this)] {
            if (navigator) {
                navigator->relayout();
            }
        });
    }

    int TabStripNavigator::destinationForDrag(int sourceIndex, qreal centerX) const {
        if (!m_view) {
            return sourceIndex;
        }
        const int count = m_view->property("count").toInt();
        if (count == 0) {
            return -1;
        }
        const int candidate = indexAt(centerX, m_view->height() / 2);
        if (candidate >= 0 && candidate < count) {
            return candidate;
        }
        const qreal origin = m_view->property("originX").toReal();
        const qreal contentWidth = m_view->property("contentWidth").toReal();
        if (centerX <= origin) {
            return 0;
        }
        if (centerX >= origin + contentWidth) {
            return count - 1;
        }
        return std::clamp(sourceIndex, 0, count - 1);
    }

    int TabStripNavigator::indexAt(qreal x, qreal y) const {
        int index = -1;
        if (m_view) {
            QMetaObject::invokeMethod(
                m_view,
                "indexAt",
                Qt::DirectConnection,
                Q_RETURN_ARG(int, index),
                Q_ARG(qreal, x),
                Q_ARG(qreal, y)
            );
        }
        return index;
    }

    void TabStripNavigator::relayout() {
        m_relayoutScheduled = false;
        if (!m_view) {
            return;
        }
        QMetaObject::invokeMethod(m_view, "forceLayout", Qt::DirectConnection);
        const qreal origin = m_view->property("originX").toReal();
        const qreal maximum = std::max(origin, origin + m_view->property("contentWidth").toReal() - m_view->width());
        if (!m_overflowing || maximum <= origin) {
            m_view->setProperty("contentX", origin);
            return;
        }
        const qreal contentX = m_view->property("contentX").toReal();
        m_view->setProperty("contentX", std::clamp(contentX, origin, maximum));
        if (m_currentIndex >= 0) {
            positionViewAtIndex(m_currentIndex, Contain);
        }
    }

    void TabStripNavigator::positionViewAtIndex(int index, PositionMode mode) {
        if (m_view) {
            QMetaObject::invokeMethod(
                m_view,
                "positionViewAtIndex",
                Qt::DirectConnection,
                Q_ARG(int, index),
                Q_ARG(int, static_cast<int>(mode))
            );
        }
    }

}

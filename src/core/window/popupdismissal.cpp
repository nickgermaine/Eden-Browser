#include "core/window/popupdismissal.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaEnum>
#include <QMetaProperty>
#include <QMouseEvent>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTouchEvent>
#include <QWheelEvent>
#include <algorithm>

namespace eden::core {

    namespace {

        struct VisiblePopup {
            QPointer<QObject> popup;
            QPointer<QQuickItem> item;
            bool modal = false;
            bool closesOnOutsidePress = false;
        };

        bool closesOnOutsidePress(const QObject *popup) {
            const QMetaObject *metaObject = popup->metaObject();
            const int enumerator = metaObject->indexOfEnumerator("ClosePolicy");
            const int property = metaObject->indexOfProperty("closePolicy");
            if (enumerator < 0 || property < 0) {
                return false;
            }
            bool known = false;
            const int flag = metaObject->enumerator(enumerator).keyToValue("CloseOnPressOutside", &known);
            bool readable = false;
            const int policy = metaObject->property(property).read(popup).toInt(&readable);
            return known && readable && (policy & flag) != 0;
        }

        bool isShown(const VisiblePopup &entry) {
            return entry.popup && entry.item && entry.item->isVisible() && entry.popup->property("visible").toBool();
        }

        void appendShownPopups(const QList<QQuickItem *> &topMostFirst, QList<VisiblePopup> &popups) {
            for (QQuickItem *item : topMostFirst) {
                QObject *popup = item->parent();
                if (!item->isVisible() || !item->inherits("QQuickPopupItem") || !popup ||
                    !popup->inherits("QQuickPopup") || !popup->property("visible").toBool()) {
                    continue;
                }
                popups.append({popup, item, popup->property("modal").toBool(), closesOnOutsidePress(popup)});
            }
        }

        QList<VisiblePopup> visiblePopups(QQuickWindow *window) {
            QQuickItem *content = window ? window->contentItem() : nullptr;
            if (!content) {
                return {};
            }
            QQuickItem *overlay = nullptr;
            QList<QQuickItem *> windowPopupItems;
            const QList<QQuickItem *> contentChildren = content->childItems();
            for (auto child = contentChildren.crbegin(); child != contentChildren.crend(); ++child) {
                if ((*child)->inherits("QQuickOverlay")) {
                    overlay = *child;
                } else if ((*child)->inherits("QQuickPopupItem")) {
                    windowPopupItems.append(*child);
                }
            }
            QList<VisiblePopup> popups;
            if (overlay && overlay->isVisible()) {
                QList<QQuickItem *> children = overlay->childItems();
                std::stable_sort(children.begin(), children.end(), [](const QQuickItem *left, const QQuickItem *right) {
                    return left->z() < right->z();
                });
                std::reverse(children.begin(), children.end());
                appendShownPopups(children, popups);
            }
            appendShownPopups(windowPopupItems, popups);
            return popups;
        }

        bool containsAny(QQuickItem *item, const QList<QPointF> &scenePoints) {
            return std::any_of(scenePoints.cbegin(), scenePoints.cend(), [item](const QPointF &point) {
                return item->contains(item->mapFromScene(point));
            });
        }

        void close(const QPointer<QObject> &popup) {
            if (popup) {
                QMetaObject::invokeMethod(popup.data(), "close", Qt::DirectConnection);
            }
        }

        bool isTransientAncestor(const QWindow *ancestor, const QWindow *window) {
            for (const QWindow *parent = window ? window->transientParent() : nullptr; parent;
                 parent = parent->transientParent()) {
                if (parent == ancestor) {
                    return true;
                }
            }
            return false;
        }

        QList<QPointF> pressedPoints(const QTouchEvent *event) {
            QList<QPointF> points;
            for (const QEventPoint &point : event->points()) {
                if (point.state() == QEventPoint::Pressed) {
                    points.append(point.scenePosition());
                }
            }
            return points;
        }

    }

    PopupDismissal::PopupDismissal(QObject *parent)
        : QObject(parent) {
        QCoreApplication::instance()->installEventFilter(this);
    }

    PopupDismissal::~PopupDismissal() {
        if (QCoreApplication *application = QCoreApplication::instance()) {
            application->removeEventFilter(this);
        }
    }

    bool
    PopupDismissal::dismissOutside(QQuickWindow *window, const QList<QPointF> &scenePoints, Interaction interaction) {
        if (scenePoints.isEmpty()) {
            return false;
        }
        for (const VisiblePopup &entry : visiblePopups(window)) {
            if (!isShown(entry)) {
                continue;
            }
            if (containsAny(entry.item, scenePoints)) {
                return false;
            }
            if (entry.modal && interaction == Interaction::Wheel) {
                return false;
            }
            if (entry.closesOnOutsidePress) {
                close(entry.popup);
            }
            if (entry.modal) {
                return entry.closesOnOutsidePress;
            }
        }
        return false;
    }

    void PopupDismissal::dismissTransient(QQuickWindow *window) {
        for (const VisiblePopup &entry : visiblePopups(window)) {
            if (!entry.modal && entry.closesOnOutsidePress && isShown(entry)) {
                close(entry.popup);
            }
        }
    }

    void PopupDismissal::dismissOtherWindows(QQuickWindow *window) {
        QList<QPointer<QQuickWindow>> others;
        const QWindowList windows = QGuiApplication::topLevelWindows();
        for (QWindow *other : windows) {
            auto *quickWindow = qobject_cast<QQuickWindow *>(other);
            if (quickWindow && other != window && !isTransientAncestor(other, window)) {
                others.append(quickWindow);
            }
        }
        for (const QPointer<QQuickWindow> &other : std::as_const(others)) {
            if (other) {
                dismissTransient(other);
            }
        }
    }

    bool PopupDismissal::eventFilter(QObject *watched, QEvent *event) {
        const QEvent::Type type = event->type();
        if (type != QEvent::MouseButtonPress && type != QEvent::TouchBegin && type != QEvent::TouchUpdate &&
            type != QEvent::Wheel && type != QEvent::WindowDeactivate) {
            return QObject::eventFilter(watched, event);
        }
        QPointer<QQuickWindow> window = qobject_cast<QQuickWindow *>(watched);
        if (!window) {
            return QObject::eventFilter(watched, event);
        }
        QList<QPointF> points;
        Interaction interaction = Interaction::Press;
        switch (type) {
        case QEvent::MouseButtonPress:
            points.append(static_cast<QMouseEvent *>(event)->scenePosition());
            break;
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
            points = pressedPoints(static_cast<QTouchEvent *>(event));
            break;
        case QEvent::Wheel:
            points.append(static_cast<QWheelEvent *>(event)->scenePosition());
            interaction = Interaction::Wheel;
            break;
        default:
            if (!isTransientAncestor(window, QGuiApplication::focusWindow())) {
                dismissTransient(window);
            }
            return QObject::eventFilter(watched, event);
        }
        if (points.isEmpty()) {
            return QObject::eventFilter(watched, event);
        }
        dismissOtherWindows(window);
        if (!window) {
            event->accept();
            return true;
        }
        if (dismissOutside(window, points, interaction)) {
            event->accept();
            return true;
        }
        return QObject::eventFilter(watched, event);
    }

}

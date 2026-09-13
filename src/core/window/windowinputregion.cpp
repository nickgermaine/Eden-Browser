#include "core/window/windowinputregion.h"

#include <QEvent>
#include <QGuiApplication>
#include <QPainterPath>
#include <QPlatformSurfaceEvent>
#include <algorithm>
#include <cmath>

#if defined(EDEN_HAS_XCB_SHAPE) && QT_CONFIG(xcb)
#include <QtGui/qguiapplication_platform.h>
#include <xcb/shape.h>
#endif

namespace eden::core {

    WindowInputRegion::WindowInputRegion(QObject *parent)
        : QObject(parent) {}

    WindowInputRegion::~WindowInputRegion() {
        clearRegion();
    }

    QWindow *WindowInputRegion::window() const {
        return m_window;
    }

    void WindowInputRegion::setWindow(QWindow *window) {
        if (m_window == window) {
            return;
        }
        if (m_window) {
            clearRegion();
            m_window->removeEventFilter(this);
            disconnect(m_window, nullptr, this, nullptr);
        }
        m_window = window;
        if (m_window) {
            m_window->installEventFilter(this);
            connect(m_window, &QWindow::widthChanged, this, &WindowInputRegion::updateRegion);
            connect(m_window, &QWindow::heightChanged, this, &WindowInputRegion::updateRegion);
            connect(m_window, &QWindow::screenChanged, this, &WindowInputRegion::updateRegion);
            connect(m_window, &QObject::destroyed, this, &WindowInputRegion::windowChanged);
        }
        updateRegion();
        emit windowChanged();
    }

    QRectF WindowInputRegion::rect() const {
        return m_rect;
    }

    void WindowInputRegion::setRect(const QRectF &rect) {
        if (m_rect == rect || !std::isfinite(rect.x()) || !std::isfinite(rect.y()) || !std::isfinite(rect.width()) ||
            !std::isfinite(rect.height())) {
            return;
        }
        m_rect = rect;
        updateRegion();
        emit rectChanged();
    }

    qreal WindowInputRegion::radius() const {
        return m_radius;
    }

    void WindowInputRegion::setRadius(qreal radius) {
        if (!std::isfinite(radius)) {
            return;
        }
        radius = std::max(qreal(0), radius);
        if (m_radius == radius) {
            return;
        }
        m_radius = radius;
        updateRegion();
        emit radiusChanged();
    }

    QRegion WindowInputRegion::inputRegion(qreal scale) const {
        if (!m_window || m_rect.isEmpty() || !std::isfinite(scale) || scale <= 0) {
            return {};
        }
        const QRectF bounds(m_rect.topLeft() * scale, m_rect.size() * scale);
        const qreal radius = std::min({m_radius * scale, bounds.width() / 2, bounds.height() / 2});
        QPainterPath outline;
        outline.addRoundedRect(bounds, radius, radius);
        return QRegion(outline.toFillPolygon().toPolygon()) & QRegion(QRect(QPoint(), (m_window->size() * scale)));
    }

    bool WindowInputRegion::applyX11Region(const QRegion &region, bool reset) {
#if defined(EDEN_HAS_XCB_SHAPE) && QT_CONFIG(xcb)
        if (QGuiApplication::platformName() != QStringLiteral("xcb")) {
            return false;
        }
        auto *native = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
        if (!native || !m_window || !m_window->handle()) {
            return true;
        }
        xcb_connection_t *connection = native->connection();
        const auto *extension = xcb_get_extension_data(connection, &xcb_shape_id);
        if (!extension || !extension->present) {
            return true;
        }
        const auto id = static_cast<xcb_window_t>(m_window->winId());
        if (reset) {
            xcb_shape_mask(connection, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, id, 0, 0, XCB_NONE);
        } else {
            QList<xcb_rectangle_t> rectangles;
            rectangles.reserve(region.rectCount());
            for (const QRect &rect : region) {
                rectangles.append(
                    {static_cast<int16_t>(rect.x()),
                     static_cast<int16_t>(rect.y()),
                     static_cast<uint16_t>(rect.width()),
                     static_cast<uint16_t>(rect.height())}
                );
            }
            xcb_shape_rectangles(
                connection,
                XCB_SHAPE_SO_SET,
                XCB_SHAPE_SK_INPUT,
                XCB_CLIP_ORDERING_YX_BANDED,
                id,
                0,
                0,
                static_cast<uint32_t>(rectangles.size()),
                rectangles.constData()
            );
        }
        xcb_flush(connection);
        return true;
#else
        Q_UNUSED(region)
        Q_UNUSED(reset)
        return false;
#endif
    }

    void WindowInputRegion::updateRegion() {
        if (!m_window) {
            return;
        }
        if (applyX11Region(inputRegion(m_window->devicePixelRatio()), m_rect.isEmpty())) {
            return;
        }
        if (QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) {
            const QRegion region = inputRegion();
            m_window->setMask(!m_rect.isEmpty() && region.isEmpty() ? QRegion(QRect(-1, -1, 1, 1)) : region);
        }
    }

    void WindowInputRegion::clearRegion() {
        if (!m_window || applyX11Region({}, true)) {
            return;
        }
        if (QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) {
            m_window->setMask({});
        }
    }

    bool WindowInputRegion::eventFilter(QObject *watched, QEvent *event) {
        if (watched == m_window) {
            if (event->type() == QEvent::PlatformSurface) {
                if (static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() ==
                    QPlatformSurfaceEvent::SurfaceCreated) {
                    updateRegion();
                }
            } else if (event->type() == QEvent::Show || event->type() == QEvent::DevicePixelRatioChange) {
                updateRegion();
            }
        }
        return QObject::eventFilter(watched, event);
    }

}

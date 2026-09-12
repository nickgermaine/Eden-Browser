#include "platform/waylandglibeventdispatcher.h"

#include <QAbstractEventDispatcher>
#include <QMetaObject>
#include <QSocketNotifier>
#include <QTimer>

#include <algorithm>

namespace eden::platform {

    WaylandGlibEventDispatcher::WaylandGlibEventDispatcher(QObject *parent, GMainContext *context)
        : QObject(parent),
          m_context(g_main_context_ref(context ? context : g_main_context_default())),
          m_deadlineTimer(new QTimer(this)) {
        if (!g_main_context_acquire(m_context)) {
            qCritical("Wayland GLib context is owned by another thread");
            return;
        }
        m_contextOwned = true;
        m_deadlineTimer->setSingleShot(true);
        m_deadlineTimer->setTimerType(Qt::PreciseTimer);
        connect(m_deadlineTimer, &QTimer::timeout, this, &WaylandGlibEventDispatcher::dispatchReadySources);
        if (QAbstractEventDispatcher *eventDispatcher = QAbstractEventDispatcher::instance()) {
            connect(
                eventDispatcher,
                &QAbstractEventDispatcher::aboutToBlock,
                this,
                &WaylandGlibEventDispatcher::refreshWaitState
            );
        }
        refreshWaitState();
    }

    WaylandGlibEventDispatcher::~WaylandGlibEventDispatcher() {
        m_deadlineTimer->stop();
        for (QSocketNotifier *notifier : m_notifiers) {
            delete notifier;
        }
        m_notifiers.clear();
        if (m_contextOwned) {
            g_main_context_release(m_context);
        }
        g_main_context_unref(m_context);
    }

    void WaylandGlibEventDispatcher::dispatchReadySources() {
        if (!m_contextOwned || m_dispatching) {
            return;
        }
        m_dispatchQueued = false;
        m_dispatching = true;
        m_deadlineTimer->stop();
        for (QSocketNotifier *notifier : m_notifiers) {
            notifier->setEnabled(false);
        }
        for (int iteration = 0; iteration < 64; ++iteration) {
            if (!g_main_context_iteration(m_context, FALSE)) {
                break;
            }
        }
        m_dispatching = false;
        refreshWaitState();
    }

    void WaylandGlibEventDispatcher::queueDispatch() {
        if (!m_contextOwned || m_dispatchQueued || m_dispatching) {
            return;
        }
        m_dispatchQueued = true;
        QMetaObject::invokeMethod(this, [this] { dispatchReadySources(); }, Qt::QueuedConnection);
    }

    void WaylandGlibEventDispatcher::refreshWaitState() {
        if (!m_contextOwned || m_dispatching) {
            return;
        }

        int priority = G_PRIORITY_DEFAULT;
        const bool ready = g_main_context_prepare(m_context, &priority);
        int timeoutMilliseconds = -1;
        int descriptorCount = g_main_context_query(m_context, priority, &timeoutMilliseconds, nullptr, 0);
        std::vector<GPollFD> descriptors(static_cast<std::size_t>(std::max(descriptorCount, 0)));
        while (descriptorCount > 0) {
            const int required = g_main_context_query(
                m_context,
                priority,
                &timeoutMilliseconds,
                descriptors.data(),
                static_cast<int>(descriptors.size())
            );
            if (required <= static_cast<int>(descriptors.size())) {
                descriptorCount = required;
                descriptors.resize(static_cast<std::size_t>(required));
                break;
            }
            descriptors.resize(static_cast<std::size_t>(required));
        }
        const bool descriptorsChanged = descriptors.size() != m_descriptors.size() ||
                                        !std::equal(
                                            descriptors.cbegin(),
                                            descriptors.cend(),
                                            m_descriptors.cbegin(),
                                            [](const GPollFD &left, const GPollFD &right) {
                                                return left.fd == right.fd && left.events == right.events;
                                            }
                                        );
        if (descriptorsChanged) {
            retireNotifiers();
            m_descriptors = descriptors;
            for (const GPollFD &descriptor : m_descriptors) {
                const auto addNotifier = [this, descriptor](QSocketNotifier::Type type) {
                    auto *notifier = new QSocketNotifier(descriptor.fd, type, this);
                    connect(notifier, &QSocketNotifier::activated, this, [this] { dispatchReadySources(); });
                    m_notifiers.push_back(notifier);
                };
                if (descriptor.events & (G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
                    addNotifier(QSocketNotifier::Read);
                }
                if (descriptor.events & G_IO_OUT) {
                    addNotifier(QSocketNotifier::Write);
                }
                if (!(descriptor.events & (G_IO_IN | G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL))) {
                    addNotifier(QSocketNotifier::Exception);
                }
            }
        }
        for (QSocketNotifier *notifier : m_notifiers) {
            notifier->setEnabled(true);
        }

        if (ready || timeoutMilliseconds == 0) {
            m_deadlineTimer->stop();
            queueDispatch();
            return;
        }
        if (timeoutMilliseconds > 0) {
            m_deadlineTimer->start(timeoutMilliseconds);
        } else {
            m_deadlineTimer->stop();
        }
    }

    void WaylandGlibEventDispatcher::retireNotifiers() {
        for (QSocketNotifier *notifier : m_notifiers) {
            notifier->setEnabled(false);
            notifier->deleteLater();
        }
        m_notifiers.clear();
    }

}

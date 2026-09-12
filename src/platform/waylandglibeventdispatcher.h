#pragma once

#include <QObject>

#include <glib.h>

#include <vector>

class QSocketNotifier;
class QTimer;

namespace eden::platform {

    class WaylandGlibEventDispatcher final : public QObject {
      public:
        explicit WaylandGlibEventDispatcher(QObject *parent = nullptr, GMainContext *context = nullptr);
        ~WaylandGlibEventDispatcher() override;

      private:
        void dispatchReadySources();
        void queueDispatch();
        void refreshWaitState();
        void retireNotifiers();

        GMainContext *m_context;
        QTimer *m_deadlineTimer;
        std::vector<GPollFD> m_descriptors;
        std::vector<QSocketNotifier *> m_notifiers;
        bool m_contextOwned = false;
        bool m_dispatchQueued = false;
        bool m_dispatching = false;
    };

}

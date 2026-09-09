#include "engine/cef/cefuibridge.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QMutex>
#include <QObject>
#include <QThread>

#include <deque>
#include <utility>

namespace eden::engine::cef {

    struct CefUiQueue {
        QMutex mutex;
        std::deque<std::function<void()>> tasks;
        quint64 generation = 0;
        bool accepting = true;
        bool scheduled = false;
    };

    static CefUiQueue &uiQueue() {
        static CefUiQueue queue;
        return queue;
    }

    static void drainUiQueue(quint64 generation);

    static bool scheduleDrain(quint64 generation) {
        return QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [generation] { drainUiQueue(generation); },
            Qt::QueuedConnection
        );
    }

    static void drainUiQueue(quint64 generation) {
        CefUiBridge::assertOnUiThread();
        CefUiQueue &queue = uiQueue();
        std::deque<std::function<void()>> canceled;
        {
            const QMutexLocker lock(&queue.mutex);
            if (queue.generation != generation) {
                return;
            }
            if (queue.tasks.empty()) {
                queue.scheduled = false;
                return;
            }
            queue.scheduled = scheduleDrain(generation);
            if (!queue.scheduled) {
                canceled.swap(queue.tasks);
                return;
            }
        }
        for (int processed = 0; processed < 64; ++processed) {
            std::function<void()> task;
            {
                const QMutexLocker lock(&queue.mutex);
                if (queue.generation != generation || queue.tasks.empty()) {
                    return;
                }
                task = std::move(queue.tasks.front());
                queue.tasks.pop_front();
            }
            task();
        }
    }

    bool CefUiBridge::runOnUiThread(std::function<void()> task) {
        if (!QCoreApplication::instance() || QCoreApplication::closingDown() || !task) {
            return false;
        }
        CefUiQueue &queue = uiQueue();
        const QMutexLocker lock(&queue.mutex);
        if (!queue.accepting) {
            return false;
        }
        if (!queue.scheduled) {
            queue.scheduled = scheduleDrain(queue.generation);
            if (!queue.scheduled) {
                return false;
            }
        }
        queue.tasks.push_back(std::move(task));
        return true;
    }

    void CefUiBridge::resumeTasks() {
        assertOnUiThread();
        CefUiQueue &queue = uiQueue();
        const QMutexLocker lock(&queue.mutex);
        queue.accepting = true;
    }

    void CefUiBridge::beginShutdown() {
        assertOnUiThread();
        CefUiQueue &queue = uiQueue();
        {
            const QMutexLocker lock(&queue.mutex);
            queue.accepting = false;
        }
        cancelPendingTasks();
    }

    void CefUiBridge::cancelPendingTasks() {
        assertOnUiThread();
        CefUiQueue &queue = uiQueue();
        std::deque<std::function<void()>> canceled;
        {
            const QMutexLocker lock(&queue.mutex);
            canceled.swap(queue.tasks);
            queue.scheduled = false;
            ++queue.generation;
        }
    }

    bool CefUiBridge::isOnUiThread() {
        const QCoreApplication *application = QCoreApplication::instance();
        return application && QThread::currentThread() == application->thread();
    }

    void CefUiBridge::assertOnUiThread(const QObject *stateOwner) {
        const QCoreApplication *application = QCoreApplication::instance();
        Q_ASSERT_X(application, "CefUiBridge::assertOnUiThread", "Qt application is unavailable");
        Q_ASSERT_X(
            QThread::currentThread() == application->thread(),
            "CefUiBridge::assertOnUiThread",
            "CEF callback accessed QObject state outside the Qt UI thread"
        );
        Q_ASSERT_X(
            !stateOwner || stateOwner->thread() == QThread::currentThread(),
            "CefUiBridge::assertOnUiThread",
            "CEF callback accessed a QObject owned by another thread"
        );
    }

}

#include "engine/cef/cefuibridge.h"

#include <QCoreApplication>
#include <QEvent>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QThread>

#include <utility>

namespace eden::engine::cef {

    class CefUiTask;

    static QMutex &pendingTaskMutex() {
        static QMutex mutex;
        return mutex;
    }

    static QList<QPointer<CefUiTask>> &pendingTasks() {
        static QList<QPointer<CefUiTask>> tasks;
        return tasks;
    }

    static bool &acceptingTasks() {
        static bool accepting = true;
        return accepting;
    }

    class CefUiTask final : public QObject {
      public:
        explicit CefUiTask(std::function<void()> task)
            : m_task(std::move(task)) {}

      protected:
        bool event(QEvent *event) override {
            if (event->type() != QEvent::User) {
                return QObject::event(event);
            }
            CefUiBridge::assertOnUiThread();
            std::function<void()> task;
            {
                const QMutexLocker lock(&pendingTaskMutex());
                pendingTasks().removeAll(this);
                task = std::move(m_task);
            }
            delete this;
            task();
            return true;
        }

      private:
        std::function<void()> m_task;
    };

    bool CefUiBridge::runOnUiThread(std::function<void()> task) {
        QCoreApplication *application = QCoreApplication::instance();
        if (!application || !task) {
            return false;
        }
        CefUiTask *uiTask = new CefUiTask(std::move(task));
        {
            const QMutexLocker lock(&pendingTaskMutex());
            if (!acceptingTasks()) {
                delete uiTask;
                return false;
            }
            pendingTasks().append(uiTask);
            uiTask->moveToThread(application->thread());
            QCoreApplication::postEvent(uiTask, new QEvent(QEvent::User));
        }
        return true;
    }

    void CefUiBridge::resumeTasks() {
        assertOnUiThread();
        const QMutexLocker lock(&pendingTaskMutex());
        acceptingTasks() = true;
    }

    void CefUiBridge::beginShutdown() {
        assertOnUiThread();
        {
            const QMutexLocker lock(&pendingTaskMutex());
            acceptingTasks() = false;
        }
        cancelPendingTasks();
    }

    void CefUiBridge::cancelPendingTasks() {
        assertOnUiThread();
        QList<QPointer<CefUiTask>> tasks;
        {
            const QMutexLocker lock(&pendingTaskMutex());
            tasks = std::move(pendingTasks());
            pendingTasks().clear();
        }
        for (CefUiTask *task : tasks) {
            delete task;
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

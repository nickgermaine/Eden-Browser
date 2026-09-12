#include "platform/waylandglibeventdispatcher.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QMetaObject>
#include <QTest>
#include <QTimer>

#include <glib-unix.h>

#include <unistd.h>

#include <chrono>
#include <thread>

struct DispatchResult {
    bool called = false;
    QEventLoop *loop = nullptr;
};

class WaylandGlibEventDispatcherTest final : public QObject {
    Q_OBJECT

  private slots:
    void dispatchesAttachedIdleSource();
    void dispatchesSourceAttachedFromAnotherThread();
    void dispatchesReadyFileDescriptor();
    void dispatchesAttachedTimeoutSource();
};

void WaylandGlibEventDispatcherTest::dispatchesAttachedIdleSource() {
    GMainContext *context = g_main_context_new();
    eden::platform::WaylandGlibEventDispatcher dispatcher(nullptr, context);
    QEventLoop loop;
    DispatchResult result{false, &loop};
    GSource *source = g_idle_source_new();
    g_source_set_callback(
        source,
        [](gpointer data) {
            auto *result = static_cast<DispatchResult *>(data);
            result->called = true;
            result->loop->quit();
            return G_SOURCE_REMOVE;
        },
        &result,
        nullptr
    );
    g_source_attach(source, context);
    g_source_unref(source);

    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    loop.exec();
    QVERIFY(result.called);
    g_main_context_unref(context);
}

void WaylandGlibEventDispatcherTest::dispatchesSourceAttachedFromAnotherThread() {
    GMainContext *context = g_main_context_new();
    eden::platform::WaylandGlibEventDispatcher dispatcher(nullptr, context);
    QEventLoop loop;
    DispatchResult result{false, &loop};
    QElapsedTimer elapsed;
    elapsed.start();
    std::thread producer([context, &result] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        GSource *source = g_idle_source_new();
        g_source_set_callback(
            source,
            [](gpointer data) {
                auto *result = static_cast<DispatchResult *>(data);
                result->called = true;
                QMetaObject::invokeMethod(result->loop, &QEventLoop::quit, Qt::QueuedConnection);
                return G_SOURCE_REMOVE;
            },
            &result,
            nullptr
        );
        g_source_attach(source, context);
        g_source_unref(source);
    });
    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    loop.exec();
    producer.join();

    QVERIFY(result.called);
    QVERIFY(elapsed.elapsed() < 300);
    g_main_context_unref(context);
}

void WaylandGlibEventDispatcherTest::dispatchesReadyFileDescriptor() {
    int descriptors[2] = {-1, -1};
    QVERIFY(pipe(descriptors) == 0);
    GMainContext *context = g_main_context_new();
    QEventLoop loop;
    DispatchResult result{false, &loop};
    GSource *source = g_unix_fd_source_new(descriptors[0], G_IO_IN);
    const auto callback = +[](gint descriptor, GIOCondition, gpointer data) {
        char byte = 0;
        const ssize_t bytesRead = read(descriptor, &byte, sizeof(byte));
        auto *result = static_cast<DispatchResult *>(data);
        result->called = bytesRead == 1;
        result->loop->quit();
        return G_SOURCE_REMOVE;
    };
    g_source_set_callback(source, G_SOURCE_FUNC(callback), &result, nullptr);
    g_source_attach(source, context);
    g_source_unref(source);
    {
        eden::platform::WaylandGlibEventDispatcher dispatcher(nullptr, context);
        QTimer::singleShot(20, &loop, [descriptor = descriptors[1]] {
            const char byte = 1;
            write(descriptor, &byte, sizeof(byte));
        });
        QTimer::singleShot(1000, &loop, &QEventLoop::quit);
        loop.exec();
        QVERIFY(result.called);
    }

    close(descriptors[0]);
    close(descriptors[1]);
    g_main_context_unref(context);
}

void WaylandGlibEventDispatcherTest::dispatchesAttachedTimeoutSource() {
    GMainContext *context = g_main_context_new();
    eden::platform::WaylandGlibEventDispatcher dispatcher(nullptr, context);
    QEventLoop loop;
    DispatchResult result{false, &loop};
    GSource *source = g_timeout_source_new(20);
    g_source_set_callback(
        source,
        [](gpointer data) {
            auto *result = static_cast<DispatchResult *>(data);
            result->called = true;
            result->loop->quit();
            return G_SOURCE_REMOVE;
        },
        &result,
        nullptr
    );
    g_source_attach(source, context);
    g_source_unref(source);

    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    loop.exec();
    QVERIFY(result.called);
    g_main_context_unref(context);
}

QTEST_GUILESS_MAIN(WaylandGlibEventDispatcherTest)

#include "waylandglibeventdispatcher_test.moc"

#include "engine/cef/cefbrowsersettings.h"
#include "engine/cef/cefprofile.h"
#include "engine/cef/cefruntime.h"
#include "engine/cef/cefuibridge.h"

#include "include/internal/cef_string_wrappers.h"

#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>

class CefBootstrapTest final : public QObject {
    Q_OBJECT

  private slots:
    void bridgeQueuesWorkToQtThread();
    void bridgePreservesOrderAndYields();
    void bridgeCancelsPendingWork();
    void bridgeSupportsNestedEventLoops();
    void alloyRuntimeIsExplicit();
    void privateContextSettingsAreInMemory();
    void runtimeCreatesIsolatedProfiles();
};

void CefBootstrapTest::bridgeQueuesWorkToQtThread() {
    std::atomic_bool invoked = false;
    std::atomic_bool queued = false;
    std::atomic_bool ranOnUiThread = false;
    std::thread callbackThread([&] {
        queued = eden::engine::cef::CefUiBridge::runOnUiThread([&] {
            ranOnUiThread = eden::engine::cef::CefUiBridge::isOnUiThread();
            invoked = true;
        });
    });
    callbackThread.join();
    QVERIFY(queued.load());
    QTRY_VERIFY(invoked.load());
    QVERIFY(ranOnUiThread.load());
}

void CefBootstrapTest::bridgePreservesOrderAndYields() {
    using eden::engine::cef::CefUiBridge;
    CefUiBridge::resumeTasks();
    QList<int> received;
    int completedAtOtherWork = -1;
    std::thread producer([&] {
        for (int value = 0; value < 16000; ++value) {
            CefUiBridge::runOnUiThread([&, value] { received.append(value); });
        }
    });
    producer.join();
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [&] { completedAtOtherWork = received.size(); },
        Qt::QueuedConnection
    );
    QTRY_COMPARE(received.size(), 16000);
    QVERIFY(completedAtOtherWork >= 0);
    QVERIFY(completedAtOtherWork < received.size());
    for (int index = 0; index < received.size(); ++index) {
        QCOMPARE(received[index], index);
    }
}

void CefBootstrapTest::bridgeCancelsPendingWork() {
    using eden::engine::cef::CefUiBridge;
    int completed = 0;
    QVERIFY(CefUiBridge::runOnUiThread([&] {
        ++completed;
        CefUiBridge::beginShutdown();
        QVERIFY(!CefUiBridge::runOnUiThread([&] { completed += 100; }));
        CefUiBridge::resumeTasks();
        QVERIFY(CefUiBridge::runOnUiThread([&] { ++completed; }));
    }));
    for (int index = 0; index < 256; ++index) {
        QVERIFY(CefUiBridge::runOnUiThread([&] { completed += 100; }));
    }
    QTRY_COMPARE(completed, 2);
    QCoreApplication::processEvents();
    QCOMPARE(completed, 2);
    int destroyed = 0;
    auto capture = std::shared_ptr<int>(new int(0), [&](int *value) {
        delete value;
        ++destroyed;
        CefUiBridge::runOnUiThread([&] { ++completed; });
    });
    QVERIFY(CefUiBridge::runOnUiThread([capture] {}));
    capture.reset();
    CefUiBridge::cancelPendingTasks();
    QCOMPARE(destroyed, 1);
    QTRY_COMPARE(completed, 3);
}

void CefBootstrapTest::bridgeSupportsNestedEventLoops() {
    using eden::engine::cef::CefUiBridge;
    QEventLoop nested;
    bool completed = false;
    bool timedOut = false;
    QVERIFY(CefUiBridge::runOnUiThread([&] {
        QVERIFY(CefUiBridge::runOnUiThread([&] { nested.quit(); }));
        QTimer deadline;
        deadline.setSingleShot(true);
        connect(&deadline, &QTimer::timeout, &nested, [&] {
            timedOut = true;
            nested.quit();
        });
        deadline.start(1000);
        nested.exec();
        completed = true;
    }));
    QTRY_VERIFY(completed);
    QVERIFY(!timedOut);
}

void CefBootstrapTest::alloyRuntimeIsExplicit() {
    CefWindowInfo windowInfo;
    windowInfo.runtime_style = CEF_RUNTIME_STYLE_CHROME;
    eden::engine::cef::configureAlloyRuntime(windowInfo);
    QCOMPARE(windowInfo.runtime_style, CEF_RUNTIME_STYLE_ALLOY);
}

void CefBootstrapTest::privateContextSettingsAreInMemory() {
    const std::filesystem::path profilePath = "/tmp/eden-cef-profile-test/profiles/abc";
    const CefRequestContextSettings normal = eden::engine::cef::createCefRequestContextSettings(false, profilePath);
    const CefRequestContextSettings privateProfile = eden::engine::cef::createCefRequestContextSettings(true, {});
    QCOMPARE(
        QString::fromStdString(CefString(&normal.cache_path).ToString()),
        QString::fromStdString(profilePath.string())
    );
    QCOMPARE(normal.persist_session_cookies, 1);
    QVERIFY(CefString(&privateProfile.cache_path).ToString().empty());
    QCOMPARE(privateProfile.persist_session_cookies, 0);
    QVERIFY(eden::engine::cef::isPathWithinRoot(profilePath, "/tmp/eden-cef-profile-test"));
    QVERIFY(!eden::engine::cef::isPathWithinRoot("/tmp/elsewhere/abc", "/tmp/eden-cef-profile-test"));
    QVERIFY(!eden::engine::cef::isPathWithinRoot("/tmp/eden-cef-profile-test/../escape", "/tmp/eden-cef-profile-test"));
}

void CefBootstrapTest::runtimeCreatesIsolatedProfiles() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    QByteArray executable = "eden-cef-bootstrap-tests";
    QByteArray noSandbox = "--no-sandbox";
    QByteArray ozonePlatform = "--ozone-platform=x11";
    char *arguments[] = {executable.data(), noSandbox.data(), ozonePlatform.data()};
    eden::engine::cef::CefRuntime &runtime = eden::engine::cef::CefRuntime::instance();
    QVERIFY(runtime.initialize(3, arguments, "Eden", "0.3.0", temporaryDirectory.path().toStdString()));
    {
        eden::engine::EngineProfileParameters normalParameters;
        normalParameters.profileId = QStringLiteral("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
        normalParameters.backend = eden::engine::Backend::Cef;
        normalParameters.dataPath = temporaryDirectory.path() + "/aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
        eden::engine::EngineProfileParameters privateParameters;
        privateParameters.backend = eden::engine::Backend::Cef;
        privateParameters.privateProfile = true;
        eden::engine::cef::CefProfile normalProfile(normalParameters);
        eden::engine::cef::CefProfile firstPrivateProfile(privateParameters);
        eden::engine::cef::CefProfile secondPrivateProfile(privateParameters);
        QVERIFY(normalProfile.requestContext());
        QVERIFY(firstPrivateProfile.requestContext());
        QVERIFY(secondPrivateProfile.requestContext());
        QVERIFY(!normalProfile.requestContext()->GetCachePath().empty());
        QVERIFY(firstPrivateProfile.requestContext()->GetCachePath().empty());
        QVERIFY(secondPrivateProfile.requestContext()->GetCachePath().empty());
        QVERIFY(!firstPrivateProfile.requestContext()->IsSame(secondPrivateProfile.requestContext()));
        QVERIFY(!normalProfile.requestContext()->IsSharingWith(firstPrivateProfile.requestContext()));
    }
    runtime.shutdown();
    QVERIFY(!runtime.isInitialized());
}

int main(int argc, char *argv[]) {
    eden::engine::cef::CefRuntime &runtime = eden::engine::cef::CefRuntime::instance();
    const int processExitCode = runtime.executeProcess(argc, argv);
    if (processExitCode >= 0) {
        return processExitCode;
    }
    QCoreApplication application(argc, argv);
    CefBootstrapTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "cefbootstrap_test.moc"

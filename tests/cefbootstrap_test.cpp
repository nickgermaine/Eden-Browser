#include "engine/cef/cefbrowsersettings.h"
#include "engine/cef/cefprofile.h"
#include "engine/cef/cefruntime.h"
#include "engine/cef/cefuibridge.h"

#include "include/internal/cef_string_wrappers.h"

#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <filesystem>
#include <thread>

class CefBootstrapTest final : public QObject {
    Q_OBJECT

  private slots:
    void bridgeQueuesWorkToQtThread();
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

void CefBootstrapTest::alloyRuntimeIsExplicit() {
    CefWindowInfo windowInfo;
    windowInfo.runtime_style = CEF_RUNTIME_STYLE_CHROME;
    eden::engine::cef::configureAlloyRuntime(windowInfo);
    QCOMPARE(windowInfo.runtime_style, CEF_RUNTIME_STYLE_ALLOY);
}

void CefBootstrapTest::privateContextSettingsAreInMemory() {
    const std::filesystem::path rootCachePath = "/tmp/eden-cef-profile-test";
    const CefRequestContextSettings normal = eden::engine::cef::createCefRequestContextSettings(false, rootCachePath);
    const CefRequestContextSettings privateProfile =
        eden::engine::cef::createCefRequestContextSettings(true, rootCachePath);
    QCOMPARE(
        QString::fromStdString(CefString(&normal.cache_path).ToString()),
        QString::fromStdString((rootCachePath / "default").string())
    );
    QCOMPARE(normal.persist_session_cookies, 1);
    QVERIFY(CefString(&privateProfile.cache_path).ToString().empty());
    QCOMPARE(privateProfile.persist_session_cookies, 0);
}

void CefBootstrapTest::runtimeCreatesIsolatedProfiles() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    QByteArray executable = QCoreApplication::applicationFilePath().toLocal8Bit();
    QByteArray noSandbox = "--no-sandbox";
    QByteArray ozonePlatform = "--ozone-platform=x11";
    char *arguments[] = {executable.data(), noSandbox.data(), ozonePlatform.data()};
    eden::engine::cef::CefRuntime &runtime = eden::engine::cef::CefRuntime::instance();
    QVERIFY(runtime.initialize(3, arguments, "Eden", "0.3.0", temporaryDirectory.path().toStdString()));
    {
        eden::engine::cef::CefProfile normalProfile(false);
        eden::engine::cef::CefProfile firstPrivateProfile(true);
        eden::engine::cef::CefProfile secondPrivateProfile(true);
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

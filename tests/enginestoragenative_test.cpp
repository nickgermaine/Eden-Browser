#include <libsecret/secret.h>

#include "core/profiles/cookiehandoffcoordinator.h"
#include "core/profiles/enginestorage.h"
#include "engine/cef/cefruntime.h"
#include "engine/enginefactory.h"
#include "engine/engineprofile.h"
#include "engine/engineview.h"

#include <QDirIterator>
#include <QFile>
#include <QGuiApplication>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <cstdio>

extern "C" gchar *secret_password_lookup_sync(const SecretSchema *, GCancellable *, GError **, ...) {
    return g_strdup(QByteArray(32, 'T').toBase64().constData());
}

namespace {
    const QByteArray marker("eden-browser-engine-private-canary-c4742d7a");

    bool waitUntil(const std::function<bool()> &ready, int milliseconds = 20000) {
        QElapsedTimer timer;
        timer.start();
        while (!ready() && timer.elapsed() < milliseconds) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
            QThread::msleep(5);
        }
        return ready();
    }

    QByteArray readFile(const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    void trace(const char *stage) {
        if (qEnvironmentVariableIsSet("EDEN_TEST_STORAGE_TRACE")) {
            std::fprintf(stderr, "Storage fixture: %s\n", stage);
            std::fflush(stderr);
        }
    }

    int child(const QString &backendName, const QUrl &url) {
        const auto backend = backendName == "cef" ? eden::engine::Backend::Cef : eden::engine::Backend::QtWebEngine;
        const auto roots = eden::core::ProfilePaths::standardRoots();
        auto *storage = eden::core::EngineStorage::instance();
        bool prepared = false;
        QString error;
        storage->prepare(roots, [&](const QString &result) {
            error = result;
            prepared = true;
        });
        if (!waitUntil([&] { return prepared; }, 40000) || !error.isEmpty()) {
            qWarning().noquote() << "Storage preparation:" << error;
            return 2;
        }
        trace("encryption ready");
        int result = 0;
        {
            QQmlEngine engine;
            eden::engine::EngineProfileParameters parameters;
            parameters.backend = backend;
            parameters.profileId = "native-storage-fixture";
            parameters.dataPath = roots.dataRoot + "/engine-data/" + backendName + "/profiles/native-storage-fixture";
            parameters.cachePath = roots.cacheRoot + "/profiles/native-storage-fixture/" + backendName;
            trace("creating source engine profile");
            auto profile = eden::engine::EngineFactory::create(parameters, &engine);
            trace("source engine profile created");
            if (!profile) {
                result = 3;
            } else {
                QQuickWindow window;
                window.resize(700, 500);
                QQmlComponent component(&engine);
                component.setData("import QtQuick; Item { width: 700; height: 500 }", QUrl());
                std::unique_ptr<QObject> object(component.create());
                auto *viewport = qobject_cast<QQuickItem *>(object.get());
                if (!viewport) {
                    return 8;
                }
                viewport->setParentItem(window.contentItem());
                window.show();
                auto view = eden::engine::EngineFactory::create(backend, profile.get());
                if (!view) {
                    result = 4;
                } else {
                    view->attach(viewport);
                    view->load(url);
                    if (!waitUntil([&] {
                            return view->title() == "storage-ready" || view->title().startsWith("failed:");
                        })) {
                        qWarning() << "Timed out waiting for storage fixture";
                        result = 5;
                    } else if (view->title() != "storage-ready") {
                        qWarning().noquote() << view->title();
                        result = 6;
                    }
                    trace("site storage ready");
                    if (result == 0 && url.path() == "/write") {
                        auto otherParameters = parameters;
                        otherParameters.backend = backend == eden::engine::Backend::Cef
                                                      ? eden::engine::Backend::QtWebEngine
                                                      : eden::engine::Backend::Cef;
                        const QString otherName = backend == eden::engine::Backend::Cef ? "qtwebengine" : "cef";
                        otherParameters.dataPath =
                            roots.dataRoot + "/engine-data/" + otherName + "/profiles/native-storage-fixture";
                        otherParameters.cachePath = roots.cacheRoot + "/profiles/native-storage-fixture/" + otherName;
                        trace("creating destination engine profile");
                        auto other = eden::engine::EngineFactory::create(otherParameters, &engine);
                        trace("destination engine profile created");
                        if (!other) {
                            result = 9;
                        } else {
                            eden::core::CookieHandoffCoordinator coordinator(parameters.profileId);
                            bool transferred = false;
                            eden::core::ProfileError error = eden::core::ProfileError::HandoffFailed;
                            coordinator.requestHandoff(profile, other, [&](auto status, const QString &) {
                                error = status;
                                transferred = true;
                            });
                            if (!waitUntil([&] { return transferred; }) || error != eden::core::ProfileError::None) {
                                result = 10;
                            } else {
                                bool received = false;
                                bool valid = false;
                                other->exportPortableCookies([&](auto snapshot) {
                                    for (const auto &cookie : snapshot.cookies) {
                                        if (cookie.name == "work" && cookie.value == marker && cookie.hostOnly &&
                                            cookie.path == "/" &&
                                            cookie.sameSite == eden::engine::CookieSameSite::Strict &&
                                            cookie.expires.has_value()) {
                                            valid = true;
                                        }
                                    }
                                    valid = valid && snapshot.errorCode.isEmpty();
                                    eden::engine::wipePortableCookies(snapshot.cookies);
                                    received = true;
                                });
                                if (!waitUntil([&] { return received; }) || !valid) {
                                    result = 11;
                                }
                            }
                        }
                    }
                    trace("handoff and destination cleanup finished");
                    bool flushed = false;
                    profile->flushStorage([&] { flushed = true; });
                    if (!waitUntil([&] { return flushed; })) {
                        result = 7;
                    }
                    trace("detaching source view");
                    view->attach(nullptr);
                }
            }
        }
        trace("source and QML engine destroyed");
        eden::engine::EngineFactory::shutdown();
        trace("engines shut down");
        storage->shutdown();
        return result;
    }
}

class EngineStorageNativeTest final : public QObject {
    Q_OBJECT
  private slots:
    void persistsAllSiteStores_data();
    void persistsAllSiteStores();
};

void EngineStorageNativeTest::persistsAllSiteStores_data() {
    QTest::addColumn<QString>("backend");
    QTest::newRow("blink") << QString("cef");
    QTest::newRow("blink-qt") << QString("qtwebengine");
}

void EngineStorageNativeTest::persistsAllSiteStores() {
    QFETCH(QString, backend);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QHash<QTcpSocket *, QByteArray> requests;
    int httpCacheRequests = 0;
    connect(&server, &QTcpServer::newConnection, this, [&] {
        while (auto *socket = server.nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                requests[socket] += socket->readAll();
                if (!requests[socket].contains("\r\n\r\n")) {
                    return;
                }
                const QByteArray path = requests.take(socket).split(' ').value(1);
                QByteArray body;
                QByteArray type("text/html");
                QByteArray cache("no-store");
                if (path == "/worker.js") {
                    type = "application/javascript";
                    body = "self.addEventListener('install',()=>self.skipWaiting());self.addEventListener('activate',"
                           "event=>event.waitUntil(clients.claim()));";
                } else if (path == "/http-cache") {
                    ++httpCacheRequests;
                    type = "text/plain";
                    cache = "max-age=3600";
                    body = marker;
                } else {
                    body = R"HTML(<!doctype html><script>
(async()=>{
const marker='eden-browser-engine-private-canary-c4742d7a';
const write=location.pathname==='/write';
const database=await new Promise((resolve,reject)=>{
    const request=indexedDB.open('work-storage',1);
    request.onupgradeneeded=()=>request.result.createObjectStore('entries');
    request.onerror=()=>reject(new Error('indexeddb-open'));
    request.onsuccess=()=>resolve(request.result);
});
const store=async(value)=>new Promise((resolve,reject)=>{
    const transaction=database.transaction('entries',write?'readwrite':'readonly');
    const request=write?transaction.objectStore('entries').put(value,'entry'):transaction.objectStore('entries').get('entry');
    let result;
    request.onsuccess=()=>result=request.result;
    transaction.oncomplete=()=>resolve(result);
    transaction.onerror=()=>reject(new Error('indexeddb-transaction'));
});
const cache=await caches.open('work-cache');
if(write){
    document.cookie='work='+marker+'; Path=/; Max-Age=3600; SameSite=Strict';
    localStorage.setItem('entry',marker);
    await store(marker);
    await cache.put('/cached-entry',new Response(marker));
    await navigator.serviceWorker.register('/worker.js');
    await navigator.serviceWorker.ready;
}else{
    if(!document.cookie.includes('work='+marker))throw new Error('cookie');
    if(localStorage.getItem('entry')!==marker)throw new Error('localstorage');
    if(await store()!==marker)throw new Error('indexeddb');
    const response=await cache.match('/cached-entry');
    if(!response||await response.text()!==marker)throw new Error('cachestorage');
    const registration=await navigator.serviceWorker.getRegistration('/');
    if(!registration||!registration.active)throw new Error('serviceworker');
}
if(await (await fetch('/http-cache')).text()!==marker)throw new Error('httpcache');
database.close();
document.title='storage-ready';
})().catch(error=>document.title='failed:'+error.message);
</script>)HTML";
                }
                socket->write(
                    "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: " + type + "\r\nCache-Control: " + cache +
                    "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body
                );
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    for (const QString &phase : {QString("write"), QString("read")}) {
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("XDG_DATA_HOME", directory.filePath("data"));
        environment.insert("XDG_CACHE_HOME", directory.filePath("cache"));
        environment.insert("XDG_CONFIG_HOME", directory.filePath("config"));
        const bool sandbox = qEnvironmentVariableIsSet("EDEN_TEST_NATIVE_SANDBOX");
        environment.insert("QTWEBENGINE_CHROMIUM_FLAGS", sandbox ? "" : "--no-sandbox");
        process.setProcessEnvironment(environment);
        process.setProgram(QCoreApplication::applicationFilePath());
        process.setArguments(
            {"--storage-child",
             backend,
             QString("http://127.0.0.1:%1/%2").arg(server.serverPort()).arg(phase),
             "--no-sandbox",
             "--engine-compositing=osr",
             "--ozone-platform=x11"}
        );
        if (sandbox) {
            auto arguments = process.arguments();
            arguments.removeAll("--no-sandbox");
            process.setArguments(arguments);
        }
        if (qEnvironmentVariableIsSet("EDEN_TEST_STORAGE_GDB") && phase == "write") {
            QStringList arguments{
                "--batch",
                "--return-child-result",
                "-ex",
                "set pagination off",
                "-ex",
                "run",
                "-ex",
                "thread apply all bt 30",
                "-ex",
                "info proc mappings",
                "--args",
                process.program()
            };
            arguments.append(process.arguments());
            process.setProgram("gdb");
            process.setArguments(arguments);
        }
        process.start();
        QVERIFY(process.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(process.state() == QProcess::NotRunning, 60000);
        const QByteArray output = phase.toUtf8() + " exit=" + QByteArray::number(process.exitCode()) +
                                  " status=" + QByteArray::number(process.exitStatus()) + '\n' +
                                  process.readAllStandardError() + process.readAllStandardOutput();
        if (qEnvironmentVariableIsSet("EDEN_TEST_STORAGE_GDB")) {
            QFile diagnostic("/tmp/eden-work-storage-gdb-output.txt");
            if (diagnostic.open(QIODevice::WriteOnly)) {
                diagnostic.write(output);
            }
        }
        QVERIFY2(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0, output.constData());
        const QStringList encrypted{
            directory.filePath("data/EdenStorageNativeFixture/engine-data.encrypted"),
            directory.filePath("cache/EdenStorageNativeFixture/profiles.encrypted")
        };
        for (const QString &root : encrypted) {
            QVERIFY(QDir(root).exists());
            QDirIterator entries(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
            while (entries.hasNext()) {
                const QString path = entries.next();
                QVERIFY(!readFile(path).contains(marker));
            }
        }
    }
    QCOMPARE(httpCacheRequests, 1);
    for (const QString &path :
         {directory.filePath("data/EdenStorageNativeFixture/engine-data"),
          directory.filePath("cache/EdenStorageNativeFixture/profiles")}) {
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
}

int main(int argc, char *argv[]) {
    const int subprocess = eden::engine::cef::CefRuntime::instance().executeProcess(argc, argv);
    if (subprocess >= 0) {
        return subprocess;
    }
    eden::engine::EngineFactory::configureApplicationArguments(argc, argv);
    if (!eden::engine::EngineFactory::prepareApplication(eden::engine::Backend::QtWebEngine)) {
        return 1;
    }
    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName("EdenStorageNativeFixture");
    const QStringList arguments = application.arguments();
    if (arguments.size() > 3 && arguments[1] == "--storage-child") {
        return child(arguments[2], QUrl(arguments[3]));
    }
    EngineStorageNativeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "enginestoragenative_test.moc"

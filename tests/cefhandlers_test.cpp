#include "core/profiles/applicationcontext.h"
#include "core/profiles/profileeditorcontroller.h"
#include "core/profiles/profilelistmodel.h"
#include "core/profiles/profilemanager.h"
#include "core/settings/settingsstore.h"
#include "core/settings/shortcutregistry.h"
#include "core/settings/theme/thememanager.h"
#include "core/window/tabstripnavigator.h"
#include "core/window/windowcontroller.h"
#include "core/window/windowframe.h"
#include "engine/cef/cefengineview.h"
#include "engine/cef/cefprofile.h"
#include "engine/cef/cefruntime.h"
#include "engine/cef/devtoolssocketserver.h"
#include "engine/enginefactory.h"
#include "engine/engineregistry.h"
#include "profiletesthelpers.h"

#include <QBuffer>
#include <QClipboard>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtGui/qguiapplication_platform.h>
#include <QtTest>

#include <X11/Xlib.h>

#undef KeyPress
#undef KeyRelease

#include <array>
#include <memory>
#include <vector>

class LocalPageServer final : public QTcpServer {
    Q_OBJECT

  public:
    explicit LocalPageServer(QObject *parent = nullptr)
        : QTcpServer(parent) {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection()) {
                if (!socket->peerAddress().isLoopback()) {
                    socket->abort();
                    socket->deleteLater();
                    continue;
                }
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    QByteArray &request = m_requests[socket];
                    request.append(socket->readAll());
                    const qsizetype headerEnd = request.indexOf("\r\n\r\n");
                    if (headerEnd < 0) {
                        return;
                    }
                    const QByteArray header = request.first(headerEnd);
                    const QList<QByteArray> lines = header.split('\n');
                    qsizetype contentLength = 0;
                    for (const QByteArray &line : lines) {
                        if (line.toLower().startsWith("content-length:")) {
                            contentLength = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                        }
                    }
                    if (request.size() < headerEnd + 4 + contentLength) {
                        return;
                    }
                    const QList<QByteArray> requestLine = lines.constFirst().trimmed().split(' ');
                    const QByteArray path = requestLine.value(1);
                    const QByteArray body = request.mid(headerEnd + 4, contentLength);
                    m_requests.remove(socket);
                    if (path == "/") {
                        ++m_rootRequests;
                    }
                    QByteArray contentType = "text/html; charset=utf-8";
                    QByteArray responseBody;
                    if (path == "/form-events") {
                        responseBody = R"HTML(<!doctype html><title>Form events</title>
<input id="todo" name="todo" style="position:absolute;left:20px;top:20px;width:300px;height:48px">
<input id="email" name="email" type="email" style="position:absolute;left:20px;top:90px;width:300px;height:48px">
<script>
const fields = Array.from(document.querySelectorAll('input'));
for (let index = 0; index < 1000; ++index) {
    for (const field of fields) {
        field.value = String(index);
        field.dispatchEvent(new Event('input', {bubbles:true}));
    }
}
for (const field of fields) {
    field.value = '';
    field.addEventListener('input', () => document.title = field.name + ':' + field.value);
}
document.title = 'Synthetic inputs complete';
</script>)HTML";
                    } else if (path == "/visibility") {
                        responseBody = R"HTML(<!doctype html><title>Visibility pending</title>
<script>
let frames = 0;
const report = () => document.title = document.visibilityState + ':' + frames;
document.addEventListener('visibilitychange', report);
function tick() {
    frames++;
    report();
    requestAnimationFrame(tick);
}
report();
requestAnimationFrame(tick);
</script>)HTML";
                    } else if (path == "/address-form") {
                        responseBody = R"HTML(<!doctype html><title>Address form</title>
<input autocomplete="email"><textarea autocomplete="street-address"></textarea>
<input autocomplete="address-level2">
<script>
const fields = Array.from(document.querySelectorAll('input,textarea'));
const changes = [];
for (const kind of ['input', 'change']) {
    document.addEventListener(kind, event => {
        changes.push(event.type);
        document.title = fields.map(field => field.value).join('|') + ':' + changes.join(',');
    });
}
</script>)HTML";
                    } else if (path == "/clipboard-denied") {
                        responseBody = R"HTML(<!doctype html><title>Clipboard pending</title>
<input value="Copied by user" style="position:absolute;left:20px;top:20px;width:300px;height:48px">
<script>
const data = new DataTransfer();
data.setData('text/plain', 'Synthetic clipboard text');
window.dispatchEvent(new ClipboardEvent('copy', {clipboardData:data}));
window.dispatchEvent(new ClipboardEvent('cut', {clipboardData:data}));
navigator.clipboard.writeText('Denied clipboard text').then(
    () => document.title = 'Unexpected clipboard success',
    () => document.title = 'Clipboard denied'
);
</script>)HTML";
                    } else if (path == "/events") {
                        responseBody = R"HTML(<!doctype html>
<title>Navigation events</title><link rel="icon" href="/favicon.png">
<button id="frames" style="position:absolute;left:20px;top:20px;width:180px;height:48px">Frame activity</button>
<a href="#section" style="position:absolute;left:20px;top:90px;width:180px;height:48px">Same document</a>
<button id="state" style="position:absolute;left:220px;top:20px;width:180px;height:48px">Update page</button>
<button id="same-url" style="position:absolute;left:220px;top:90px;width:180px;height:48px">Add history state</button>
<input type="file" style="position:absolute;left:20px;top:160px;width:240px;height:48px">
<script>
document.getElementById('same-url').onclick = () => history.pushState({step:1}, '', location.href);
document.getElementById('state').onclick = () => {
    history.pushState({}, '', '/events?state=1');
    history.replaceState({}, '', '/events?state=2');
    document.title = 'Updated navigation';
    document.querySelector('link').href = '/frame-icon.png';
};
document.getElementById('frames').onclick = async () => {
    for (let index = 0; index < 24; ++index) {
        const frame = document.createElement('iframe');
        const loaded = new Promise(resolve => frame.onload = resolve);
        const url = new URL('/event-frame?index=' + index, location.href);
        if (index % 2) { url.hostname = 'localhost'; }
        frame.src = url.href;
        document.body.append(frame);
        await loaded;
        if (index % 2 === 0) {
            frame.contentWindow.history.replaceState({}, '', '/frame-state?index=' + index);
        }
        frame.remove();
    }
    await fetch('/events-complete');
};
</script>)HTML";
                    } else if (path.startsWith("/event-frame?")) {
                        responseBody = "<!doctype html><title>Child frame</title>"
                                       "<link rel=icon href=/frame-icon.png><body>Frame content";
                    } else if (path == "/events-complete") {
                        ++m_completedFrameRuns;
                        responseBody = "complete";
                    } else if (path == "/events-pending") {
                        ++m_pendingNavigations;
                        return;
                    } else if (path == "/events-redirect") {
                        socket->write(
                            "HTTP/1.1 302 Found\r\nLocation: /events\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                        );
                        socket->disconnectFromHost();
                        return;
                    } else if (path == "/favicon.png" || path == "/frame-icon.png") {
                        QImage icon(16, 16, QImage::Format_ARGB32);
                        icon.fill(QColor(path == "/favicon.png" ? "#315d32" : "#cc0088"));
                        QBuffer buffer(&responseBody);
                        buffer.open(QIODevice::WriteOnly);
                        icon.save(&buffer, "PNG");
                        contentType = "image/png";
                    } else if (path == "/download.txt") {
                        responseBody = "download-ok";
                        contentType = "text/plain";
                    } else if (path == "/popup") {
                        const QString postValue =
                            QString::fromUtf8(body).contains("value=posted") ? "posted" : "missing";
                        responseBody =
                            QString(
                                "<!doctype html><title>Popup pending</title><body>popup<script>document.title='Popup "
                                "opener='+(window.opener&&window.opener.name==='source'?'yes':'no')+' "
                                "post=%1'</script></body>"
                            )
                                .arg(postValue)
                                .toUtf8();
                    } else if (path == "/delayed") {
                        responseBody =
                            "<!doctype html><title>Delayed pending</title><body>delayed<script>document.title='Delayed "
                            "opener='+(window.opener&&window.opener.name==='source'?'yes':'no')</script></body>";
                    } else {
                        responseBody = R"HTML(<!doctype html>
<html><head><title>E35 Main</title><link rel="icon" href="/favicon.png"></head>
<body style="margin:0;background:#d8ead3;color:#142211;font:20px sans-serif">
<form action="/popup" method="post" target="popupTarget"><input name="value" value="posted"><button id="post" style="position:absolute;left:20px;top:20px;width:180px;height:48px">POST popup</button></form>
<button id="delayed" style="position:absolute;left:20px;top:90px;width:180px;height:48px">Delayed popup</button>
<button id="dialog" style="position:absolute;left:20px;top:160px;width:180px;height:48px">Dialog</button>
<button id="permission" style="position:absolute;left:20px;top:230px;width:180px;height:48px">Permission</button>
<a id="download" download="eden-handler.txt" href="/download.txt" style="position:absolute;left:20px;top:300px">Download</a>
<a id="cursor-link" href="#cursor" style="position:absolute;left:20px;top:360px">Cursor link</a>
<input id="editor" style="position:absolute;left:20px;top:410px;width:400px;height:42px;font-size:20px">
<button id="display" style="position:absolute;left:20px;top:470px;width:180px;height:48px">Share display</button>
<div style="position:absolute;left:280px;top:40px;width:500px;height:380px;border-radius:32px;background:#315d32;color:white;display:grid;place-items:center;font-size:36px">E3.5 OSR</div>
<script>
window.name='source'
document.getElementById('delayed').onclick=()=>setTimeout(()=>window.open('/delayed'),120)
document.getElementById('dialog').onclick=()=>prompt('Handler prompt','seed')
document.getElementById('permission').onclick=()=>navigator.geolocation.getCurrentPosition(()=>{},()=>{})
document.getElementById('editor').oninput=event=>document.title='Input '+event.target.value
document.getElementById('display').onclick=()=>{
    Object.defineProperty(navigator.mediaDevices,'getUserMedia',{
        configurable:true,
        value:constraints=>{
            const videoId=constraints.video.mandatory.chromeMediaSourceId
            const audioId=constraints.audio&&constraints.audio.mandatory?constraints.audio.mandatory.chromeMediaSourceId:'none'
            document.title='Capture source '+videoId+' audio '+audioId
            return Promise.resolve(new MediaStream())
        },
        writable:true
    })
    navigator.mediaDevices.getDisplayMedia({video:true,audio:true}).catch(()=>document.title='Capture cancelled')
}
</script></body></html>)HTML";
                    }
                    QByteArray headers = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: " + contentType +
                                         "\r\nContent-Length: " + QByteArray::number(responseBody.size()) + "\r\n";
                    if (path == "/clipboard-denied") {
                        headers += "Permissions-Policy: clipboard-write=()\r\n";
                    }
                    if (path == "/download.txt") {
                        headers += "Content-Disposition: attachment; filename=eden-handler.txt\r\n";
                    }
                    socket->write(headers + "\r\n" + responseBody);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, this, [this, socket] { m_requests.remove(socket); });
                connect(socket, &QTcpSocket::destroyed, this, [this, socket] { m_requests.remove(socket); });
            }
        });
    }

    int rootRequests() const {
        return m_rootRequests;
    }

    int completedFrameRuns() const {
        return m_completedFrameRuns;
    }

    int pendingNavigations() const {
        return m_pendingNavigations;
    }

  private:
    int m_rootRequests = 0;
    int m_completedFrameRuns = 0;
    int m_pendingNavigations = 0;
    QHash<QTcpSocket *, QByteArray> m_requests;
};

class CefHandlersTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void handlerSuite();
    void navigationEvents();
    void clipboardPermissions();
    void addressAutofill();
    void browserVisibility();
    void formEvents();
    void devToolsSuite();
    void shellDevToolsSuite();
    void resizeStress();
    void cleanupTestCase();

  private:
    QTemporaryDir m_dataDirectory;
    QTemporaryDir m_environmentDirectory;
    LocalPageServer m_server;
    std::unique_ptr<eden::core::ApplicationContext> m_applicationContext;
};

static bool hasColorVariation(const QImage &image, const QRect &region, int minimumColors) {
    QSet<QRgb> colors;
    for (int y = region.top(); y <= region.bottom(); ++y) {
        for (int x = region.left(); x <= region.right(); ++x) {
            colors.insert(image.pixel(x, y));
            if (colors.size() >= minimumColors) {
                return true;
            }
        }
    }
    return false;
}

static int differingPixels(const QImage &first, const QImage &second, const QRect &region) {
    if (first.size() != second.size() || first.isNull() || second.isNull()) {
        return 0;
    }
    int count = 0;
    for (int y = region.top(); y <= region.bottom(); y += 2) {
        for (int x = region.left(); x <= region.right(); x += 2) {
            if (first.pixel(x, y) != second.pixel(x, y)) {
                ++count;
            }
        }
    }
    return count;
}

void CefHandlersTest::initTestCase() {
    Q_INIT_RESOURCE(eden_ui_raw_res_0);
    QVERIFY(m_dataDirectory.isValid());
    QVERIFY(m_environmentDirectory.isValid());
    const QString configDirectory = QDir(m_environmentDirectory.path()).filePath("config");
    const QString downloadDirectory = QDir(m_environmentDirectory.path()).filePath("Downloads");
    QVERIFY(QDir().mkpath(configDirectory));
    QVERIFY(QDir().mkpath(downloadDirectory));
    QFile userDirectories(QDir(configDirectory).filePath("user-dirs.dirs"));
    QVERIFY(userDirectories.open(QIODevice::WriteOnly));
    userDirectories.write(QString("XDG_DOWNLOAD_DIR=\"%1\"\n").arg(downloadDirectory).toUtf8());
    userDirectories.close();
    qputenv("HOME", m_environmentDirectory.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", configDirectory.toUtf8());
    QQuickStyle::setStyle("Basic");
    qmlRegisterType<eden::core::WindowController>("Eden.Ui", 1, 0, "WindowController");
    qmlRegisterType<eden::core::WindowFrame>("Eden.Ui", 1, 0, "WindowFrame");
    qmlRegisterType<eden::core::TabStripNavigator>("Eden.Ui", 1, 0, "TabStripNavigator");
    qmlRegisterUncreatableType<eden::engine::EngineView>(
        "Eden.Ui",
        1,
        0,
        "EngineView",
        "Engine views are created by WindowController"
    );
    qmlRegisterUncreatableType<eden::core::ProfileListModel>(
        "Eden.Ui",
        1,
        0,
        "ProfileListModel",
        "The profile list is owned by ProfileManager"
    );
    qmlRegisterUncreatableType<eden::core::ProfileEditorController>(
        "Eden.Ui",
        1,
        0,
        "ProfileEditorController",
        "Profile editors are created by ProfileManager"
    );
    m_applicationContext = std::make_unique<eden::core::ApplicationContext>(
        eden::core::ProfilePaths::Roots{m_dataDirectory.path() + "/app-data", m_dataDirectory.path() + "/app-cache"}
    );
    qmlRegisterSingletonInstance("Eden.Ui", 1, 0, "Profiles", m_applicationContext->profiles());
    qmlRegisterSingletonInstance("Eden.Ui", 1, 0, "Engines", eden::engine::EngineRegistry::instance());
    qmlRegisterSingletonInstance("Eden.Ui", 1, 0, "Settings", eden::core::SettingsStore::instance());
    qmlRegisterSingletonInstance("Eden.Ui", 1, 0, "Themes", eden::core::ThemeManager::instance());
    eden::core::SettingsStore::instance()->initialize();
    eden::core::ThemeManager::instance()->refresh();
    QVERIFY(m_server.listen(QHostAddress::AnyIPv4));
    QList<QByteArray> encodedArguments;
    for (const QString &argument : QCoreApplication::arguments()) {
        encodedArguments.append(argument.toLocal8Bit());
    }
    std::vector<char *> arguments;
    for (QByteArray &argument : encodedArguments) {
        arguments.push_back(argument.data());
    }
    QVERIFY(
        eden::engine::cef::CefRuntime::instance().initialize(
            static_cast<int>(arguments.size()),
            arguments.data(),
            "Eden",
            "0.3.0",
            m_dataDirectory.path().toStdString()
        )
    );
}

void CefHandlersTest::handlerSuite() {
    const QString downloadDirectory = QDir(m_environmentDirectory.path()).filePath("Downloads");
    const QString existingDownloadPath = QDir(downloadDirectory).filePath("eden-handler.txt");
    QFile existingDownload(existingDownloadPath);
    QVERIFY(existingDownload.open(QIODevice::WriteOnly));
    QCOMPARE(existingDownload.write("preserve"), qint64(8));
    existingDownload.close();
    eden::engine::EngineProfileParameters privateParameters;
    privateParameters.backend = eden::engine::Backend::Cef;
    privateParameters.privateProfile = true;
    eden::engine::cef::CefProfile profile(privateParameters);
    QQuickWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *x11Application = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    QVERIFY(x11Application);
    Display *display = x11Application->display();
    QVERIFY(display);
    const Window nativeWindow = static_cast<Window>(window.winId());
    XWindowAttributes attributes;
    QVERIFY(XGetWindowAttributes(display, nativeWindow, &attributes));
    XSetInputFocus(display, nativeWindow, RevertToParent, CurrentTime);
    XSync(display, False);
    QTRY_VERIFY_WITH_TIMEOUT(window.isActive(), 5000);
    QQuickItem viewport(window.contentItem());
    viewport.setSize(window.size());
    eden::engine::cef::CefEngineView source(&profile);
    source.attach(&viewport);
    const QUrl mainUrl(QString("http://127.0.0.1:%1/").arg(m_server.serverPort()));
    source.load(mainUrl);
    QTRY_COMPARE_WITH_TIMEOUT(source.title(), QString("E35 Main"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!source.faviconUrl().isEmpty(), 15000);
    QTRY_COMPARE_WITH_TIMEOUT(source.loadProgress(), 100, 15000);

    std::vector<std::unique_ptr<QQuickWindow>> popupWindows;
    std::vector<std::unique_ptr<QQuickItem>> popupViewports;
    std::vector<std::unique_ptr<eden::engine::cef::CefEngineView>> popupViews;
    QStringList popupTitles;
    int popupRequests = 0;
    bool rejectPopup = false;
    connect(
        &source,
        &eden::engine::EngineView::newViewRequested,
        &source,
        [&](eden::engine::EngineNewViewRequest *request) {
            ++popupRequests;
            if (rejectPopup) {
                rejectPopup = false;
                return;
            }
            auto popupWindow = std::make_unique<QQuickWindow>();
            popupWindow->resize(760, 520);
            popupWindow->show();
            auto popupViewport = std::make_unique<QQuickItem>(popupWindow->contentItem());
            popupViewport->setSize(popupWindow->size());
            auto popupView = std::make_unique<eden::engine::cef::CefEngineView>(&profile);
            popupView->attach(popupViewport.get());
            connect(
                popupView.get(),
                &eden::engine::EngineView::titleChanged,
                popupView.get(),
                [&popupTitles, view = popupView.get()] { popupTitles.append(view->title()); }
            );
            QVERIFY(request->openIn(popupView.get()));
            popupWindows.push_back(std::move(popupWindow));
            popupViewports.push_back(std::move(popupViewport));
            popupViews.push_back(std::move(popupView));
        }
    );

    QQuickItem *osrItem = viewport.childItems().isEmpty() ? nullptr : viewport.childItems().constFirst();
    QVERIFY(osrItem);
    QTest::mouseMove(&window, QPoint(75, 370));
    QTRY_COMPARE_WITH_TIMEOUT(osrItem->cursor().shape(), Qt::PointingHandCursor, 5000);
    QTest::mouseMove(&window, QPoint(200, 430));
    QTRY_COMPARE_WITH_TIMEOUT(osrItem->cursor().shape(), Qt::IBeamCursor, 5000);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(200, 430));
    osrItem->forceActiveFocus(Qt::MouseFocusReason);
    QTRY_VERIFY_WITH_TIMEOUT(osrItem->hasActiveFocus(), 5000);
    QTest::keyClick(&window, Qt::Key_A);
    QTest::keyClick(&window, Qt::Key_Period);
    QTest::keyClick(&window, Qt::Key_B);
    QTest::keyClick(&window, Qt::Key_At);
    QTest::keyClick(&window, Qt::Key_E);
    QTest::keyClick(&window, Qt::Key_X);
    QTest::keyClick(&window, Qt::Key_A);
    QTest::keyClick(&window, Qt::Key_M);
    QTest::keyClick(&window, Qt::Key_P);
    QTest::keyClick(&window, Qt::Key_L);
    QTest::keyClick(&window, Qt::Key_E);
    QTest::keyClick(&window, Qt::Key_Period);
    QTest::keyClick(&window, Qt::Key_C);
    QTest::keyClick(&window, Qt::Key_O);
    QTest::keyClick(&window, Qt::Key_M);
    QTRY_COMPARE_WITH_TIMEOUT(source.title(), QString("Input a.b@example.com"), 10000);
    QGuiApplication::clipboard()->setText(" pasted.value");
    QTest::keyClick(&window, Qt::Key_V, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(source.title(), QString("Input a.b@example.com pasted.value"), 10000);

    source.load(mainUrl);
    QTRY_COMPARE_WITH_TIMEOUT(source.title(), QString("E35 Main"), 15000);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(110, 45));
    QTRY_COMPARE_WITH_TIMEOUT(popupRequests, 1, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(popupTitles.contains("Popup opener=yes post=posted"), 15000);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(110, 115));
    QTRY_VERIFY_WITH_TIMEOUT(popupTitles.contains("Delayed opener=yes"), 15000);

    popupViews.clear();
    popupViewports.clear();
    popupWindows.clear();

    QSignalSpy contextMenu(&source, &eden::engine::EngineView::contextMenuRequested);
    QTest::mouseClick(&window, Qt::RightButton, {}, QPoint(430, 220));
    QTRY_COMPARE_WITH_TIMEOUT(contextMenu.size(), 1, 10000);
    const int requestsBeforeReload = m_server.rootRequests();
    source.executeContextMenuCommand("reload");
    QTRY_VERIFY_WITH_TIMEOUT(m_server.rootRequests() > requestsBeforeReload, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(source.loadProgress(), 100, 10000);
    QTest::mouseClick(&window, Qt::RightButton, {}, QPoint(430, 220));
    QTRY_COMPARE_WITH_TIMEOUT(contextMenu.size(), 2, 10000);
    source.dismissContextMenu();
    QTest::qWait(100);

    const QUrl historyOne(QString("http://127.0.0.1:%1/history-one").arg(m_server.serverPort()));
    const QUrl historyTwo(QString("http://127.0.0.1:%1/history-two").arg(m_server.serverPort()));
    source.load(historyOne);
    QTRY_COMPARE_WITH_TIMEOUT(source.url(), historyOne, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(source.loadProgress(), 100, 10000);
    source.load(historyTwo);
    QTRY_COMPARE_WITH_TIMEOUT(source.url(), historyTwo, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(source.loadProgress(), 100, 10000);
    QTest::mouseClick(&window, Qt::RightButton, {}, QPoint(430, 220));
    QTRY_COMPARE_WITH_TIMEOUT(contextMenu.size(), 3, 10000);
    source.executeContextMenuCommand("back");
    QTRY_COMPARE_WITH_TIMEOUT(source.url(), historyOne, 10000);
    QTest::mouseClick(&window, Qt::RightButton, {}, QPoint(430, 220));
    QTRY_COMPARE_WITH_TIMEOUT(contextMenu.size(), 4, 10000);
    source.executeContextMenuCommand("forward");
    QTRY_COMPARE_WITH_TIMEOUT(source.url(), historyTwo, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(source.loadProgress(), 100, 10000);
    source.load(mainUrl);
    QTRY_COMPARE_WITH_TIMEOUT(source.url(), mainUrl, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(source.loadProgress(), 100, 10000);
    QTest::qWait(100);

    QSignalSpy dialog(&source, &eden::engine::EngineView::javaScriptDialogRequested);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(110, 185));
    QTRY_COMPARE_WITH_TIMEOUT(dialog.size(), 1, 10000);
    const eden::engine::JavaScriptDialogInfo dialogInfo =
        dialog.constFirst().constFirst().value<eden::engine::JavaScriptDialogInfo>();
    QCOMPARE(dialogInfo.kind, QString("prompt"));
    QCOMPARE(dialogInfo.defaultText, QString("seed"));
    source.resolveJavaScriptDialog(dialogInfo.id, true, "accepted");

    QSignalSpy permission(&source, &eden::engine::EngineView::permissionRequested);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(110, 255));
    QTRY_COMPARE_WITH_TIMEOUT(permission.size(), 1, 10000);
    const eden::engine::PermissionRequestInfo permissionInfo =
        permission.constFirst().constFirst().value<eden::engine::PermissionRequestInfo>();
    QVERIFY(permissionInfo.permissions.contains("location"));
    source.resolvePermissionRequest(permissionInfo.id, false);

    QSignalSpy displayCapture(&source, &eden::engine::EngineView::displayCaptureRequested);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(110, 495));
    QTRY_COMPARE_WITH_TIMEOUT(displayCapture.size(), 1, 10000);
    const eden::engine::DisplayCaptureRequestInfo displayCaptureInfo =
        displayCapture.constFirst().constFirst().value<eden::engine::DisplayCaptureRequestInfo>();
    QVERIFY(displayCaptureInfo.audioRequested);
    QCOMPARE(displayCaptureInfo.origin.host(), QString("127.0.0.1"));
    source.resolveDisplayCaptureRequest(displayCaptureInfo.id, {});
    QTRY_COMPARE_WITH_TIMEOUT(source.title(), QString("Capture cancelled"), 10000);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(110, 495));
    QTRY_COMPARE_WITH_TIMEOUT(displayCapture.size(), 2, 10000);
    const eden::engine::DisplayCaptureRequestInfo windowCaptureInfo =
        displayCapture.at(1).constFirst().value<eden::engine::DisplayCaptureRequestInfo>();
    source.resolveDisplayCaptureRequest(windowCaptureInfo.id, "window");
    const QRegularExpression windowCaptureTitle("^Capture source window:([1-9][0-9]*):0 audio none$");
    QTRY_VERIFY_WITH_TIMEOUT(windowCaptureTitle.match(source.title()).hasMatch(), 10000);

    QSignalSpy downloadStarted(&profile, &eden::engine::EngineProfile::downloadStarted);
    QSignalSpy downloadUpdated(&profile, &eden::engine::EngineProfile::downloadUpdated);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(80, 315));
    QTRY_COMPARE_WITH_TIMEOUT(downloadStarted.size(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!downloadUpdated.isEmpty(), 10000);
    const QString targetPath = downloadStarted.constFirst().at(3).toString();
    QCOMPARE(QFileInfo(targetPath).absolutePath(), QFileInfo(downloadDirectory).absoluteFilePath());
    QCOMPARE(QFileInfo(targetPath).fileName(), QString("eden-handler (1).txt"));
    QFile preservedDownload(existingDownloadPath);
    QVERIFY(preservedDownload.open(QIODevice::ReadOnly));
    QCOMPARE(preservedDownload.readAll(), QByteArray("preserve"));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(targetPath), 10000);

    const QImage screenshot = window.grabWindow();
    QVERIFY(!screenshot.isNull());
    QVERIFY(screenshot.pixelColor(430, 220) != screenshot.pixelColor(10, 10));
    const QString screenshotPath = qEnvironmentVariable("EDEN_E35_SCREENSHOT");
    if (!screenshotPath.isEmpty()) {
        QVERIFY(screenshot.save(screenshotPath));
    }

    const QUrl failedUrl("http://127.0.0.1:1/unavailable");
    source.load(failedUrl);
    QTRY_COMPARE_WITH_TIMEOUT(source.title(), QString("Page unavailable"), 15000);
    QCOMPARE(source.url(), failedUrl);

    source.load(mainUrl);
    QTRY_COMPARE_WITH_TIMEOUT(source.title(), QString("E35 Main"), 15000);
    rejectPopup = true;
    const int requestsBeforeReject = popupRequests;
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(110, 115));
    QTRY_COMPARE_WITH_TIMEOUT(popupRequests, requestsBeforeReject + 1, 10000);
    QTest::qWait(100);

    const auto createPendingView = [&](const QUrl &url) {
        auto pendingView = std::make_unique<eden::engine::cef::CefEngineView>(&profile);
        pendingView->attach(&viewport);
        pendingView->load(url);
        return pendingView;
    };
    source.attach(nullptr);
    {
        auto pendingDialogView = createPendingView(QUrl(QString("http://localhost:%1/").arg(m_server.serverPort())));
        QTRY_COMPARE_WITH_TIMEOUT(pendingDialogView->title(), QString("E35 Main"), 15000);
        QSignalSpy pendingDialog(pendingDialogView.get(), &eden::engine::EngineView::javaScriptDialogRequested);
        QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(110, 185));
        QTRY_COMPARE_WITH_TIMEOUT(pendingDialog.size(), 1, 10000);
        pendingDialogView.reset();
        QTest::qWait(100);
    }
    {
        auto pendingPermissionView =
            createPendingView(QUrl(QString("http://127.0.0.2:%1/").arg(m_server.serverPort())));
        QTRY_COMPARE_WITH_TIMEOUT(pendingPermissionView->title(), QString("E35 Main"), 15000);
        QSignalSpy pendingPermission(pendingPermissionView.get(), &eden::engine::EngineView::permissionRequested);
        QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(110, 255));
        QTRY_COMPARE_WITH_TIMEOUT(pendingPermission.size(), 1, 10000);
        pendingPermissionView.reset();
        QTest::qWait(100);
    }
    {
        auto pendingContextView = createPendingView(QUrl(QString("http://127.0.0.3:%1/").arg(m_server.serverPort())));
        QTRY_COMPARE_WITH_TIMEOUT(pendingContextView->title(), QString("E35 Main"), 15000);
        QSignalSpy pendingContext(pendingContextView.get(), &eden::engine::EngineView::contextMenuRequested);
        QTest::mouseClick(&window, Qt::RightButton, {}, QPoint(430, 220));
        QTRY_COMPARE_WITH_TIMEOUT(pendingContext.size(), 1, 10000);
        pendingContextView.reset();
        QTest::qWait(100);
    }
}

void CefHandlersTest::formEvents() {
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::Cef;
    parameters.privateProfile = true;
    eden::engine::cef::CefProfile profile(parameters);
    QQuickWindow window;
    window.resize(700, 400);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *x11Application = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    QVERIFY(x11Application);
    Display *display = x11Application->display();
    QVERIFY(display);
    XSetInputFocus(display, static_cast<Window>(window.winId()), RevertToParent, CurrentTime);
    XSync(display, False);
    QTRY_VERIFY_WITH_TIMEOUT(window.isActive(), 5000);
    QQuickItem viewport(window.contentItem());
    viewport.setSize(window.size());
    eden::engine::cef::CefEngineView view(&profile);
    QSignalSpy reports(&view, &eden::engine::EngineView::formFieldFocused);
    view.attach(&viewport);
    view.load(QUrl(QString("http://127.0.0.1:%1/form-events").arg(m_server.serverPort())));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("Synthetic inputs complete"), 15000);
    QTest::qWait(100);
    QCOMPARE(reports.size(), 0);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(110, 44));
    QVERIFY(!viewport.childItems().isEmpty());
    viewport.childItems().constFirst()->forceActiveFocus(Qt::MouseFocusReason);
    QTest::keyClick(&window, Qt::Key_A);
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("todo:a"), 5000);
    QCOMPARE(reports.size(), 0);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(110, 114));
    QTRY_VERIFY_WITH_TIMEOUT(!reports.isEmpty(), 5000);
    QCOMPARE(reports.constLast().constFirst().toMap().value("type").toString(), QString("email"));
    QTest::keyClick(&window, Qt::Key_A);
    QTest::keyClick(&window, Qt::Key_B);
    QTRY_COMPARE_WITH_TIMEOUT(reports.constLast().constFirst().toMap().value("value").toString(), QString("ab"), 5000);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(110, 44));
    QTRY_COMPARE_WITH_TIMEOUT(reports.constLast().constFirst().toMap().value("type").toString(), QString(), 5000);
}

void CefHandlersTest::browserVisibility() {
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::Cef;
    parameters.privateProfile = true;
    eden::engine::cef::CefProfile profile(parameters);
    QQuickWindow window;
    window.resize(700, 400);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQuickItem container(window.contentItem());
    QQuickItem viewport(&container);
    viewport.setSize(window.size());
    eden::engine::cef::CefEngineView view(&profile);
    view.attach(&viewport);
    view.load(QUrl(QString("http://127.0.0.1:%1/visibility").arg(m_server.serverPort())));
    QTRY_VERIFY_WITH_TIMEOUT(view.title().startsWith("visible:"), 15000);
    QTRY_VERIFY(view.title().section(':', 1).toInt() > 2);

    const auto verifyHidden = [&view] {
        QTRY_VERIFY_WITH_TIMEOUT(view.title().startsWith("hidden:"), 5000);
        const QString stopped = view.title();
        QTest::qWait(250);
        QCOMPARE(view.title(), stopped);
    };
    container.setVisible(false);
    verifyHidden();
    container.setVisible(true);
    QTRY_VERIFY_WITH_TIMEOUT(view.title().startsWith("visible:"), 5000);
    window.setVisibility(QWindow::Minimized);
    verifyHidden();
    window.showNormal();
    QTRY_VERIFY_WITH_TIMEOUT(view.title().startsWith("visible:"), 5000);
    view.attach(nullptr);
    verifyHidden();
    view.attach(&viewport);
    QTRY_VERIFY_WITH_TIMEOUT(view.title().startsWith("visible:"), 5000);

    QQuickItem backgroundViewport(window.contentItem());
    backgroundViewport.setSize(window.size());
    backgroundViewport.setVisible(false);
    eden::engine::cef::CefEngineView background(&profile);
    background.attach(&backgroundViewport);
    background.load(QUrl(QString("http://127.0.0.1:%1/visibility").arg(m_server.serverPort())));
    QTRY_VERIFY_WITH_TIMEOUT(background.title().startsWith("hidden:"), 15000);
    const QString stopped = background.title();
    QTest::qWait(250);
    QCOMPARE(background.title(), stopped);
    backgroundViewport.setVisible(true);
    QTRY_VERIFY_WITH_TIMEOUT(background.title().startsWith("visible:"), 5000);
}

void CefHandlersTest::addressAutofill() {
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::Cef;
    parameters.privateProfile = true;
    eden::engine::cef::CefProfile profile(parameters);
    QQuickWindow window;
    window.resize(700, 400);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQuickItem viewport(window.contentItem());
    viewport.setSize(window.size());
    eden::engine::cef::CefEngineView view(&profile);
    view.attach(&viewport);
    view.load(QUrl(QString("http://127.0.0.1:%1/address-form").arg(m_server.serverPort())));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("Address form"), 15000);
    view.fillForm({{"email", "person@example.test"}, {"addressLine1", "12 Test Street"}, {"city", "Halifax"}});
    QTRY_COMPARE_WITH_TIMEOUT(
        view.title(),
        QString("person@example.test|12 Test Street|Halifax:input,change,input,change,input,change"),
        5000
    );
}

void CefHandlersTest::clipboardPermissions() {
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::Cef;
    parameters.privateProfile = true;
    eden::engine::cef::CefProfile profile(parameters);
    QQuickWindow window;
    window.resize(700, 400);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *x11Application = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    QVERIFY(x11Application);
    Display *display = x11Application->display();
    QVERIFY(display);
    XSetInputFocus(display, static_cast<Window>(window.winId()), RevertToParent, CurrentTime);
    XSync(display, False);
    QTRY_VERIFY_WITH_TIMEOUT(window.isActive(), 5000);
    QQuickItem viewport(window.contentItem());
    viewport.setSize(window.size());
    eden::engine::cef::CefEngineView view(&profile);
    view.attach(&viewport);
    QGuiApplication::clipboard()->setText("Original clipboard text");
    view.load(QUrl(QString("http://127.0.0.1:%1/clipboard-denied").arg(m_server.serverPort())));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("Clipboard denied"), 15000);
    QTest::qWait(200);
    QCOMPARE(QGuiApplication::clipboard()->text(), QString("Original clipboard text"));
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(110, 44));
    QVERIFY(!viewport.childItems().isEmpty());
    QQuickItem *osrItem = viewport.childItems().constFirst();
    osrItem->forceActiveFocus(Qt::MouseFocusReason);
    QTRY_VERIFY_WITH_TIMEOUT(osrItem->hasActiveFocus(), 5000);
    QTest::keyClick(&window, Qt::Key_A, Qt::ControlModifier);
    QTest::qWait(100);
    QTest::keyClick(&window, Qt::Key_C, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(QGuiApplication::clipboard()->text(), QString("Copied by user"), 5000);
}

void CefHandlersTest::navigationEvents() {
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::Cef;
    parameters.privateProfile = true;
    eden::engine::cef::CefProfile profile(parameters);
    QQuickWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQuickItem viewport(window.contentItem());
    viewport.setSize(window.size());
    eden::engine::cef::CefEngineView view(&profile);
    view.attach(&viewport);
    const QUrl page(QString("http://127.0.0.1:%1/events").arg(m_server.serverPort()));
    view.load(page);
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("Navigation events"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading() && !view.faviconUrl().isEmpty(), 15000);
    QTest::qWait(250);
    const QUrl favicon = view.faviconUrl();
    QSignalSpy loading(&view, &eden::engine::EngineView::loadingChanged);
    QSignalSpy progress(&view, &eden::engine::EngineView::loadProgressChanged);
    QSignalSpy urls(&view, &eden::engine::EngineView::urlChanged);
    QSignalSpy titles(&view, &eden::engine::EngineView::titleChanged);
    QSignalSpy favicons(&view, &eden::engine::EngineView::faviconUrlChanged);
    const int completedRuns = m_server.completedFrameRuns();
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(110, 44));
    QTRY_COMPARE_WITH_TIMEOUT(m_server.completedFrameRuns(), completedRuns + 1, 15000);
    QTest::qWait(250);
    qInfo(
        "Frame activity: loading=%lld progress=%lld url=%lld title=%lld favicon=%lld",
        static_cast<long long>(loading.size()),
        static_cast<long long>(progress.size()),
        static_cast<long long>(urls.size()),
        static_cast<long long>(titles.size()),
        static_cast<long long>(favicons.size())
    );
    QCOMPARE(view.url(), page);
    QCOMPARE(view.title(), QString("Navigation events"));
    QCOMPARE(view.faviconUrl(), favicon);
    QCOMPARE(loading.size(), 0);
    QCOMPARE(progress.size(), 0);
    QCOMPARE(urls.size(), 0);
    QCOMPARE(titles.size(), 0);
    QCOMPARE(favicons.size(), 0);

    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(110, 100));
    QUrl fragment = page;
    fragment.setFragment("section");
    QTRY_COMPARE_WITH_TIMEOUT(view.url(), fragment, 10000);
    QTest::qWait(100);
    QCOMPARE(loading.size(), 0);
    QCOMPARE(progress.size(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(view.canGoBack(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.navigationHistory(-1).isEmpty(), 10000);
    view.back();
    QTRY_COMPARE_WITH_TIMEOUT(view.url(), page, 10000);
    QCOMPARE(loading.size(), 0);

    const qsizetype historyEntries = view.navigationHistory(-1).size();
    const qsizetype urlChanges = urls.size();
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(310, 114));
    QTRY_COMPARE_WITH_TIMEOUT(view.navigationHistory(-1).size(), historyEntries + 1, 10000);
    QCOMPARE(urls.size(), urlChanges);
    QCOMPARE(loading.size(), 0);
    view.back();
    QTRY_COMPARE_WITH_TIMEOUT(view.navigationHistory(-1).size(), historyEntries, 10000);
    QCOMPARE(urls.size(), urlChanges);
    QCOMPARE(loading.size(), 0);

    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(310, 44));
    QUrl stateUrl = page;
    stateUrl.setQuery("state=2");
    QTRY_COMPARE_WITH_TIMEOUT(view.url(), stateUrl, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("Updated navigation"), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.faviconUrl().isEmpty() && view.faviconUrl() != favicon, 10000);
    QCOMPARE(loading.size(), 0);
    QCOMPARE(progress.size(), 0);
    view.back();
    QTRY_COMPARE_WITH_TIMEOUT(view.url(), page, 10000);
    QCOMPARE(loading.size(), 0);

    QSignalSpy fileDialogs(&view, &eden::engine::EngineView::fileDialogRequested);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(50, 180));
    QTRY_COMPARE_WITH_TIMEOUT(fileDialogs.size(), 1, 10000);
    const eden::engine::FileDialogInfo fileDialog =
        fileDialogs.constFirst().constFirst().value<eden::engine::FileDialogInfo>();
    view.resolveFileDialog(fileDialog.id, false, {});

    view.reload();
    QTRY_COMPARE_WITH_TIMEOUT(loading.size(), 2, 10000);
    QVERIFY(!view.isLoading());
    QCOMPARE(view.loadProgress(), 100);
    loading.clear();
    view.load(QUrl(QString("http://127.0.0.1:%1/events-redirect").arg(m_server.serverPort())));
    QTRY_COMPARE_WITH_TIMEOUT(loading.size(), 2, 10000);
    QCOMPARE(view.url(), page);
    QVERIFY(!view.isLoading());
    loading.clear();
    const int pendingNavigations = m_server.pendingNavigations();
    view.load(QUrl(QString("http://127.0.0.1:%1/events-pending").arg(m_server.serverPort())));
    QTRY_COMPARE_WITH_TIMEOUT(m_server.pendingNavigations(), pendingNavigations + 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(view.isLoading(), 10000);
    view.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 10000);
    QCOMPARE(loading.size(), 2);
    view.load(QUrl("http://127.0.0.1:1/"));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("Page unavailable"), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 10000);
    QCOMPARE(view.url(), QUrl("http://127.0.0.1:1/"));
}

void CefHandlersTest::resizeStress() {
    eden::engine::EngineProfileParameters privateParameters;
    privateParameters.backend = eden::engine::Backend::Cef;
    privateParameters.privateProfile = true;
    eden::engine::cef::CefProfile profile(privateParameters);
    QQuickWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQuickItem viewport(window.contentItem());
    viewport.setSize(window.size());
    eden::engine::cef::CefEngineView source(&profile);
    source.attach(&viewport);
    source.load(QUrl(QString("http://127.0.0.1:%1/").arg(m_server.serverPort())));
    QTRY_COMPARE_WITH_TIMEOUT(source.title(), QString("E35 Main"), 15000);
    QTRY_COMPARE_WITH_TIMEOUT(source.loadProgress(), 100, 15000);

    const std::array<QSize, 8> sizes{
        QSize(1000, 700),
        QSize(1500, 900),
        QSize(920, 640),
        QSize(1420, 840),
        QSize(1000, 700),
        QSize(1280, 760),
        QSize(940, 620),
        QSize(1000, 700),
    };
    for (int iteration = 0; iteration < 12; ++iteration) {
        for (const QSize &size : sizes) {
            window.resize(size);
            viewport.setSize(size);
            QTest::qWait(16);
        }
    }

    window.resize(1000, 700);
    viewport.setSize(window.size());
    QTest::qWait(250);
    const QImage screenshot = window.grabWindow();
    QVERIFY(!screenshot.isNull());
    QCOMPARE(screenshot.size(), QSize(1000, 700));
    QVERIFY(screenshot.pixelColor(430, 220) != screenshot.pixelColor(10, 10));
}

void CefHandlersTest::devToolsSuite() {
    eden::engine::EngineProfileParameters privateProfileParameters;
    privateProfileParameters.backend = eden::engine::Backend::Cef;
    privateProfileParameters.privateProfile = true;
    eden::engine::cef::CefProfile profile(privateProfileParameters);
    QQuickWindow window;
    window.resize(1200, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQuickItem pageViewport(window.contentItem());
    pageViewport.setSize(QSizeF(700, 700));
    QQuickItem devToolsViewport(window.contentItem());
    devToolsViewport.setX(700);
    devToolsViewport.setSize(QSizeF(500, 700));
    eden::engine::cef::CefEngineView source(&profile);
    source.attach(&pageViewport);
    source.load(QUrl(QString("http://127.0.0.1:%1/").arg(m_server.serverPort())));
    QTRY_COMPARE_WITH_TIMEOUT(source.title(), QString("E35 Main"), 15000);
    QVERIFY(source.capabilities() & eden::engine::EngineView::DockedDevtools);
    source.openDevTools();
    source.attachDevTools(&devToolsViewport);
    QVERIFY(source.devToolsOpen());
    QTRY_COMPARE_WITH_TIMEOUT(eden::engine::cef::sharedDevToolsSocketServer()->sessionCount(), 1, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(eden::engine::cef::sharedDevToolsSocketServer()->connectedSessionCount(), 1, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!devToolsViewport.childItems().isEmpty(), 10000);

    QImage dockedImage;
    const QString devToolsScreenshotPath = qEnvironmentVariable("EDEN_E36_SCREENSHOT");
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&] {
            dockedImage = window.grabWindow();
            return !dockedImage.isNull() && hasColorVariation(dockedImage, QRect(900, 40, 300, 350), 300);
        })(),
        15000
    );
    if (!devToolsScreenshotPath.isEmpty()) {
        QVERIFY(dockedImage.save(devToolsScreenshotPath));
    }

    QQuickWindow separateWindow;
    separateWindow.resize(700, 600);
    separateWindow.show();
    QVERIFY(QTest::qWaitForWindowExposed(&separateWindow));
    QQuickItem separateViewport(separateWindow.contentItem());
    separateViewport.setSize(separateWindow.size());
    source.attachDevTools(&separateViewport);
    QTRY_VERIFY_WITH_TIMEOUT(!separateViewport.childItems().isEmpty(), 10000);
    QImage separateImage;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&] {
            separateImage = separateWindow.grabWindow();
            return !separateImage.isNull() && hasColorVariation(separateImage, QRect(200, 40, 500, 350), 300);
        })(),
        15000
    );

    source.attachDevTools(&devToolsViewport);
    QTRY_VERIFY_WITH_TIMEOUT(!devToolsViewport.childItems().isEmpty(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&] {
            dockedImage = window.grabWindow();
            return !dockedImage.isNull() && hasColorVariation(dockedImage, QRect(900, 40, 300, 350), 300);
        })(),
        15000
    );

    source.closeDevTools();
    QVERIFY(!source.devToolsOpen());
    QTRY_COMPARE_WITH_TIMEOUT(eden::engine::cef::sharedDevToolsSocketServer()->sessionCount(), 0, 10000);
    QVERIFY(!eden::engine::cef::sharedDevToolsSocketServer()->isListening());
}

void CefHandlersTest::shellDevToolsSuite() {
    QQmlApplicationEngine qmlEngine;
    m_applicationContext->profiles()->setQmlEngine(&qmlEngine);
    qmlEngine.rootContext()->setContextProperty("startupPrivateWindow", true);
    qmlEngine.rootContext()->setContextProperty("startupEngineName", QString("cef"));
    qmlEngine.loadFromModule("Eden.Ui", "BrowserWindow");
    QTRY_COMPARE_WITH_TIMEOUT(qmlEngine.rootObjects().size(), 1, 10000);
    auto *window = qobject_cast<QQuickWindow *>(qmlEngine.rootObjects().constFirst());
    QVERIFY(window);
    QTRY_VERIFY_WITH_TIMEOUT(window->isVisible(), 10000);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto *controller = window->findChild<eden::core::WindowController *>("windowController");
    QVERIFY(controller);
    eden::test::ProfileHarness harness;
    QVERIFY(harness.create(&qmlEngine));
    controller->initialize(harness.context, true, "cef", true);
    eden::engine::EngineView *view = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&] {
            view = qobject_cast<eden::engine::EngineView *>(controller->currentEngine());
            return view;
        })(),
        10000
    );
    view->load(QUrl(QString("http://127.0.0.1:%1/").arg(m_server.serverPort())));
    QTRY_COMPARE_WITH_TIMEOUT(view->title(), QString("E35 Main"), 15000);

    bool devToolsShortcutFound = false;
    bool commandPaletteShortcutFound = false;
    for (int row = 0; row < controller->shortcuts()->rowCount(); ++row) {
        const QModelIndex shortcutIndex = controller->shortcuts()->index(row);
        const QString shortcutId =
            controller->shortcuts()->data(shortcutIndex, eden::core::ShortcutRegistry::IdRole).toString();
        if (shortcutId == "devtools") {
            QCOMPARE(
                controller->shortcuts()->data(shortcutIndex, eden::core::ShortcutRegistry::ShortcutRole).toString(),
                QString("F12")
            );
            devToolsShortcutFound = true;
        } else if (shortcutId == "command_palette") {
            QCOMPARE(
                controller->shortcuts()->data(shortcutIndex, eden::core::ShortcutRegistry::ShortcutRole).toString(),
                QString("Ctrl+K")
            );
            commandPaletteShortcutFound = true;
        }
    }
    QVERIFY(devToolsShortcutFound);
    QVERIFY(commandPaletteShortcutFound);
    controller->shortcuts()->execute("devtools");
    QTRY_VERIFY_WITH_TIMEOUT(view->devToolsOpen(), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(eden::engine::cef::sharedDevToolsSocketServer()->connectedSessionCount(), 1, 15000);
    QImage dockedRight;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&] {
            dockedRight = window->grabWindow();
            return hasColorVariation(dockedRight, QRect(900, 140, 400, 500), 300);
        })(),
        15000
    );

    view->toggleDevToolsOrientation();
    QCOMPARE(view->devToolsPlacement(), eden::engine::EngineView::DevToolsBottom);
    QImage dockedBottom;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&] {
            dockedBottom = window->grabWindow();
            return hasColorVariation(dockedBottom, QRect(400, 560, 700, 250), 300);
        })(),
        15000
    );

    const auto visibleWindowCount = [] {
        const QWindowList windows = QGuiApplication::topLevelWindows();
        return std::count_if(windows.cbegin(), windows.cend(), [](QWindow *candidate) {
            return candidate->isVisible();
        });
    };
    const int originalWindowCount = visibleWindowCount();
    view->toggleDevToolsSeparate();
    QCOMPARE(view->devToolsPlacement(), eden::engine::EngineView::DevToolsSeparate);
    QTRY_VERIFY_WITH_TIMEOUT(visibleWindowCount() > originalWindowCount, 10000);
    view->toggleDevToolsSeparate();
    QCOMPARE(view->devToolsPlacement(), eden::engine::EngineView::DevToolsBottom);
    QTRY_COMPARE_WITH_TIMEOUT(eden::engine::cef::sharedDevToolsSocketServer()->connectedSessionCount(), 1, 10000);

    const QImage beforeOverlay = window->grabWindow();
    controller->shortcuts()->execute("command_palette");
    QTRY_VERIFY_WITH_TIMEOUT(controller->commandPaletteVisible(), 10000);
    QImage withOverlay;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&] {
            withOverlay = window->grabWindow();
            return differingPixels(beforeOverlay, withOverlay, QRect(700, 220, 250, 400)) > 1000;
        })(),
        10000
    );
    const QString shellScreenshotPath = qEnvironmentVariable("EDEN_E36_SHELL_SCREENSHOT");
    if (!shellScreenshotPath.isEmpty()) {
        QVERIFY(withOverlay.save(shellScreenshotPath));
    }

    const int activeIndex = controller->activeIndex();
    controller->newTab(QUrl("about:blank"), true, "cef");
    controller->closeTab(activeIndex);
    QVERIFY(!view->devToolsOpen());
    QTRY_COMPARE_WITH_TIMEOUT(eden::engine::cef::sharedDevToolsSocketServer()->sessionCount(), 0, 10000);
    QVERIFY(!eden::engine::cef::sharedDevToolsSocketServer()->isListening());
    window->close();
}

void CefHandlersTest::cleanupTestCase() {
    eden::engine::cef::CefRuntime::instance().shutdown();
}

int main(int argc, char *argv[]) {
    eden::engine::cef::CefRuntime &runtime = eden::engine::cef::CefRuntime::instance();
    const int processExitCode = runtime.executeProcess(argc, argv);
    if (processExitCode >= 0) {
        return processExitCode;
    }
    QGuiApplication application(argc, argv);
    eden::engine::EngineFactory::configureApplicationArguments(argc, argv);
    CefHandlersTest test;
    QStringList testArguments;
    for (const QString &argument : QCoreApplication::arguments()) {
        if (!argument.startsWith("--")) {
            testArguments.append(argument);
        }
    }
    return QTest::qExec(&test, testArguments);
}

#include "cefhandlers_test.moc"

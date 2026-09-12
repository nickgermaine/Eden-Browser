#include <libsecret/secret.h>

#include "autofilltargetchecks.h"
#include "downloadchecks.h"
#if EDEN_ENABLE_AUTOMATION
#include "core/automation/performancemetrics.h"
#endif
#include "core/profiles/applicationcontext.h"
#include "core/profiles/enginestorage.h"
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
#include "engine/cef/cefuibridge.h"
#include "engine/cef/devtoolssocketserver.h"
#include "engine/enginefactory.h"
#include "engine/engineregistry.h"
#include "include/capi/cef_task_capi.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_task.h"
#include "profiletesthelpers.h"

#include <QBuffer>
#include <QClipboard>
#include <QColor>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHoverEvent>
#include <QImage>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
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
#include <QTextCharFormat>
#include <QTimer>
#include <QtGui/qguiapplication_platform.h>
#include <QtTest>

#include <X11/Xlib.h>

#undef KeyPress
#undef KeyRelease

#include <array>
#include <atomic>
#include <condition_variable>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

static thread_local bool rejectNextCefTask = false;

extern "C" int cef_post_task(cef_thread_id_t threadId, cef_task_t *task) {
    if (std::exchange(rejectNextCefTask, false)) {
        if (task) {
            task->base.release(&task->base);
        }
        return false;
    }
    static const auto post = reinterpret_cast<decltype(&cef_post_task)>(dlsym(RTLD_NEXT, "cef_post_task"));
    if (!post) {
        qFatal("CEF task posting is unavailable");
    }
    return post(threadId, task);
}

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
                    if (path.startsWith("/gc-frame")) {
                        responseBody = R"HTML(<!doctype html><title>GC ready</title><iframe
style="position:absolute;left:0;top:0;width:480px;height:300px;border:0" src="/gc-page?busy"></iframe>)HTML";
                    } else if (path.startsWith("/gc-page")) {
                        responseBody = R"HTML(<!doctype html><title>GC ready</title><body style="margin:0">
<button style="position:absolute;left:20px;top:20px;width:120px;height:40px"
onclick="window.gcBusy=false;document.title='GC quiet'">Stop work</button>
<input style="position:absolute;left:20px;top:100px;width:300px;height:40px">
<script>
window.gcBusy=location.search.includes('busy');
if(location.search.includes('memory')){window.liveAllocation=new Array(2000000).fill(0.25);}
function work(){
if(!window.gcBusy){return;}
var end=performance.now()+35;
while(performance.now()<end){}
setTimeout(work,5);
}
if(window.gcBusy){setTimeout(work,0);}
</script>)HTML";
                    } else if (path.startsWith("/ime-frame?")) {
                        const QByteArray nested = QByteArray("/ime?") + path.sliced(path.indexOf('?') + 1);
                        responseBody = QByteArray(
                                           "<!doctype html><title>Input ready</title><body style='margin:0'><iframe "
                                           "style='position:absolute;left:60px;top:60px;width:520px;height:380px;"
                                           "border:0' src='http://127.0.0.2:"
                                       ) +
                                       QByteArray::number(serverPort()) + nested + "'></iframe></body>";
                    } else if (path.startsWith("/ime?")) {
                        m_inputPage = QString::fromUtf8(path);
                        m_inputState = {};
                        responseBody = R"HTML(<!doctype html><title>Input ready</title>
<body style="margin:0;background:#214365">
<input id="editor" style="position:absolute;left:20px;top:20px;width:400px;height:40px;font:20px sans-serif">
<button style="position:absolute;left:20px;top:90px;width:160px;height:40px">Leave editor</button>
<input readonly style="position:absolute;left:20px;top:160px;width:400px;height:40px">
<input type="PASSWORD" style="position:absolute;left:20px;top:230px;width:400px;height:40px">
<script>
const editor = document.getElementById('editor');
let composing = false;
const events = [];
let pending = Promise.resolve();
function report() {
    const selection = getSelection();
    const range = editor.isContentEditable && selection.rangeCount ? selection.getRangeAt(0).cloneRange() : null;
    if(range){range.collapse(false);}
    const caret = range ? range.getBoundingClientRect() : null;
    const body = JSON.stringify({page:location.pathname+location.search,
        value:editor.isContentEditable ? editor.textContent : editor.value,
        cursor:editor.isContentEditable ? selection.focusOffset : editor.selectionStart,
        caret:caret ? {x:caret.x,y:caret.y,width:caret.width,height:caret.height} : null,
        composing, events, ready:true});
    pending = pending.then(()=>fetch('/ime-state', {method:'POST', body}));
}
editor.addEventListener('compositionstart', ()=>{composing=true;events.push('start');report();});
editor.addEventListener('compositionend', ()=>{composing=false;events.push('end');report();});
editor.addEventListener('input', report);
document.addEventListener('selectionchange', report);
report();
</script>)HTML";
                        if (path.contains("rtl-caret")) {
                            responseBody.replace(
                                "<input id=\"editor\" style=\"position:absolute;left:20px;top:20px;width:400px;"
                                "height:40px;font:20px sans-serif\">",
                                "<div id=\"editor\" contenteditable dir=\"rtl\" style=\"position:absolute;left:20px;"
                                "top:20px;width:400px;height:40px;font:20px sans-serif;background:white\"></div>"
                            );
                        }
                    } else if (path == "/ime-state") {
                        const QJsonObject state = QJsonDocument::fromJson(body).object();
                        if (state.value("page").toString() == m_inputPage) {
                            m_inputState = state;
                        }
                        responseBody = "received";
                    } else if (path == "/thumbnail") {
                        responseBody = R"HTML(<!doctype html><title>Thumbnail ready</title>
<body style="margin:0;background:#cc2233">
<div style="position:fixed;left:40%;top:40%;width:20%;height:20%;background:#22bb66"></div>)HTML";
                    } else if (path.startsWith("/select-popup")) {
                        const QByteArray placement =
                            path.contains("edge=1") ? "right:4px;bottom:4px" : "left:40px;top:40px";
                        responseBody = R"HTML(<!doctype html><title>Select ready</title>
<body style="margin:0;background:#214365">
<select style="position:absolute;width:180px;height:40px;font:20px sans-serif;EDEN_POSITION">
<option>First option</option>
<option style="background:#e1a023;color:#102030">Second option with enough text to widen the menu</option>
<option style="background:#29b372;color:#102030">Third option</option>
<option>Fourth option</option><option>Fifth option</option><option>Sixth option</option>
</select><script>
document.querySelector('select').onchange=event=>{
    document.title='selection:'+event.target.selectedIndex;
    fetch('/select-result?index='+event.target.selectedIndex);
};
</script>)HTML";
                        responseBody.replace("EDEN_POSITION", placement);
                    } else if (path.startsWith("/select-result")) {
                        m_selectedOptions.append(path.sliced(path.indexOf('=') + 1).toInt());
                        responseBody = "selected";
                    } else if (path == "/paint-burst") {
                        responseBody = R"HTML(<!doctype html><title>Paint ready</title>
<body style="margin:0;background:#dd2222">
<div id="left" style="position:fixed;left:0;top:0;width:50vw;height:100vh;background:#dd2222"></div>
<div id="right" style="position:fixed;left:50vw;top:0;width:50vw;height:100vh;background:#dd2222"></div>
<button style="position:relative;width:180px;height:70px">Paint burst</button>
<script>
document.querySelector('button').onclick = () => {
    let frames = 0;
    function paint() {
        frames++;
        const panel = document.querySelector(frames % 2 ? '#left' : '#right');
        panel.style.background = frames >= 11 ? '#2255dd' : (frames % 3 ? '#22dd55' : '#dd5522');
        if (frames === 12) { document.title = 'Final paint'; }
        else { requestAnimationFrame(paint); }
    }
    requestAnimationFrame(paint);
};
</script>)HTML";
                    } else if (path == "/form-events") {
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
                    } else if (path == "/fullscreen") {
                        responseBody =
                            R"HTML(<!doctype html><title>fullscreen-ready</title><body style="background:#315d32;color:white;min-height:100vh">Click to enter fullscreen<script>
document.addEventListener('click',()=>document.documentElement.requestFullscreen(),{once:true});
document.addEventListener('fullscreenchange',()=>document.title=document.fullscreenElement?'fullscreen-active':'fullscreen-exited');
</script></body>)HTML";
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
                    } else if (path.startsWith("/clipboard-writes") || path == "/clipboard-child") {
                        responseBody = R"HTML(<!doctype html><title>Clipboard ready</title>
<body style="margin:0;background:#438521">
<button style="position:absolute;left:20px;top:20px;width:240px;height:48px">Copy</button>
<script>
let attempt=0;
const child=location.pathname==='/clipboard-child';
const prefix=child?'Frame ':'';
document.querySelector('button').onclick=async()=>{
    const current=++attempt;
    try {
        if(current===1){await navigator.clipboard.write([new ClipboardItem({'text/plain':new Blob([prefix+'Blob text'],{type:'text/plain'})})]);}
        else {await navigator.clipboard.writeText(current===2?prefix+'Plain text':'');}
        document.title='Copied:'+current;
    } catch(error) {document.title=String(error);}
};
</script>)HTML";
                        if (path.startsWith("/clipboard-writes")) {
                            responseBody += QByteArray(
                                                "<iframe style='position:absolute;left:20px;top:100px;width:300px;"
                                                "height:180px;border:0' allow='clipboard-write' src='http://127.0.0.2:"
                                            ) +
                                            QByteArray::number(serverPort()) + "/clipboard-child'></iframe>";
                        }
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

    const QList<int> &selectedOptions() const {
        return m_selectedOptions;
    }

    const QJsonObject &inputState() const {
        return m_inputState;
    }

  private:
    QString m_inputPage;
    QJsonObject m_inputState;
    int m_rootRequests = 0;
    int m_completedFrameRuns = 0;
    int m_pendingNavigations = 0;
    QList<int> m_selectedOptions;
    QHash<QTcpSocket *, QByteArray> m_requests;
};

enum class InputScenario { Unicode, Composition, InitialCaret, RightToLeftCaret, FocusedFrameNavigation };

Q_DECLARE_METATYPE(InputScenario)

class CefHandlersTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void handlerSuite();
    void hoverEntry_data();
    void hoverEntry();
    void navigationEvents();
    void garbageCollection_data();
    void garbageCollection();
    void garbageCollectionMemory();
    void clipboardPermissions();
    void clipboardWrites_data();
    void clipboardWrites();
    void permissionDismissal();
    void addressAutofill();
    void browserVisibility();
    void formEvents();
    void credentialTargets();
    void mediaConsentAndIndicators();
    void ordinaryCookieHandoff();
    void concurrentDownloadsKeepDistinctDestinations();
    void preservesFinalPaint_data();
    void preservesFinalPaint();
    void devToolsSuite();
    void shellDevToolsSuite();
    void shellFullscreen();
    void shellCallIndicators();
    void resizeStress();
    void preservesPaintAcrossCoalescedResizes();
    void popupMenus_data();
    void popupMenus();
    void thumbnailPostFailure();
    void unicodeAndComposition_data();
    void unicodeAndComposition();
    void thumbnailCrop_data();
    void thumbnailCrop();
    void asynchronousClose_data();
    void asynchronousClose();
    void shutdownWaitsForPendingCreation();
    void cleanupTestCase();

  private:
    QTemporaryDir m_dataDirectory;
    QTemporaryDir m_environmentDirectory;
    LocalPageServer m_server;
    std::unique_ptr<eden::core::ApplicationContext> m_applicationContext;
};

class CefNavigateDevTools final : public CefTask {
  public:
    CefNavigateDevTools(QUrl url, std::shared_ptr<std::atomic_bool> requested, QUrl source = {})
        : m_url(std::move(url)),
          m_requested(std::move(requested)),
          m_source(std::move(source)) {}

    void Execute() override {
        for (int id = 1; id < 1000; ++id) {
            const CefRefPtr<CefBrowser> browser = CefBrowserHost::GetBrowserByIdentifier(id);
            const CefRefPtr<CefFrame> frame = browser ? browser->GetMainFrame() : nullptr;
            if (frame && (frame->GetURL().ToString().starts_with("devtools://") ||
                          frame->GetURL() == m_url.toString().toStdString() ||
                          (!m_source.isEmpty() && frame->GetURL() == m_source.toString().toStdString()))) {
                frame->LoadURL(m_url.toString().toStdString());
                m_requested->store(true);
                return;
            }
        }
    }

  private:
    QUrl m_url;
    std::shared_ptr<std::atomic_bool> m_requested;
    QUrl m_source;

    IMPLEMENT_REFCOUNTING(CefNavigateDevTools);
};

class CefNavigateFocusedFrame final : public CefTask {
  public:
    CefNavigateFocusedFrame(QUrl source, std::shared_ptr<std::atomic_bool> requested)
        : m_source(std::move(source)),
          m_requested(std::move(requested)) {}

    void Execute() override {
        for (int id = 1; id < 1000; ++id) {
            const CefRefPtr<CefBrowser> browser = CefBrowserHost::GetBrowserByIdentifier(id);
            const CefRefPtr<CefFrame> main = browser ? browser->GetMainFrame() : nullptr;
            if (!main || main->GetURL() != m_source.toString().toStdString()) {
                continue;
            }
            const CefRefPtr<CefFrame> focused = browser->GetFocusedFrame();
            if (focused && !focused->IsMain()) {
                focused->LoadURL("about:blank");
                m_requested->store(true);
            }
            return;
        }
    }

  private:
    QUrl m_source;
    std::shared_ptr<std::atomic_bool> m_requested;

    IMPLEMENT_REFCOUNTING(CefNavigateFocusedFrame);
};

void CefHandlersTest::hoverEntry_data() {
    QTest::addColumn<bool>("devTools");
    QTest::newRow("page") << false;
    QTest::newRow("devtools") << true;
}

void CefHandlersTest::hoverEntry() {
    QFETCH(bool, devTools);
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::Cef;
    parameters.privateProfile = true;
    eden::engine::cef::CefProfile profile(parameters);
    QQuickWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQuickItem viewport(window.contentItem());
    viewport.setPosition(QPointF(100, 100));
    viewport.setSize(QSizeF(700, 500));
    QQuickItem inspectedPage(window.contentItem());
    inspectedPage.setSize(QSizeF(40, 40));
    eden::engine::cef::CefEngineView view(&profile);
    const QUrl fixture(QString("http://127.0.0.1:%1/hover-entry").arg(m_server.serverPort()));
    if (devTools) {
        view.attach(&inspectedPage);
        view.load(QUrl(QString("http://127.0.0.1:%1/").arg(m_server.serverPort())));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("E35 Main"), 15000);
        view.openDevTools();
        view.attachDevTools(&viewport);
        QTRY_COMPARE_WITH_TIMEOUT(eden::engine::cef::sharedDevToolsSocketServer()->connectedSessionCount(), 1, 15000);
        auto requested = std::make_shared<std::atomic_bool>(false);
        QVERIFY(CefPostTask(TID_UI, new CefNavigateDevTools(fixture, requested)));
        QTRY_VERIFY_WITH_TIMEOUT(requested->load(), 5000);
    } else {
        view.attach(&viewport);
        view.load(fixture);
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("E35 Main"), 15000);
    }
    QTRY_VERIFY_WITH_TIMEOUT(!viewport.childItems().isEmpty(), 5000);
    QQuickItem *item = viewport.childItems().constFirst();
    QTRY_COMPARE_WITH_TIMEOUT(
        window.grabWindow().pixelColor(
            QPoint(qRound(105 * window.devicePixelRatio()), qRound(300 * window.devicePixelRatio()))
        ),
        QColor("#d8ead3"),
        10000
    );
    for (const QPoint &position : {QPoint(75, 370), QPoint(200, 430), QPoint(75, 370)}) {
        const QPointF global = item->mapToGlobal(position);
        QHoverEvent leave(QEvent::HoverLeave, position, global, position);
        QCoreApplication::sendEvent(item, &leave);
        QCOMPARE(item->cursor().shape(), Qt::ArrowCursor);
        QHoverEvent enter(QEvent::HoverEnter, position, global, QPointF(-1, -1));
        QCoreApplication::sendEvent(item, &enter);
        QTRY_COMPARE_WITH_TIMEOUT(
            item->cursor().shape(),
            position.y() == 370 ? Qt::PointingHandCursor : Qt::IBeamCursor,
            5000
        );
    }
}

void CefHandlersTest::unicodeAndComposition_data() {
    QTest::addColumn<bool>("devTools");
    QTest::addColumn<bool>("embedded");
    QTest::addColumn<InputScenario>("scenario");
    QTest::newRow("page-unicode") << false << false << InputScenario::Unicode;
    QTest::newRow("devtools-unicode") << true << false << InputScenario::Unicode;
    QTest::newRow("page-ime") << false << false << InputScenario::Composition;
    QTest::newRow("devtools-ime") << true << false << InputScenario::Composition;
    QTest::newRow("page-iframe-ime") << false << true << InputScenario::Composition;
    QTest::newRow("devtools-iframe-ime") << true << true << InputScenario::Composition;
    QTest::newRow("page-initial-caret") << false << false << InputScenario::InitialCaret;
    QTest::newRow("devtools-initial-caret") << true << false << InputScenario::InitialCaret;
    QTest::newRow("page-iframe-initial-caret") << false << true << InputScenario::InitialCaret;
    QTest::newRow("devtools-iframe-initial-caret") << true << true << InputScenario::InitialCaret;
    QTest::newRow("page-rtl-caret") << false << false << InputScenario::RightToLeftCaret;
    QTest::newRow("devtools-rtl-caret") << true << false << InputScenario::RightToLeftCaret;
    QTest::newRow("page-iframe-rtl-caret") << false << true << InputScenario::RightToLeftCaret;
    QTest::newRow("devtools-iframe-rtl-caret") << true << true << InputScenario::RightToLeftCaret;
    QTest::newRow("page-iframe-navigation") << false << true << InputScenario::FocusedFrameNavigation;
    QTest::newRow("devtools-iframe-navigation") << true << true << InputScenario::FocusedFrameNavigation;
}

void CefHandlersTest::unicodeAndComposition() {
    QFETCH(bool, devTools);
    QFETCH(bool, embedded);
    QFETCH(InputScenario, scenario);
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::Cef;
    parameters.privateProfile = true;
    eden::engine::cef::CefProfile profile(parameters);
    QQuickWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    QVERIFY(x11);
    XSetInputFocus(x11->display(), static_cast<Window>(window.winId()), RevertToParent, CurrentTime);
    XSync(x11->display(), False);
    QTRY_VERIFY_WITH_TIMEOUT(window.isActive(), 5000);
    QQuickItem viewport(window.contentItem());
    viewport.setPosition(QPointF(100, 100));
    viewport.setSize(QSizeF(700, 500));
    QQuickItem inspectedPage(window.contentItem());
    inspectedPage.setSize(QSizeF(40, 40));
    eden::engine::cef::CefEngineView view(&profile);
    const QUrl fixture(QString("http://127.0.0.1:%1/%2?case=%3")
                           .arg(m_server.serverPort())
                           .arg(embedded ? "ime-frame" : "ime")
                           .arg(QString::fromLatin1(QTest::currentDataTag())));
    const QPoint inputOffset = embedded ? QPoint(60, 60) : QPoint();
    if (devTools) {
        view.attach(&inspectedPage);
        view.load(QUrl(QString("http://127.0.0.1:%1/").arg(m_server.serverPort())));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("E35 Main"), 15000);
        view.openDevTools();
        view.attachDevTools(&viewport);
        QTRY_COMPARE_WITH_TIMEOUT(eden::engine::cef::sharedDevToolsSocketServer()->connectedSessionCount(), 1, 15000);
        auto requested = std::make_shared<std::atomic_bool>(false);
        QVERIFY(CefPostTask(TID_UI, new CefNavigateDevTools(fixture, requested)));
        QTRY_VERIFY_WITH_TIMEOUT(requested->load(), 5000);
    } else {
        view.attach(&viewport);
        view.load(fixture);
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("Input ready"), 15000);
    }
    QTRY_COMPARE_WITH_TIMEOUT(
        m_server.inputState().value("page").toString(),
        QString("/ime?") + fixture.query(),
        10000
    );
    QTRY_VERIFY_WITH_TIMEOUT(!viewport.childItems().isEmpty(), 5000);
    QQuickItem *item = viewport.childItems().constFirst();
    QTRY_COMPARE_WITH_TIMEOUT(
        window.grabWindow().pixelColor(QPoint(
            qRound((110 + inputOffset.x()) * window.devicePixelRatio()),
            qRound((110 + inputOffset.y()) * window.devicePixelRatio())
        )),
        QColor("#214365"),
        10000
    );
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(150, 140) + inputOffset);
    QTRY_VERIFY_WITH_TIMEOUT(item->hasActiveFocus(), 5000);
    if (scenario == InputScenario::Unicode) {
        const QString text = QString::fromUtf8("A🚀é");
        QKeyEvent press(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, text);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_unknown, Qt::NoModifier, text);
        QCoreApplication::sendEvent(&window, &press);
        QCoreApplication::sendEvent(&window, &release);
        QTRY_COMPARE_WITH_TIMEOUT(m_server.inputState().value("value").toString(), text, 5000);
        return;
    }
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&] {
            if (!item->inputMethodQuery(Qt::ImEnabled).toBool()) {
                QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(150, 140) + inputOffset);
            }
            return item->inputMethodQuery(Qt::ImEnabled).toBool();
        }()),
        5000
    );
    if (scenario == InputScenario::FocusedFrameNavigation) {
        auto requested = std::make_shared<std::atomic_bool>(false);
        QVERIFY(CefPostTask(TID_UI, new CefNavigateFocusedFrame(fixture, requested)));
        QTRY_VERIFY_WITH_TIMEOUT(requested->load(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(
            window.grabWindow().pixelColor(QPoint(
                qRound((110 + inputOffset.x()) * window.devicePixelRatio()),
                qRound((110 + inputOffset.y()) * window.devicePixelRatio())
            )),
            QColor(Qt::white),
            10000
        );
        QTRY_VERIFY_WITH_TIMEOUT(!item->inputMethodQuery(Qt::ImEnabled).toBool(), 5000);
        QInputMethodEvent ignored;
        ignored.setCommitString("ignored");
        ignored.ignore();
        QCoreApplication::sendEvent(&window, &ignored);
        QVERIFY(!ignored.isAccepted());
        return;
    }
    if (scenario == InputScenario::InitialCaret) {
        QTRY_VERIFY_WITH_TIMEOUT(
            item->inputMethodQuery(Qt::ImCursorRectangle)
                .toRectF()
                .intersects(QRectF(20 + inputOffset.x(), 20 + inputOffset.y(), 420, 50)),
            5000
        );
        return;
    }
    if (scenario == InputScenario::RightToLeftCaret) {
        const QString text = QString::fromUtf8("ببب");
        for (const int cursor : {3, 0, 1, 3}) {
            QInputMethodEvent preedit(text, {{QInputMethodEvent::Cursor, cursor, 1, {}}});
            QCoreApplication::sendEvent(&window, &preedit);
            QTRY_COMPARE_WITH_TIMEOUT(m_server.inputState().value("value").toString(), text, 5000);
            QTRY_COMPARE_WITH_TIMEOUT(m_server.inputState().value("cursor").toInt(), cursor, 5000);
            QTRY_VERIFY_WITH_TIMEOUT(
                m_server.inputState().value("caret").toObject().value("height").toDouble() > 0,
                5000
            );
            const QJsonObject expected = m_server.inputState().value("caret").toObject();
            QTRY_VERIFY_WITH_TIMEOUT(
                std::abs(
                    item->inputMethodQuery(Qt::ImCursorRectangle).toRectF().x() -
                    (expected.value("x").toDouble() + inputOffset.x())
                ) <= 2.0,
                5000
            );
            const QRectF rectangle = item->inputMethodQuery(Qt::ImCursorRectangle).toRectF();
            QVERIFY(std::abs(rectangle.y() - (expected.value("y").toDouble() + inputOffset.y())) <= 2.0);
            QVERIFY(std::abs(rectangle.height() - expected.value("height").toDouble()) <= 2.0);
        }
        QInputMethodEvent commit;
        commit.setCommitString(text);
        QCoreApplication::sendEvent(&window, &commit);
        QTRY_VERIFY_WITH_TIMEOUT(!m_server.inputState().value("composing").toBool(), 5000);
        return;
    }
    QTextCharFormat format;
    format.setFontUnderline(true);
    format.setForeground(QColor(Qt::blue));
    format.setBackground(QColor(Qt::yellow));
    QInputMethodEvent preedit(
        QString::fromUtf8("に"),
        {{QInputMethodEvent::TextFormat, 0, 1, QVariant::fromValue(QTextFormat(format))},
         {QInputMethodEvent::Cursor, 1, 1, {}}}
    );
    preedit.ignore();
    QCoreApplication::sendEvent(&window, &preedit);
    QVERIFY(preedit.isAccepted());
    QTRY_COMPARE_WITH_TIMEOUT(m_server.inputState().value("value").toString(), QString::fromUtf8("に"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(m_server.inputState().value("composing").toBool(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        item->inputMethodQuery(Qt::ImCursorRectangle)
            .toRectF()
            .intersects(QRectF(20 + inputOffset.x(), 20 + inputOffset.y(), 420, 50)),
        5000
    );
    QInputMethodEvent update(QString::fromUtf8("日本"), {{QInputMethodEvent::Cursor, 1, 1, {}}});
    QCoreApplication::sendEvent(&window, &update);
    QTRY_COMPARE_WITH_TIMEOUT(m_server.inputState().value("value").toString(), QString::fromUtf8("日本"), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(m_server.inputState().value("cursor").toInt(), 1, 5000);
    const QString committed = QString::fromUtf8("日本語🚀");
    QInputMethodEvent commit;
    commit.setCommitString(committed);
    QCoreApplication::sendEvent(&window, &commit);
    QTRY_COMPARE_WITH_TIMEOUT(m_server.inputState().value("value").toString(), committed, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!m_server.inputState().value("composing").toBool(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(item->inputMethodQuery(Qt::ImAbsolutePosition).toInt(), committed.size(), 5000);
    QInputMethodEvent replacement;
    replacement.setCommitString("!", -2, 2);
    QCoreApplication::sendEvent(&window, &replacement);
    QTRY_COMPARE_WITH_TIMEOUT(m_server.inputState().value("value").toString(), QString::fromUtf8("日本語!"), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(item->inputMethodQuery(Qt::ImAbsolutePosition).toInt(), 4, 5000);
    QInputMethodEvent removal;
    removal.setCommitString({}, -1, 1);
    QCoreApplication::sendEvent(&window, &removal);
    QTRY_COMPARE_WITH_TIMEOUT(m_server.inputState().value("value").toString(), QString::fromUtf8("日本語"), 5000);
    QInputMethodEvent cancelled(QString::fromUtf8("仮"), {});
    QCoreApplication::sendEvent(&window, &cancelled);
    QTRY_COMPARE_WITH_TIMEOUT(m_server.inputState().value("value").toString(), QString::fromUtf8("日本語仮"), 5000);
    QInputMethodEvent cancel;
    QCoreApplication::sendEvent(&window, &cancel);
    QTRY_COMPARE_WITH_TIMEOUT(m_server.inputState().value("value").toString(), QString::fromUtf8("日本語"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!m_server.inputState().value("composing").toBool(), 5000);
    QTest::keyClick(&window, Qt::Key_A, Qt::ControlModifier);
    QInputMethodEvent initial;
    initial.setCommitString("abcXYZ");
    QCoreApplication::sendEvent(&window, &initial);
    QTRY_COMPARE_WITH_TIMEOUT(m_server.inputState().value("value").toString(), QString("abcXYZ"), 5000);
    for (int index = 0; index < 3; ++index) {
        QTest::keyClick(&window, Qt::Key_Left);
    }
    QTRY_COMPARE_WITH_TIMEOUT(item->inputMethodQuery(Qt::ImAbsolutePosition).toInt(), 3, 5000);
    QInputMethodEvent crossing(QString::fromUtf8("仮"), {});
    QCoreApplication::sendEvent(&window, &crossing);
    QTRY_COMPARE_WITH_TIMEOUT(m_server.inputState().value("value").toString(), QString::fromUtf8("abc仮XYZ"), 5000);
    QInputMethodEvent replaceAcross;
    replaceAcross.setCommitString("a", -1, 2);
    QCoreApplication::sendEvent(&window, &replaceAcross);
    QTRY_COMPARE_WITH_TIMEOUT(m_server.inputState().value("value").toString(), QString("abaYZ"), 5000);
    QInputMethodEvent combined(QString::fromUtf8("仮"), {});
    combined.setCommitString("!");
    QCoreApplication::sendEvent(&window, &combined);
    QTRY_COMPARE_WITH_TIMEOUT(m_server.inputState().value("value").toString(), QString::fromUtf8("aba!仮YZ"), 5000);
    QInputMethodEvent finish;
    finish.setCommitString("done");
    QCoreApplication::sendEvent(&window, &finish);
    QTRY_COMPARE_WITH_TIMEOUT(m_server.inputState().value("value").toString(), QString("aba!doneYZ"), 5000);
    QInputMethodEvent blur(QString::fromUtf8("仮"), {});
    QCoreApplication::sendEvent(&window, &blur);
    QTRY_VERIFY_WITH_TIMEOUT(m_server.inputState().value("composing").toBool(), 5000);
    QQuickItem outside(window.contentItem());
    outside.setSize(QSizeF(20, 20));
    outside.forceActiveFocus();
    QTRY_VERIFY_WITH_TIMEOUT(!m_server.inputState().value("composing").toBool(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(m_server.inputState().value("value").toString(), QString::fromUtf8("aba!done仮YZ"), 5000);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(180, 210) + inputOffset);
    QTRY_VERIFY_WITH_TIMEOUT(!item->inputMethodQuery(Qt::ImEnabled).toBool(), 5000);
    QInputMethodEvent ignored;
    ignored.setCommitString("ignored");
    ignored.ignore();
    QCoreApplication::sendEvent(&window, &ignored);
    QVERIFY(!ignored.isAccepted());
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(180, 280) + inputOffset);
    QTRY_VERIFY_WITH_TIMEOUT(!item->inputMethodQuery(Qt::ImEnabled).toBool(), 5000);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(180, 350) + inputOffset);
    QTRY_VERIFY_WITH_TIMEOUT(item->inputMethodQuery(Qt::ImEnabled).toBool(), 5000);
    QVERIFY(item->inputMethodQuery(Qt::ImHints).toInt() & Qt::ImhHiddenText);
    QVERIFY(item->inputMethodQuery(Qt::ImHints).toInt() & Qt::ImhSensitiveData);
    QCOMPARE(m_server.inputState().value("value").toString(), QString::fromUtf8("aba!done仮YZ"));
    auto navigating = std::make_shared<std::atomic_bool>(false);
    QVERIFY(CefPostTask(TID_UI, new CefNavigateDevTools(QUrl("about:blank"), navigating, fixture)));
    QTRY_VERIFY_WITH_TIMEOUT(navigating->load(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!item->inputMethodQuery(Qt::ImEnabled).toBool(), 5000);
}

void CefHandlersTest::popupMenus_data() {
    QTest::addColumn<bool>("edge");
    QTest::addColumn<bool>("devTools");
    QTest::newRow("page-center") << false << false;
    QTest::newRow("page-bottom-right") << true << false;
    QTest::newRow("devtools-center") << false << true;
    QTest::newRow("devtools-bottom-right") << true << true;
}

void CefHandlersTest::popupMenus() {
    QFETCH(bool, edge);
    QFETCH(bool, devTools);
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::Cef;
    parameters.privateProfile = true;
    eden::engine::cef::CefProfile profile(parameters);
    QQuickWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    QVERIFY(x11);
    Display *display = x11->display();
    QVERIFY(display);
    XSetInputFocus(display, static_cast<Window>(window.winId()), RevertToParent, CurrentTime);
    XSync(display, False);
    QTRY_VERIFY_WITH_TIMEOUT(window.isActive(), 5000);
    QQuickItem viewport(window.contentItem());
    viewport.setPosition(QPointF(100, 100));
    viewport.setSize(QSizeF(700, 500));
    QQuickItem inspectedPage(window.contentItem());
    inspectedPage.setSize(QSizeF(40, 40));
    eden::engine::cef::CefEngineView view(&profile);
    const QUrl fixture(
        QString("http://127.0.0.1:%1/select-popup?edge=%2").arg(m_server.serverPort()).arg(edge ? 1 : 0)
    );
    if (devTools) {
        view.attach(&inspectedPage);
        view.load(QUrl(QString("http://127.0.0.1:%1/").arg(m_server.serverPort())));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("E35 Main"), 15000);
        view.openDevTools();
        view.attachDevTools(&viewport);
        QTRY_COMPARE_WITH_TIMEOUT(eden::engine::cef::sharedDevToolsSocketServer()->connectedSessionCount(), 1, 15000);
        auto requested = std::make_shared<std::atomic_bool>(false);
        QVERIFY(CefPostTask(TID_UI, new CefNavigateDevTools(fixture, requested)));
        QTRY_VERIFY_WITH_TIMEOUT(requested->load(), 5000);
    } else {
        view.attach(&viewport);
        view.load(fixture);
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("Select ready"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    }
    const QPoint button = edge ? QPoint(100 + 700 - 94, 100 + 500 - 24) : QPoint(230, 160);
    const qreal scale = window.devicePixelRatio();
    const auto coloredBounds = [](const QImage &image, const QColor &color) {
        QRect bounds;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (image.pixelColor(x, y) == color) {
                    bounds |= QRect(x, y, 1, 1);
                }
            }
        }
        return bounds;
    };
    QTRY_VERIFY_WITH_TIMEOUT(!coloredBounds(window.grabWindow(), QColor("#214365")).isEmpty(), 5000);
    QTest::mouseClick(&window, Qt::LeftButton, {}, button);
    QRect option;
    QImage popupImage;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&] {
            popupImage = window.grabWindow();
            option = coloredBounds(popupImage, QColor("#e1a023"));
            return option.width() > 100 * scale && option.height() > 10 * scale;
        })(),
        5000
    );
    const QRect viewportPixels(qRound(100 * scale), qRound(100 * scale), qRound(700 * scale), qRound(500 * scale));
    QVERIFY(viewportPixels.contains(option));
    QTest::keyClick(&window, Qt::Key_Escape);
    QTRY_VERIFY_WITH_TIMEOUT(coloredBounds(window.grabWindow(), QColor("#e1a023")).isEmpty(), 5000);
    QTest::mouseClick(&window, Qt::LeftButton, {}, button);
    QTRY_VERIFY_WITH_TIMEOUT(!coloredBounds(window.grabWindow(), QColor("#e1a023")).isEmpty(), 5000);
    const qsizetype previousSelections = m_server.selectedOptions().size();
    QTest::mouseClick(&window, Qt::LeftButton, {}, (QPointF(option.center()) / scale).toPoint());
    QTRY_COMPARE_WITH_TIMEOUT(m_server.selectedOptions().size(), previousSelections + 1, 5000);
    QCOMPARE(m_server.selectedOptions().constLast(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(coloredBounds(window.grabWindow(), QColor("#e1a023")).isEmpty(), 5000);
    QTest::mouseClick(&window, Qt::LeftButton, {}, button);
    QTRY_VERIFY_WITH_TIMEOUT(!coloredBounds(window.grabWindow(), QColor("#29b372")).isEmpty(), 5000);
    if (devTools) {
        auto requested = std::make_shared<std::atomic_bool>(false);
        QVERIFY(CefPostTask(TID_UI, new CefNavigateDevTools(fixture, requested)));
        QTRY_VERIFY_WITH_TIMEOUT(requested->load(), 5000);
    } else {
        view.reload();
    }
    QTRY_VERIFY_WITH_TIMEOUT(coloredBounds(window.grabWindow(), QColor("#29b372")).isEmpty(), 5000);
}

void CefHandlersTest::thumbnailPostFailure() {
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::Cef;
    parameters.privateProfile = true;
    eden::engine::cef::CefProfile profile(parameters);
    QQuickWindow window;
    window.resize(640, 480);
    QQuickItem viewport(window.contentItem());
    viewport.setSize(QSizeF(640, 480));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    eden::engine::cef::CefEngineView view(&profile);
    view.attach(&viewport);
    view.load(QUrl(QString("http://127.0.0.1:%1/").arg(m_server.serverPort())));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("E35 Main"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    int completions = 0;
    QImage captured;
    const auto completed = [&](const QImage &image) {
        ++completions;
        captured = image;
    };
    rejectNextCefTask = true;
    view.requestThumbnail(QSize(64, 48), completed);
    QVERIFY(!std::exchange(rejectNextCefTask, false));
    QCOMPARE(completions, 1);
    QVERIFY(captured.isNull());
    view.requestThumbnail(QSize(64, 48), completed);
    QTRY_COMPARE_WITH_TIMEOUT(completions, 2, 10000);
    QCOMPARE(captured.size(), QSize(64, 48));
}

void CefHandlersTest::thumbnailCrop_data() {
    QTest::addColumn<QSize>("viewportSize");
    QTest::newRow("wide") << QSize(1000, 400);
    QTest::newRow("tall") << QSize(400, 800);
}

void CefHandlersTest::thumbnailCrop() {
    QFETCH(QSize, viewportSize);
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::Cef;
    parameters.privateProfile = true;
    eden::engine::cef::CefProfile profile(parameters);
    QQuickWindow window;
    window.resize(viewportSize);
    QQuickItem viewport(window.contentItem());
    viewport.setSize(viewportSize);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    eden::engine::cef::CefEngineView view(&profile);
    view.attach(&viewport);
    view.load(QUrl(QString("http://127.0.0.1:%1/thumbnail").arg(m_server.serverPort())));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("Thumbnail ready"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    int completions = 0;
    QImage captured;
    view.requestThumbnail(QSize(200, 200), [&](const QImage &image) {
        ++completions;
        captured = image;
    });
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 10000);
    QCOMPARE(captured.size(), QSize(200, 200));
    const auto near = [](const QColor &color, const QColor &expected) {
        return std::abs(color.red() - expected.red()) <= 5 && std::abs(color.green() - expected.green()) <= 5 &&
               std::abs(color.blue() - expected.blue()) <= 5;
    };
    QVERIFY(near(captured.pixelColor(100, 100), QColor("#22bb66")));
    QVERIFY(near(captured.pixelColor(10, 10), QColor("#cc2233")));
    const QPoint inside = viewportSize.width() > viewportSize.height() ? QPoint(60, 100) : QPoint(100, 70);
    QVERIFY(near(captured.pixelColor(inside), QColor("#22bb66")));
    QVERIFY(near(captured.pixelColor(100, 155), QColor("#cc2233")));
}

class CefUiPause final : public CefTask {
  public:
    void Execute() override {
        entered.store(true);
        std::unique_lock lock(m_mutex);
        m_released.wait_for(lock, std::chrono::seconds(3), [this] { return m_resume; });
        finished.store(true);
    }

    void resume() {
        {
            const std::lock_guard lock(m_mutex);
            m_resume = true;
        }
        m_released.notify_all();
    }

    std::atomic_bool entered = false;
    std::atomic_bool finished = false;

  private:
    std::mutex m_mutex;
    std::condition_variable m_released;
    bool m_resume = false;

    IMPLEMENT_REFCOUNTING(CefUiPause);
};

void CefHandlersTest::asynchronousClose_data() {
    QTest::addColumn<QString>("scenario");
    for (const char *scenario : {"pending-creation", "loaded-page", "devtools-close", "page-with-devtools"}) {
        QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }
}

void CefHandlersTest::asynchronousClose() {
    QFETCH(QString, scenario);
    auto &runtime = eden::engine::cef::CefRuntime::instance();
    QTRY_COMPARE_WITH_TIMEOUT(runtime.browserClientCount(), std::size_t(0), 10000);
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::Cef;
    parameters.privateProfile = true;
    eden::engine::cef::CefProfile profile(parameters);
    QQuickWindow window;
    window.resize(1200, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQuickItem viewport(window.contentItem());
    viewport.setSize(QSizeF(700, 700));
    QQuickItem devToolsViewport(window.contentItem());
    devToolsViewport.setX(700);
    devToolsViewport.setSize(QSizeF(500, 700));
    auto view = std::make_unique<eden::engine::cef::CefEngineView>(&profile);
    if (scenario.contains("devtools") && !(view->capabilities() & eden::engine::EngineView::DockedDevtools)) {
        QSKIP("The selected mode has no docked developer tools");
    }
    if (scenario != "pending-creation") {
        view->attach(&viewport);
        view->load(QUrl(QString("http://127.0.0.1:%1/").arg(m_server.serverPort())));
        QTRY_COMPARE_WITH_TIMEOUT(view->title(), QString("E35 Main"), 15000);
        if (scenario.contains("devtools")) {
            view->openDevTools();
            view->attachDevTools(&devToolsViewport);
            QTRY_COMPARE_WITH_TIMEOUT(
                eden::engine::cef::sharedDevToolsSocketServer()->connectedSessionCount(),
                1,
                15000
            );
        }
    }
    CefRefPtr<CefUiPause> pause = new CefUiPause;
    QVERIFY(CefPostTask(TID_UI, pause));
    QTRY_VERIFY_WITH_TIMEOUT(pause->entered.load(), 1000);
    if (scenario == "pending-creation") {
        view->attach(&viewport);
    }
    if (scenario == "devtools-close") {
        view->closeDevTools();
    } else {
        view.reset();
    }
    const bool returnedBeforeNativeClose = !pause->finished.load();
    pause->resume();
    QVERIFY(returnedBeforeNativeClose);
    QTRY_COMPARE_WITH_TIMEOUT(runtime.browserClientCount(), std::size_t(view ? 1 : 0), 10000);
    if (view) {
        view->reload();
        QTRY_VERIFY_WITH_TIMEOUT(!view->isLoading(), 10000);
        view.reset();
        QTRY_COMPARE_WITH_TIMEOUT(runtime.browserClientCount(), std::size_t(0), 10000);
    }
}

void CefHandlersTest::shutdownWaitsForPendingCreation() {
    auto &runtime = eden::engine::cef::CefRuntime::instance();
    QTRY_COMPARE_WITH_TIMEOUT(runtime.browserClientCount(), std::size_t(0), 10000);
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::Cef;
    parameters.privateProfile = true;
    auto profile = std::make_unique<eden::engine::cef::CefProfile>(parameters);
    QQuickWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQuickItem viewport(window.contentItem());
    viewport.setSize(window.size());
    auto view = std::make_unique<eden::engine::cef::CefEngineView>(profile.get());
    CefRefPtr<CefUiPause> pause = new CefUiPause;
    QVERIFY(CefPostTask(TID_UI, pause));
    QTRY_VERIFY_WITH_TIMEOUT(pause->entered.load(), 1000);
    view->attach(&viewport);
    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    QCOMPARE(QCoreApplication::exec(), 0);
    view.reset();
    profile.reset();
    QCOMPARE(runtime.browserClientCount(), std::size_t(1));
    bool qtResponded = false;
    bool acceptedDuringShutdown = false;
    QTimer::singleShot(0, &window, [&] {
        qtResponded = true;
        acceptedDuringShutdown = runtime.registerBrowserClient(&qtResponded, [] {});
        runtime.shutdown();
        pause->resume();
    });
    runtime.shutdown();
    QVERIFY(qtResponded);
    QVERIFY(!acceptedDuringShutdown);
    QCOMPARE(runtime.browserClientCount(), std::size_t(0));
    QVERIFY(!runtime.isInitialized());
}

class CefIsolatedWorldProbe final : public CefTask, public CefDevToolsMessageObserver {
  public:
    CefIsolatedWorldProbe(QString url, std::shared_ptr<std::atomic_int> result)
        : m_url(std::move(url)),
          m_result(std::move(result)) {}

    void Execute() override {
        for (int id = 1; id < 1000; ++id) {
            CefRefPtr<CefBrowser> browser = CefBrowserHost::GetBrowserByIdentifier(id);
            if (browser && browser->GetMainFrame() && browser->GetMainFrame()->GetURL() == m_url.toStdString()) {
                m_registration = browser->GetHost()->AddDevToolsMessageObserver(this);
                const QByteArray message = R"({"id":900001,"method":"Page.getFrameTree"})";
                browser->GetHost()->SendDevToolsMessage(message.constData(), message.size());
                return;
            }
        }
        m_result->store(-1);
    }

    void OnDevToolsMethodResult(
        CefRefPtr<CefBrowser> browser,
        int messageId,
        bool success,
        const void *result,
        size_t size
    ) override {
        if (messageId != 900001 && messageId != 900002) {
            return;
        }
        if (!success || !result) {
            m_result->store(-1);
            m_registration = nullptr;
            return;
        }
        if (messageId == 900002) {
            m_result->store(1);
            m_registration = nullptr;
            return;
        }
        const QJsonObject object =
            QJsonDocument::fromJson(QByteArray(static_cast<const char *>(result), size)).object();
        const QString frameId = object.value("frameTree").toObject().value("frame").toObject().value("id").toString();
        const QByteArray message =
            QJsonDocument(
                QJsonObject{
                    {"id", 900002},
                    {"method", "Page.createIsolatedWorld"},
                    {"params", QJsonObject{{"frameId", frameId}, {"worldName", "autofill-isolated-world-regression"}}}
                }
            ).toJson(QJsonDocument::Compact);
        browser->GetHost()->SendDevToolsMessage(message.constData(), message.size());
    }

  private:
    QString m_url;
    std::shared_ptr<std::atomic_int> m_result;
    CefRefPtr<CefRegistration> m_registration;

    IMPLEMENT_REFCOUNTING(CefIsolatedWorldProbe);
};

void CefHandlersTest::concurrentDownloadsKeepDistinctDestinations() {
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
    eden::engine::cef::CefEngineView first(&profile);
    eden::engine::cef::CefEngineView second(&profile);
    first.attach(&viewport);
    second.attach(&viewport);
    verifyConcurrentDownloads(profile, first, second);
}

void CefHandlersTest::ordinaryCookieHandoff() {
    AutofillPageServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::Cef;
    parameters.profileId = "cookie-test";
    parameters.dataPath =
        QString::fromStdString(eden::engine::cef::CefRuntime::instance().rootCachePath().string()) + "/cookie-test";
    parameters.cachePath = parameters.dataPath;
    eden::engine::cef::CefProfile profile(parameters);
    QQuickWindow window;
    window.resize(700, 400);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQuickItem viewport(window.contentItem());
    viewport.setSize(window.size());
    eden::engine::cef::CefEngineView view(&profile);
    view.attach(&viewport);
    verifyOrdinaryCookieHandoff(view, profile, server.serverPort());
}

void CefHandlersTest::mediaConsentAndIndicators() {
    AutofillPageServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
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
    QSignalSpy permissions(&view, &eden::engine::EngineView::permissionRequested);
    connect(&view, &eden::engine::EngineView::permissionRequested, &view, [&view](const auto &request) {
        view.resolvePermissionRequest(request.id, true);
    });
    view.load(QUrl(QString("http://127.0.0.1:%1/capture-guard").arg(server.serverPort())));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("legacy:NotAllowedError"), 15000);
    QCOMPARE(permissions.size(), 0);
    QVERIFY(!view.isCapturing());
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(80, 35));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("call-active"), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(view.isCapturing(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(view.activityIndicators().size(), 2, 5000);
    QCOMPARE(view.activityIndicators().at(0).toMap().value("icon").toString(), QString("camera"));
    QCOMPARE(view.activityIndicators().at(1).toMap().value("icon").toString(), QString("microphone"));
    QCOMPARE(permissions.size(), 1);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(80, 100));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("call-stopped"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isCapturing(), 5000);
    QVERIFY(view.captureDescription().isEmpty());
}

void CefHandlersTest::credentialTargets() {
    AutofillPageServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
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
    verifyAutofillTargets(view, server.serverPort(), [&view] {
        auto result = std::make_shared<std::atomic_int>(0);
        QVERIFY(CefPostTask(TID_UI, new CefIsolatedWorldProbe(view.url().toString(), result)));
        QTRY_VERIFY_WITH_TIMEOUT(result->load() != 0, 5000);
        QCOMPARE(result->load(), 1);
    });
    if (!QTest::currentTestFailed()) {
        verifyPersonalDataTargets(view, server.serverPort());
        verifyCookieIsolation(view, profile, server.serverPort());
    }
}

void CefHandlersTest::preservesFinalPaint_data() {
    QTest::addColumn<bool>("stallRendering");
    QTest::newRow("qt-delivery") << false;
    QTest::newRow("scene-graph") << true;
}

void CefHandlersTest::preservesFinalPaint() {
    QFETCH(bool, stallRendering);
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::Cef;
    parameters.privateProfile = true;
    eden::engine::cef::CefProfile profile(parameters);
    std::atomic_bool stallNextFrame = false;
    std::atomic_bool threadedRendering = false;
    QThread *qtThread = QThread::currentThread();
    QQuickWindow window;
    connect(
        &window,
        &QQuickWindow::beforeRendering,
        &window,
        [&] {
            threadedRendering.store(QThread::currentThread() != qtThread);
            if (stallNextFrame.exchange(false)) {
                QThread::msleep(800);
            }
        },
        Qt::DirectConnection
    );
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQuickItem viewport(window.contentItem());
    viewport.setSize(window.size());
    eden::engine::cef::CefEngineView view(&profile);
    view.attach(&viewport);
    view.load(QUrl(QString("http://127.0.0.1:%1/paint-burst").arg(m_server.serverPort())));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("Paint ready"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    const auto colorAt = [&window](int quarter) {
        const QImage frame = window.grabWindow();
        return frame.isNull() ? QColor() : frame.pixelColor(frame.width() * quarter / 4, frame.height() / 2);
    };
    QTRY_COMPARE_WITH_TIMEOUT(colorAt(1), QColor("#dd2222"), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(colorAt(3), QColor("#dd2222"), 5000);
    if (stallRendering && !threadedRendering.load()) {
        QSKIP("The scene graph test requires threaded rendering");
    }
    stallNextFrame.store(stallRendering);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(90, 35));
    if (!stallRendering) {
        QThread::msleep(800);
    }
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("Final paint"), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(colorAt(1), QColor("#2255dd"), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(colorAt(3), QColor("#2255dd"), 3000);
}

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

extern "C" gchar *secret_password_lookup_sync(const SecretSchema *, GCancellable *, GError **, ...) {
    return g_strdup(QByteArray(32, 'H').toBase64().constData());
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
    qputenv("XDG_DATA_HOME", (m_environmentDirectory.path() + "/data").toUtf8());
    qputenv("XDG_CACHE_HOME", (m_environmentDirectory.path() + "/cache").toUtf8());
    bool encrypted = false;
    QString encryptionError;
    eden::core::EngineStorage::instance()->prepare(
        eden::core::ProfilePaths::standardRoots(),
        [&](const QString &error) {
            encryptionError = error;
            encrypted = true;
        }
    );
    QTRY_VERIFY_WITH_TIMEOUT(encrypted, 40000);
    QVERIFY2(encryptionError.isEmpty(), qPrintable(encryptionError));
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
    QTRY_COMPARE_WITH_TIMEOUT(
        window.grabWindow().pixelColor(
            QPoint(qRound(5 * window.devicePixelRatio()), qRound(200 * window.devicePixelRatio()))
        ),
        QColor("#d8ead3"),
        10000
    );
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

    QSignalSpy captureClosed(&source, &eden::engine::EngineView::displayCaptureRequestClosed);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(110, 495));
    QTRY_COMPARE_WITH_TIMEOUT(displayCapture.size(), 3, 10000);
    const auto staleCapture = displayCapture.at(2).constFirst().value<eden::engine::DisplayCaptureRequestInfo>();
    source.reload();
    QTRY_COMPARE_WITH_TIMEOUT(captureClosed.size(), 1, 10000);
    QCOMPARE(captureClosed.constFirst().constFirst().toULongLong(), staleCapture.id);
    QTRY_COMPARE_WITH_TIMEOUT(source.title(), QString("E35 Main"), 10000);
    source.resolveDisplayCaptureRequest(staleCapture.id, "window");

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
    auto target = std::make_shared<std::optional<eden::engine::AutofillTarget>>();
    view.requestAutofillTarget([target](auto value) { *target = std::move(value); });
    QTRY_VERIFY_WITH_TIMEOUT(target->has_value(), 5000);
    view.fillForm(
        target->value(),
        {{"email", "person@example.test"}, {"addressLine1", "12 Test Street"}, {"city", "Halifax"}}
    );
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

class CefClipboardMessage final : public CefTask {
  public:
    CefClipboardMessage(QUrl url, QString token, QString text, bool begin, std::shared_ptr<std::atomic_bool> delivered)
        : m_url(std::move(url)),
          m_token(std::move(token)),
          m_text(std::move(text)),
          m_begin(begin),
          m_delivered(std::move(delivered)) {}

    void Execute() override {
        for (int id = 1; id < 1000; ++id) {
            const CefRefPtr<CefBrowser> browser = CefBrowserHost::GetBrowserByIdentifier(id);
            const CefRefPtr<CefFrame> frame = browser ? browser->GetMainFrame() : nullptr;
            if (!frame || frame->GetURL() != m_url.toString().toStdString()) {
                continue;
            }
            const auto message = CefProcessMessage::Create(m_begin ? "eden_clipboard_begin" : "eden_clipboard_copy");
            message->GetArgumentList()->SetString(0, m_token.toStdString());
            if (!m_begin) {
                message->GetArgumentList()->SetString(1, m_text.toStdString());
            }
            if (browser->GetHost()->GetClient()->OnProcessMessageReceived(browser, frame, PID_RENDERER, message)) {
                eden::engine::cef::CefUiBridge::runOnUiThread([delivered = m_delivered] { delivered->store(true); });
            }
            return;
        }
    }

  private:
    QUrl m_url;
    QString m_token;
    QString m_text;
    bool m_begin;
    std::shared_ptr<std::atomic_bool> m_delivered;

    IMPLEMENT_REFCOUNTING(CefClipboardMessage);
};

void CefHandlersTest::clipboardWrites_data() {
    QTest::addColumn<bool>("devTools");
    QTest::newRow("page") << false;
    QTest::newRow("devtools") << true;
}

void CefHandlersTest::clipboardWrites() {
    QFETCH(bool, devTools);
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::Cef;
    parameters.privateProfile = true;
    eden::engine::cef::CefProfile profile(parameters);
    QQuickWindow window;
    window.resize(700, 400);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    QVERIFY(x11);
    XSetInputFocus(x11->display(), static_cast<Window>(window.winId()), RevertToParent, CurrentTime);
    XSync(x11->display(), False);
    QTRY_VERIFY_WITH_TIMEOUT(window.isActive(), 5000);
    QQuickItem viewport(window.contentItem());
    viewport.setSize(window.size());
    QQuickItem inspected(window.contentItem());
    inspected.setSize(QSizeF(10, 10));
    eden::engine::cef::CefEngineView view(&profile);
    const QUrl fixture(QString("http://127.0.0.1:%1/clipboard-writes?%2")
                           .arg(m_server.serverPort())
                           .arg(QString::fromLatin1(QTest::currentDataTag())));
    if (devTools) {
        view.attach(&inspected);
        view.load(QUrl(QString("http://127.0.0.1:%1/").arg(m_server.serverPort())));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("E35 Main"), 15000);
        view.openDevTools();
        view.attachDevTools(&viewport);
        QTRY_COMPARE_WITH_TIMEOUT(eden::engine::cef::sharedDevToolsSocketServer()->connectedSessionCount(), 1, 15000);
        auto requested = std::make_shared<std::atomic_bool>(false);
        QVERIFY(CefPostTask(TID_UI, new CefNavigateDevTools(fixture, requested)));
        QTRY_VERIFY_WITH_TIMEOUT(requested->load(), 5000);
    } else {
        view.attach(&viewport);
        view.load(fixture);
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("Clipboard ready"), 15000);
    }
    QTRY_COMPARE_WITH_TIMEOUT(window.grabWindow().pixelColor(QPoint(30, 180)), QColor("#438521"), 10000);
    QClipboard *clipboard = QGuiApplication::clipboard();
    clipboard->setText("Initial clipboard");
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(120, 44));
    QTRY_COMPARE_WITH_TIMEOUT(clipboard->text(), QString("Blob text"), 5000);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(120, 44));
    QTRY_COMPARE_WITH_TIMEOUT(clipboard->text(), QString("Plain text"), 5000);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(140, 144));
    QTRY_COMPARE_WITH_TIMEOUT(clipboard->text(), QString("Frame Blob text"), 5000);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(120, 44));
    QTRY_COMPARE_WITH_TIMEOUT(clipboard->text(), QString(), 5000);
    const auto deliver = [&fixture](const QString &token, const QString &text, bool begin) {
        auto delivered = std::make_shared<std::atomic_bool>(false);
        QVERIFY(CefPostTask(TID_UI, new CefClipboardMessage(fixture, token, text, begin, delivered)));
        QTRY_VERIFY_WITH_TIMEOUT(delivered->load(), 5000);
    };
    deliver("first-frame:1", {}, true);
    deliver("second-frame:1", {}, true);
    deliver("second-frame:1", "Newer frame", false);
    deliver("first-frame:1", "Older extraction", false);
    QCOMPARE(clipboard->text(), QString("Newer frame"));
    deliver("pending:1", {}, true);
    clipboard->setText("External copy");
    deliver("pending:1", "Older extraction", false);
    QCOMPARE(clipboard->text(), QString("External copy"));
}

void CefHandlersTest::permissionDismissal() {
    AutofillPageServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
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
    QSignalSpy requests(&view, &eden::engine::EngineView::permissionRequested);
    view.load(QUrl(QString("http://127.0.0.1:%1/permission-dismissal").arg(server.serverPort())));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("permission-ready"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(100, 44));
    QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 1, 10000);
    const auto first = requests.constFirst().at(0).value<eden::engine::PermissionRequestInfo>();
    QVERIFY(first.permissions.contains("location"));
    view.dismissPermissionRequest(first.id);
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("prompt:1"), 5000);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(100, 44));
    QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 2, 10000);
    const auto second = requests.constLast().at(0).value<eden::engine::PermissionRequestInfo>();
    view.resolvePermissionRequest(second.id, false);
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("denied:2"), 5000);
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

void CefHandlersTest::garbageCollection_data() {
    QTest::addColumn<QString>("workload");
    for (const char *name : {"idle", "busy", "typing", "pointer", "navigation", "close", "subframe"}) {
        QTest::newRow(name) << QString::fromLatin1(name);
    }
}

void CefHandlersTest::garbageCollection() {
#if EDEN_ENABLE_AUTOMATION
    QFETCH(QString, workload);
    using eden::core::PerformanceMetrics;
    const quint64 sequence = PerformanceMetrics::sequence();
    const auto collections = [sequence] {
        int count = 0;
        for (const QJsonValue &value : PerformanceMetrics::samplesAfter(sequence)) {
            if (value.toObject().value("name") == "renderer.gc.request") {
                ++count;
            }
        }
        return count;
    };
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::Cef;
    parameters.privateProfile = true;
    eden::engine::cef::CefProfile profile(parameters);
    QQuickWindow window;
    window.resize(640, 420);
    window.show();
    window.requestActivate();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQuickItem viewport(window.contentItem());
    viewport.setSize(window.size());
    auto view = std::make_unique<eden::engine::cef::CefEngineView>(&profile);
    view->attach(&viewport);
    const QString page = QString("http://127.0.0.1:%1/gc-page?").arg(m_server.serverPort());
    if (workload == "subframe") {
        view->load(QUrl(QString("http://127.0.0.1:%1/gc-frame").arg(m_server.serverPort())));
    } else {
        view->load(QUrl(page + (workload == "busy" || workload == "navigation" ? "busy" : "idle")));
    }
    QTRY_COMPARE_WITH_TIMEOUT(view->title(), QString("GC ready"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view->isLoading(), 10000);
    if (workload == "idle") {
        QTRY_COMPARE_WITH_TIMEOUT(collections(), 1, 10000);
    } else if (workload == "close") {
        view.reset();
        QTest::qWait(4500);
        QCOMPARE(collections(), 0);
        return;
    } else if (workload == "typing" || workload == "pointer") {
        if (workload == "typing") {
            QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(120, 120));
        }
        for (int index = 0; index < 24; ++index) {
            if (workload == "typing") {
                QTest::keyClick(&window, Qt::Key_A);
            } else {
                QTest::mouseMove(&window, QPoint(200 + (index % 2) * 20, 200));
            }
            QTest::qWait(200);
        }
        QCOMPARE(collections(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(collections(), 1, 10000);
    } else {
        QTest::qWait(4800);
        QCOMPARE(collections(), 0);
        if (workload == "navigation") {
            view->load(QUrl(page + "idle"));
            QTRY_COMPARE_WITH_TIMEOUT(view->url(), QUrl(page + "idle"), 10000);
            QTest::qWait(1800);
            QCOMPARE(collections(), 0);
        } else {
            QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(80, 40));
        }
        QTRY_COMPARE_WITH_TIMEOUT(collections(), 1, 10000);
    }
    QTest::qWait(3500);
    QCOMPARE(collections(), 1);
#else
    QSKIP("Garbage collection diagnostics require automation support");
#endif
}

void CefHandlersTest::garbageCollectionMemory() {
#if EDEN_ENABLE_AUTOMATION
    using eden::core::PerformanceMetrics;
    const quint64 sequence = PerformanceMetrics::sequence();
    const auto collections = [sequence] {
        int count = 0;
        for (const QJsonValue &value : PerformanceMetrics::samplesAfter(sequence)) {
            if (value.toObject().value("name") == "renderer.gc.request") {
                ++count;
            }
        }
        return count;
    };
    const auto residentBytes = [](qint64 pid) -> qint64 {
        QFile status(QString("/proc/%1/status").arg(pid));
        if (!status.open(QIODevice::ReadOnly)) {
            return -1;
        }
        const QList<QByteArray> lines = status.readAll().split('\n');
        for (const QByteArray &line : lines) {
            if (line.startsWith("VmRSS:")) {
                bool valid = false;
                const qint64 value = line.simplified().split(' ').value(1).toLongLong(&valid);
                return valid ? value * 1024 : -1;
            }
        }
        return -1;
    };
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::Cef;
    parameters.privateProfile = true;
    eden::engine::cef::CefProfile profile(parameters);
    QQuickWindow window;
    window.resize(640, 420);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQuickItem viewport(window.contentItem());
    viewport.setSize(window.size());
    eden::engine::cef::CefEngineView view(&profile);
    view.attach(&viewport);
    view.load(QUrl(QString("http://127.0.0.1:%1/gc-page?memory").arg(m_server.serverPort())));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("GC ready"), 15000);
    QTRY_COMPARE_WITH_TIMEOUT(collections(), 1, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(view.rendererProcessId() > 0, 10000);
    const qint64 pid = view.rendererProcessId();
    qint64 baseline = -1;
    qint64 largestGrowth = 0;
    for (int iteration = 0; iteration < 13; ++iteration) {
        const int previousCollections = collections();
        view.reload();
        QTRY_COMPARE_WITH_TIMEOUT(collections(), previousCollections + 1, 15000);
        QTest::qWait(1000);
        QCOMPARE(view.rendererProcessId(), pid);
        const qint64 rss = residentBytes(pid);
        QVERIFY(rss > 0);
        if (iteration == 2) {
            baseline = rss;
        } else if (iteration > 2) {
            largestGrowth = std::max(largestGrowth, rss - baseline);
            qInfo("GC reload %d resident bytes %lld growth bytes %lld", iteration - 2, rss, rss - baseline);
        }
    }
    qInfo("GC baseline bytes %lld largest growth bytes %lld", baseline, largestGrowth);
    QVERIFY2(largestGrowth <= 20 * 1024 * 1024, "Renderer memory grew more than 20 MiB after cleanup");
#else
    QSKIP("Garbage collection diagnostics require automation support");
#endif
}

class CefPaintSequence final : public CefTask {
  public:
    CefPaintSequence(QString url, QList<QImage> frames, std::shared_ptr<std::atomic_int> result)
        : m_url(std::move(url)),
          m_frames(std::move(frames)),
          m_result(std::move(result)) {}

    void Execute() override {
        for (int id = 1; id < 1000; ++id) {
            const CefRefPtr<CefBrowser> browser = CefBrowserHost::GetBrowserByIdentifier(id);
            if (!browser || !browser->GetMainFrame() || browser->GetMainFrame()->GetURL() != m_url.toStdString()) {
                continue;
            }
            const CefRefPtr<CefBrowserHost> host = browser->GetHost();
            host->WasHidden(true);
            const CefRefPtr<CefRenderHandler> handler = host->GetClient()->GetRenderHandler();
            for (const QImage &image : m_frames) {
                handler->OnPaint(
                    browser,
                    PET_VIEW,
                    {CefRect(0, 0, 1, 1)},
                    image.constBits(),
                    image.width(),
                    image.height()
                );
            }
            m_result->store(1);
            return;
        }
        m_result->store(-1);
    }

  private:
    QString m_url;
    QList<QImage> m_frames;
    std::shared_ptr<std::atomic_int> m_result;

    IMPLEMENT_REFCOUNTING(CefPaintSequence);
};

void CefHandlersTest::preservesPaintAcrossCoalescedResizes() {
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::Cef;
    parameters.privateProfile = true;
    eden::engine::cef::CefProfile profile(parameters);
    QQuickWindow window;
    window.resize(128, 96);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQuickItem viewport(window.contentItem());
    viewport.setSize(window.size());
    eden::engine::cef::CefEngineView view(&profile);
    view.attach(&viewport);
    const QString page = QString("http://127.0.0.1:%1/gc-page?idle").arg(m_server.serverPort());
    view.load(QUrl(page));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("GC ready"), 15000);
    QTest::qWait(250);
    const auto corner = [&window] {
        const QImage image = window.grabWindow();
        return image.isNull() ? QColor() : image.pixelColor((QPointF(100, 70) * window.devicePixelRatio()).toPoint());
    };
    QImage initial(64, 48, QImage::Format_ARGB32_Premultiplied);
    initial.fill(QColor("#c02040"));
    auto result = std::make_shared<std::atomic_int>(0);
    QVERIFY(CefPostTask(TID_UI, new CefPaintSequence(page, {initial}, result)));
    QTRY_COMPARE_WITH_TIMEOUT(result->load(), 1, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(corner(), QColor("#c02040"), 10000);
    QImage intermediate(32, 24, QImage::Format_ARGB32_Premultiplied);
    intermediate.fill(QColor("#208040"));
    QImage final(64, 48, QImage::Format_ARGB32_Premultiplied);
    final.fill(QColor("#2040a0"));
    result = std::make_shared<std::atomic_int>(0);
    QVERIFY(CefPostTask(TID_UI, new CefPaintSequence(page, {intermediate, final}, result)));
    QDeadlineTimer deadline(5000);
    while (result->load() == 0 && !deadline.hasExpired()) {
        QThread::msleep(1);
    }
    QCOMPARE(result->load(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(corner(), QColor("#2040a0"), 10000);
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
    const qreal scale = window.devicePixelRatio();
    QCOMPARE(screenshot.size(), (QSizeF(window.size()) * scale).toSize());
    QVERIFY(
        screenshot.pixelColor((QPointF(430, 220) * scale).toPoint()) !=
        screenshot.pixelColor((QPointF(10, 10) * scale).toPoint())
    );
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

void CefHandlersTest::shellCallIndicators() {
    AutofillPageServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QQmlApplicationEngine qmlEngine;
    m_applicationContext->profiles()->setQmlEngine(&qmlEngine);
    qmlEngine.rootContext()->setContextProperty("startupPrivateWindow", true);
    qmlEngine.rootContext()->setContextProperty("startupEngineName", QString("cef"));
    qmlEngine.loadFromModule("Eden.Ui", "BrowserWindow");
    QTRY_COMPARE_WITH_TIMEOUT(qmlEngine.rootObjects().size(), 1, 10000);
    auto *window = qobject_cast<QQuickWindow *>(qmlEngine.rootObjects().constFirst());
    QVERIFY(window);
    window->resize(1100, 750);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto *controller = window->findChild<eden::core::WindowController *>("windowController");
    QVERIFY(controller);
    eden::test::ProfileHarness harness;
    QVERIFY(harness.create(&qmlEngine));
    controller->initialize(harness.context, true, "cef", false);
    controller->newTab(QUrl(QString("http://127.0.0.1:%1/capture-guard").arg(server.serverPort())), false, "cef");
    auto *view = qobject_cast<eden::engine::EngineView *>(controller->currentEngine());
    QVERIFY(view);
    QTRY_COMPARE_WITH_TIMEOUT(view->title(), QString("legacy:NotAllowedError"), 15000);
    QQuickItem *viewport = nullptr;
    std::function<void(QQuickItem *)> findViewport = [&](QQuickItem *item) {
        if (item->objectName() == "tabViewport" && item->isVisible()) {
            viewport = item;
        }
        for (auto *child : item->childItems()) {
            findViewport(child);
        }
    };
    findViewport(window->contentItem());
    QVERIFY(viewport);
    QTest::mouseClick(window, Qt::LeftButton, {}, viewport->mapToScene(QPointF(80, 35)).toPoint());
    QTRY_VERIFY_WITH_TIMEOUT(!controller->permissionRequest().isEmpty(), 10000);
    controller->resolvePermissionRequest(true);
    QTRY_COMPARE_WITH_TIMEOUT(view->title(), QString("call-active"), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(
        controller->tabPreview(controller->activeIndex()).value("activityIndicators").toList().size(),
        2,
        5000
    );
    QObject *preview = nullptr;
    for (auto *candidate : window->findChildren<QObject *>()) {
        if (QString::fromLatin1(candidate->metaObject()->className()).startsWith("TabPreview_QMLTYPE") &&
            candidate->property("tabIndex").toInt() == controller->activeIndex()) {
            preview = candidate;
            break;
        }
    }
    QVERIFY(preview);
    QVERIFY(QMetaObject::invokeMethod(preview, "open"));
    QTRY_VERIFY(preview->property("opened").toBool());
    QCOMPARE(preview->property("previewData").toMap().value("activityIndicators").toList().size(), 2);
    QTest::qWait(250);
    QVERIFY(window->grabWindow().save("/tmp/eden-work-tab-preview.png"));
    QVERIFY(QMetaObject::invokeMethod(preview, "close"));
    QTest::mouseClick(window, Qt::LeftButton, {}, viewport->mapToScene(QPointF(80, 100)).toPoint());
    QTRY_COMPARE_WITH_TIMEOUT(view->title(), QString("call-stopped"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!view->isCapturing(), 5000);
    QVERIFY(controller->tabPreview(controller->activeIndex()).value("activityIndicators").toList().isEmpty());
    window->close();
}

void CefHandlersTest::shellFullscreen() {
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
    controller->initialize(harness.context, true, "cef", false);
    controller->newTab(QUrl(QString("http://127.0.0.1:%1/fullscreen").arg(m_server.serverPort())), false, "cef");
    eden::engine::EngineView *view = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&] {
            view = qobject_cast<eden::engine::EngineView *>(controller->currentEngine());
            return view;
        })(),
        10000
    );
    QTRY_COMPARE_WITH_TIMEOUT(view->title(), QString("fullscreen-ready"), 10000);
    const QPoint pagePoint(window->width() / 2, window->height() / 2);
    QVERIFY(view->containsPageScenePoint(pagePoint));
    if (QCoreApplication::arguments().contains("--engine-compositing=windowed")) {
        QLibrary xtest("Xtst", 6);
        using FakeButton = int (*)(Display *, unsigned int, int, unsigned long);
        const auto fakeButton = reinterpret_cast<FakeButton>(xtest.resolve("XTestFakeButtonEvent"));
        QVERIFY(fakeButton);
        auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
        QVERIFY(x11);
        Display *display = x11->display();
        XWarpPointer(display, 0, window->winId(), 0, 0, 0, 0, pagePoint.x(), pagePoint.y());
        QVERIFY(fakeButton(display, 1, true, 0));
        QVERIFY(fakeButton(display, 1, false, 0));
        XFlush(display);
    } else {
        QTest::mouseClick(window, Qt::LeftButton, {}, pagePoint);
    }
    QTRY_VERIFY_WITH_TIMEOUT(controller->contentFullscreen(), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(view->title(), QString("fullscreen-active"), 10000);
    QCOMPARE(controller->fullscreenOrigin(), QString("http://127.0.0.1:%1").arg(m_server.serverPort()));
    auto *notice = window->findChild<QQuickItem *>("fullscreenNotice");
    QVERIFY(notice);
    QVERIFY(notice->isVisible());
    QCOMPARE(notice->height(), 48.0);
    QTRY_VERIFY_WITH_TIMEOUT(!view->containsPageScenePoint(notice->mapToScene(QPointF(20, 20))), 5000);
    QQuickItem *exitButton = nullptr;
    for (QQuickItem *candidate : window->findChildren<QQuickItem *>()) {
        if (candidate->property("text").toString() == "Exit fullscreen" && candidate->isVisible()) {
            exitButton = candidate;
            break;
        }
    }
    QVERIFY(exitButton);
    const QImage fullscreen = window->grabWindow();
    QVERIFY(!fullscreen.isNull());
    const QString screenshotPath = qEnvironmentVariable("EDEN_FULLSCREEN_SCREENSHOT");
    if (!screenshotPath.isEmpty()) {
        QVERIFY(fullscreen.save(screenshotPath));
    }
    const QPoint exitPoint =
        exitButton->mapToScene(QPointF(exitButton->width() / 2, exitButton->height() / 2)).toPoint();
    QTest::mouseClick(window, Qt::LeftButton, {}, exitPoint);
    QTRY_VERIFY_WITH_TIMEOUT(!controller->contentFullscreen(), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(view->title(), QString("fullscreen-exited"), 10000);
    QVERIFY(!notice->isVisible());
    window->close();
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
    eden::engine::EngineFactory::shutdown();
    eden::engine::cef::CefRuntime::instance().shutdown();
    eden::core::EngineStorage::instance()->shutdown();
    const auto roots = eden::core::ProfilePaths::standardRoots();
    for (const auto &path : {roots.dataRoot + "/engine-data", roots.cacheRoot + "/profiles"}) {
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
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

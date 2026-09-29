#include "engine/engineplugin.h"
#include "engine/fontpolicy.h"
#include "engine/wpe/wpeengineview.h"
#include "engine/wpe/wpeprofile.h"
#include "engine/wpe/wperuntime.h"
#include "engine/wpe/wpesurface.h"
#include "platform/waylandglibeventdispatcher.h"

#include <QBuffer>
#include <QClipboard>
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLibrary>
#include <QMimeData>
#include <QProcess>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QScreen>
#include <QSemaphore>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>

using namespace eden::engine;

static QList<pid_t> descendantProcessesNamed(const QByteArray &name) {
    QList<pid_t> result;
    const pid_t self = getpid();
    const auto entries = QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &entry : entries) {
        bool numeric = false;
        const pid_t pid = entry.toInt(&numeric);
        if (!numeric) {
            continue;
        }
        QFile comm(QString("/proc/%1/comm").arg(pid));
        if (!comm.open(QIODevice::ReadOnly) || comm.readAll().trimmed() != name) {
            continue;
        }
        pid_t ancestor = pid;
        for (int depth = 0; depth < 8 && ancestor > 1; ++depth) {
            QFile stat(QString("/proc/%1/stat").arg(ancestor));
            if (!stat.open(QIODevice::ReadOnly)) {
                break;
            }
            const QByteArray line = stat.readAll();
            ancestor = line.mid(line.lastIndexOf(')') + 2).split(' ').value(1).toInt();
            if (ancestor == self) {
                result.append(pid);
                break;
            }
        }
    }
    return result;
}

struct ProcessWarmingTestApi {
    using SetEnabled = void (*)(WebKitWebContext *, gboolean);
    using GetEnabled = gboolean (*)(WebKitWebContext *);
    using Prewarm = gboolean (*)(WebKitWebContext *, WebKitNetworkSession *);
    using Identifier = guint64 (*)(WebKitWebContext *);
    SetEnabled setEnabled =
        reinterpret_cast<SetEnabled>(dlsym(RTLD_DEFAULT, "webkit_web_context_set_automatic_process_warming_enabled"));
    GetEnabled getEnabled =
        reinterpret_cast<GetEnabled>(dlsym(RTLD_DEFAULT, "webkit_web_context_get_automatic_process_warming_enabled"));
    Prewarm prewarm = reinterpret_cast<Prewarm>(dlsym(RTLD_DEFAULT, "webkit_web_context_prewarm_web_process"));
    Identifier identifier =
        reinterpret_cast<Identifier>(dlsym(RTLD_DEFAULT, "webkit_web_context_get_prewarmed_web_process_identifier"));

    explicit operator bool() const {
        return setEnabled && getEnabled && prewarm && identifier;
    }
};

class WpeBackendTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() {
        const bool jscPolicyWasSet = qEnvironmentVariableIsSet("JSC_jitPolicyScale");
        const bool tunedDefaultsDisabled = qEnvironmentVariable("EDEN_WPE_TUNED_DEFAULTS") == "0";
        QVERIFY(WpeRuntime::initialize("Eden", "test"));
        if (qEnvironmentVariableIsSet("EDEN_WPE_REQUIRE_PHASE1")) {
            if (!jscPolicyWasSet && !tunedDefaultsDisabled) {
                QCOMPARE(qEnvironmentVariable("JSC_jitPolicyScale"), QStringLiteral("1.5"));
            }
            QVERIFY2(bool(ProcessWarmingTestApi{}), "The selected runtime lacks the Phase 1 warming API");
            QVERIFY2(
                dlsym(RTLD_DEFAULT, "webkit_web_view_preconnect_to_uri"),
                "The selected runtime lacks the Phase 1 preconnect API"
            );
        }
        QVERIFY(m_server.listen(QHostAddress::LocalHost));
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                    QByteArray pending = socket->property("pending").toByteArray() + socket->readAll();
                    if (!pending.contains("\r\n\r\n")) {
                        socket->setProperty("pending", pending);
                        return;
                    }
                    const auto path = pending.split(' ').value(1);
                    QByteArray body;
                    if (path.startsWith("/favicon.png")) {
                        QImage icon(16, 16, QImage::Format_ARGB32);
                        icon.fill(QColor(30, 200, 80));
                        QBuffer buffer(&body);
                        buffer.open(QIODevice::WriteOnly);
                        icon.save(&buffer, "PNG");
                        socket->write(
                            "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: " +
                            QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body
                        );
                        socket->disconnectFromHost();
                        return;
                    } else if (path.startsWith("/download")) {
                        body = "synthetic-download-content";
                        socket->write(
                            "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Disposition: "
                            "attachment; filename=eden-synthetic-test.txt\r\nContent-Length: " +
                            QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body
                        );
                        socket->disconnectFromHost();
                        return;
                    } else if (path.startsWith("/cookie-check")) {
                        body = pending.toLower().contains("cookie: synthetic-session=test-only")
                                   ? "<title>Authenticated</title>"
                                   : "<title>Anonymous</title>";
                    } else if (path.startsWith("/slow-thumbnail")) {
                        QTimer::singleShot(4500, socket, [socket] {
                            const QByteArray response = "<!doctype html><title>slow-thumbnail</title>";
                            socket->write(
                                "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
                                QByteArray::number(response.size()) + "\r\nConnection: close\r\n\r\n" + response
                            );
                            socket->disconnectFromHost();
                        });
                        return;
                    } else if (path.startsWith("/input")) {
                        body =
                            "<!doctype "
                            "html><title>Input</"
                            "title><style>body{margin:0}input,select{display:block;width:250px;height:45px}</"
                            "style><input id=text autocomplete=username><input id=file type=file accept='text/plain' "
                            "multiple><select id=select><option>First</option><option>Second</option></select><div "
                            "style='height:4000px'>Scrollable</div>";
                    } else if (path.startsWith("/frame-dialog")) {
                        body = "<!doctype html><script>alert('subframe')</script>";
                    } else if (path.startsWith("/notify")) {
                        body = "<!doctype html><title>Permission</title><link rel=icon href=/favicon.png><button "
                               "style='width:200px;height:80px' "
                               "onclick=\"Notification.requestPermission().then(x=>document.title=x)\">Ask "
                               "permission</button>";
                    } else if (path.startsWith("/form")) {
                        body = "<!doctype "
                               "html><title>Form</"
                               "title><style>body{margin:0}input{display:block;width:250px;height:40px}</"
                               "style><form><input id=user autocomplete=username><input id=secret type=password "
                               "autocomplete=current-password><button id=submit>Sign "
                               "in</button></"
                               "form><script>document.querySelector('form').onsubmit=e=>e.preventDefault()</script>";
                    } else if (path.startsWith("/list-renumbering")) {
                        body = "<!doctype html><title>list-renumbering</title><style>html,body{margin:0;width:100%;"
                               "height:100%;overflow:hidden;background:#f5f7fa}</style>";
                    } else if (path.startsWith("/media-autoplay")) {
                        body = R"HTML(<!doctype html><script>
const tone = () => {
    const rate = 22050;
    const samples = rate * 2;
    const buffer = new ArrayBuffer(44 + samples * 2);
    const view = new DataView(buffer);
    const text = (offset, value) => { for (let index = 0; index < value.length; index++) view.setUint8(offset + index, value.charCodeAt(index)); };
    text(0, 'RIFF');
    view.setUint32(4, 36 + samples * 2, true);
    text(8, 'WAVE');
    text(12, 'fmt ');
    view.setUint32(16, 16, true);
    view.setUint16(20, 1, true);
    view.setUint16(22, 1, true);
    view.setUint32(24, rate, true);
    view.setUint32(28, rate * 2, true);
    view.setUint16(32, 2, true);
    view.setUint16(34, 16, true);
    text(36, 'data');
    view.setUint32(40, samples * 2, true);
    for (let index = 0; index < samples; index++)
        view.setInt16(44 + index * 2, Math.sin(2 * Math.PI * 440 * index / rate) * 8000, true);
    return URL.createObjectURL(new Blob([buffer], {type: 'audio/wav'}));
};
window.toneSource = tone.toString();
window.toneURL = tone();
window.initialAutoplay = {hasBeenActive: navigator.userActivation.hasBeenActive};
for (const [name, muted] of [['audible', false], ['muted', true]]) {
    const audio = new Audio(window.toneURL);
    audio.muted = muted;
    audio.loop = true;
    audio.play().then(() => { window.initialAutoplay[name] = 'played'; audio.pause(); }, error => { window.initialAutoplay[name] = error.name; });
}
</script><title>media-autoplay</title>)HTML";
                    } else if (path.startsWith("/popup")) {
                        body = "<!doctype "
                               "html><title>Popup</title><script>window.opener.postMessage('popup-ready','*')</"
                               "script>Popup";
                    } else {
                        body = "<!doctype html><title>" + path.mid(1) +
                               "</"
                               "title><style>body{margin:0;background:#e03020;height:2200px}#box{position:absolute;"
                               "left:20px;top:20px;width:80px;height:80px;background:#20d040}</style><div "
                               "id=box></div><button style='position:absolute;top:140px' id=popup "
                               "onclick=\"window.open('/popup','auth')\">Open "
                               "popup</button><script>window.addEventListener('message',e=>document.title=e.data)</"
                               "script>";
                    }
                    socket->write(
                        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
                        QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body
                    );
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    void navigationWarmupPolicyBoundsResources() {
        using Kind = WpeConnectionWarmup::Kind;
        WpeConnectionWarmup policy;
        const QUrl https("https://example.invalid/private?q=secret#fragment");
        QCOMPARE(WpeConnectionWarmup::origin(https), QUrl("https://example.invalid/"));
        QVERIFY(policy.reserve(https, Kind::Dns, 0));
        QVERIFY(!policy.reserve(https, Kind::Dns, 59999));
        QVERIFY(policy.reserve(https, Kind::Connection, 0));
        QVERIFY(!policy.reserve(QUrl("https://EXAMPLE.invalid.:8443/"), Kind::Connection, 59999));
        QVERIFY(policy.reserve(https, Kind::Connection, 60000));
        QVERIFY(policy.reserve(https, Kind::Dns, 60000));
        for (const auto &invalid :
             {QUrl("file:///tmp/page"),
              QUrl("data:text/plain,test"),
              QUrl("https://user:password@example.invalid/"),
              QUrl("https://example.invalid:0")}) {
            QVERIFY(!policy.reserve(invalid, Kind::Dns, 120000));
            QVERIFY(!policy.reserve(invalid, Kind::Connection, 120000));
        }
        QVERIFY(!policy.reserve(QUrl("http://example.invalid/"), Kind::Connection, 120000));
        QVERIFY(!policy.reserve(https, Kind::Dns, -1));
        QVERIFY(!policy.reserve(https, Kind::Dns, std::numeric_limits<qint64>::max()));
        WpeConnectionWarmup bounded;
        for (int i = 0; i < 128; ++i) {
            QVERIFY(bounded.reserve(QUrl(QString("https://host%1.invalid/").arg(i)), Kind::Dns, 0));
        }
        QCOMPARE(bounded.hostCount(), 128);
        QVERIFY(!bounded.reserve(QUrl("https://overflow.invalid/"), Kind::Dns, 59999));
        QVERIFY(bounded.reserve(QUrl("https://overflow.invalid/"), Kind::Dns, 60000));
        QCOMPARE(bounded.hostCount(), 1);
        WpeConnectionWarmup connections;
        for (int i = 0; i < 16; ++i) {
            QVERIFY(connections.reserve(QUrl(QString("https://host%1.invalid/").arg(i)), Kind::Connection, 0));
        }
        QVERIFY(!connections.reserve(QUrl("https://overflow.invalid/"), Kind::Connection, 59999));
        QVERIFY(connections.reserve(QUrl("https://overflow.invalid/"), Kind::Connection, 60000));
    }

    void navigationWarmupUsesTheViewsSession() {
        if (!dlsym(RTLD_DEFAULT, "webkit_web_view_preconnect_to_uri")) {
            QSKIP("The accepted runtime predates the Phase 1 preconnect API");
        }
        WpeProfile first(privateParameters());
        WpeProfile second(privateParameters());
        WpeEngineView firstView(&first);
        WpeEngineView secondView(&second);
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QUrl target(QString("https://127.0.0.1:%1/private?secret=never-sent").arg(server.serverPort()));
        first.preconnectNavigation(secondView.nativeView(), target);
        QCOMPARE(first.connectionWarmupCounters()["preconnectRequests"].toULongLong(), 0);
        first.preconnectNavigation(firstView.nativeView(), target);
        second.preconnectNavigation(secondView.nativeView(), target);
        const quint64 expected = WpeProfile::connectionWarmupEnabled() ? 1 : 0;
        QCOMPARE(first.connectionWarmupCounters()["preconnectRequests"].toULongLong(), expected);
        QCOMPARE(second.connectionWarmupCounters()["preconnectRequests"].toULongLong(), expected);
        if (expected) {
            QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 5000);
            std::unique_ptr<QTcpSocket> firstConnection(server.nextPendingConnection());
            QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 5000);
            std::unique_ptr<QTcpSocket> secondConnection(server.nextPendingConnection());
            QVERIFY(firstConnection);
            QVERIFY(secondConnection);
        }
        first.preconnectNavigation(firstView.nativeView(), target);
        QCOMPARE(first.connectionWarmupCounters()["preconnectRequests"].toULongLong(), expected);
        QVERIFY(webkit_network_session_is_ephemeral(first.session()));
        QVERIFY(webkit_network_session_is_ephemeral(second.session()));
        QVERIFY(first.session() != second.session());
    }

    void navigationWarmupDebouncesAndCancels() {
        if (!dlsym(RTLD_DEFAULT, "webkit_web_view_preconnect_to_uri")) {
            QSKIP("The accepted runtime predates the Phase 1 preconnect API");
        }
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(800, 600);
        view.attach(window.contentItem());
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(wpe_view_get_visible(view.surface()->nativeView()), 5000);
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QUrl target(QString("https://127.0.0.1:%1/").arg(server.serverPort()));
        for (int i = 0; i < 100; ++i) {
            view.prepareNavigation({target}, target);
        }
        QCOMPARE(profile.connectionWarmupCounters()["preconnectRequests"].toULongLong(), 0);
        view.prepareNavigation({}, {});
        QTest::qWait(150);
        QCOMPARE(profile.connectionWarmupCounters()["preconnectRequests"].toULongLong(), 0);
        view.prepareNavigation({target}, target);
        view.attach(nullptr);
        QTest::qWait(150);
        QCOMPARE(profile.connectionWarmupCounters()["preconnectRequests"].toULongLong(), 0);
        view.attach(window.contentItem());
        QTRY_VERIFY_WITH_TIMEOUT(wpe_view_get_visible(view.surface()->nativeView()), 5000);
        view.prepareNavigation({target}, QUrl("https://not-a-suggestion.invalid/"));
        QTest::qWait(150);
        QCOMPARE(profile.connectionWarmupCounters()["preconnectRequests"].toULongLong(), 0);
        view.prepareNavigation({target}, target);
        if (WpeProfile::connectionWarmupEnabled()) {
            QTRY_COMPARE(profile.connectionWarmupCounters()["preconnectRequests"].toULongLong(), 1);
        } else {
            QTest::qWait(150);
            QCOMPARE(profile.connectionWarmupCounters()["preconnectRequests"].toULongLong(), 0);
        }
        WpeProfile hiddenProfile(privateParameters());
        WpeEngineView hiddenView(&hiddenProfile);
        QQuickWindow hiddenWindow;
        hiddenWindow.resize(800, 600);
        hiddenView.attach(hiddenWindow.contentItem());
        hiddenWindow.hide();
        hiddenView.prepareNavigation({target}, target);
        QTest::qWait(150);
        QCOMPARE(hiddenProfile.connectionWarmupCounters()["preconnectRequests"].toULongLong(), 0);
        hiddenWindow.show();
        QTRY_VERIFY_WITH_TIMEOUT(wpe_view_get_visible(hiddenView.surface()->nativeView()), 5000);
        hiddenView.prepareNavigation({target}, target);
        if (WpeProfile::connectionWarmupEnabled()) {
            QTRY_COMPARE(hiddenProfile.connectionWarmupCounters()["preconnectRequests"].toULongLong(), 1);
        }
    }

    void prewarmedProcessServesFirstPage_data() {
        QTest::addColumn<bool>("privateProfile");
        QTest::newRow("persistent") << false;
        QTest::newRow("ephemeral") << true;
    }

    void prewarmedProcessServesFirstPage() {
        const ProcessWarmingTestApi warming;
        if (!warming) {
            QSKIP("The control runtime has no process warming API");
        }
        QFETCH(bool, privateProfile);
        QTemporaryDir directory;
        WpeProfile profile(privateProfile ? privateParameters() : regularParameters(directory.path(), "prewarming"));
        if (qEnvironmentVariable("EDEN_WPE_PREWARM") == "0") {
            QVERIFY(!warming.getEnabled(profile.context()));
            QVERIFY(!warming.prewarm(profile.context(), profile.session()));
            QCOMPARE(warming.identifier(profile.context()), guint64(0));
            return;
        }
        QVERIFY(warming.getEnabled(profile.context()));
        QTRY_VERIFY_WITH_TIMEOUT(warming.identifier(profile.context()), 15000);
        const auto identifier = warming.identifier(profile.context());
        for (int request = 0; request < 8; ++request) {
            QVERIFY(warming.prewarm(profile.context(), profile.session()));
            QCOMPARE(warming.identifier(profile.context()), identifier);
        }
        WpeEngineView view(&profile);
        view.load(url("prewarmed"));
        QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading() && view.title() == "prewarmed", 15000);
        QCOMPARE(quint64(view.rendererProcessId()), identifier);
        QVERIFY(webkit_network_session_is_ephemeral(profile.session()) == privateProfile);
        QVERIFY(evaluate(view, "document.body !== null").toBool());
    }

    void prewarmingRejectsDifferentSessionAndCanBeDisabled() {
        const ProcessWarmingTestApi warming;
        if (!warming) {
            QSKIP("The control runtime has no process warming API");
        }
        auto *context = webkit_web_context_new();
        auto *first = webkit_network_session_new_ephemeral();
        auto *second = webkit_network_session_new_ephemeral();
        const auto cleanup = qScopeGuard([&] {
            warming.setEnabled(context, FALSE);
            g_object_unref(context);
            g_object_unref(first);
            g_object_unref(second);
        });
        warming.setEnabled(context, TRUE);
        QVERIFY(warming.prewarm(context, first));
        QTRY_VERIFY_WITH_TIMEOUT(warming.identifier(context), 15000);
        const auto identifier = warming.identifier(context);
        QVERIFY(!warming.prewarm(context, second));
        QCOMPARE(warming.identifier(context), identifier);
        warming.setEnabled(context, FALSE);
        QVERIFY(!warming.getEnabled(context));
        QVERIFY(!warming.prewarm(context, first));
        QTRY_COMPARE_WITH_TIMEOUT(warming.identifier(context), guint64(0), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(QString("/proc/%1").arg(identifier)), 15000);
        warming.setEnabled(context, TRUE);
        QVERIFY(!warming.prewarm(context, second));
        QVERIFY(warming.prewarm(context, first));
        QTRY_VERIFY_WITH_TIMEOUT(warming.identifier(context), 15000);
    }

    void closingProfileTerminatesItsSpareProcess() {
        const ProcessWarmingTestApi warming;
        if (!warming) {
            QSKIP("The control runtime has no process warming API");
        }
        auto profile = std::make_unique<WpeProfile>(privateParameters());
        warming.setEnabled(profile->context(), TRUE);
        QVERIFY(warming.prewarm(profile->context(), profile->session()));
        QTRY_VERIFY_WITH_TIMEOUT(warming.identifier(profile->context()), 15000);
        const auto identifier = warming.identifier(profile->context());
        profile.reset();
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(QString("/proc/%1").arg(identifier)), 15000);
    }

    void faviconsPreserveResolutionAndChooseLargestIcoFrame() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        view.load(url("icons"));
        QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading() && view.title() == "icons", 15000);
        const auto png = [](int size, QColor color) {
            QImage image(size, size, QImage::Format_ARGB32);
            image.fill(color);
            QByteArray bytes;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            return bytes;
        };
        const QByteArray small = png(16, Qt::red);
        const QByteArray large = png(64, Qt::green);
        QByteArray ico;
        QDataStream stream(&ico, QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream << quint16(0) << quint16(1) << quint16(2);
        quint32 offset = 38;
        for (const auto &entry : {qMakePair(16, small), qMakePair(64, large)}) {
            stream << quint8(entry.first) << quint8(entry.first) << quint8(0) << quint8(0) << quint16(1) << quint16(32)
                   << quint32(entry.second.size()) << offset;
            offset += entry.second.size();
        }
        ico.append(small);
        ico.append(large);
        const auto deliver = [&](const QByteArray &data) {
            GBytes *bytes = g_bytes_new(data.constData(), data.size());
            g_signal_emit_by_name(view.nativeView(), "eden-favicon-v1", bytes);
            g_bytes_unref(bytes);
        };
        const auto image = [&] {
            return QImage::fromData(QByteArray::fromBase64(view.faviconUrl().toString().section(',', 1).toLatin1()));
        };
        deliver(ico);
        QCOMPARE(image().size(), QSize(64, 64));
        QCOMPARE(image().pixelColor(32, 32), QColor(Qt::green));
        deliver(small);
        QCOMPARE(image().size(), QSize(64, 64));
        QCOMPARE(image().pixelColor(32, 32), QColor(Qt::green));
        deliver(
            "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'><rect width='16' height='16' "
            "fill='blue'/></svg>"
        );
        QCOMPARE(image().size(), QSize(128, 128));
        QCOMPARE(image().pixelColor(64, 64), QColor(Qt::blue));
        view.load(url("next-icons"));
        QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading() && view.title() == "next-icons", 15000);
        deliver(small);
        QCOMPARE(image().size(), QSize(16, 16));
        QCOMPARE(image().pixelColor(8, 8), QColor(Qt::red));
    }

    void rendererMemoryUsesTheActualProcess() {
        WpeProfile firstProfile(privateParameters());
        WpeProfile secondProfile(privateParameters());
        WpeEngineView first(&firstProfile);
        WpeEngineView second(&secondProfile);
        first.load(url("first-process"));
        second.load(url("second-process"));
        QTRY_VERIFY_WITH_TIMEOUT(first.rendererProcessId() > 0 && second.rendererProcessId() > 0, 15000);
        QVERIFY(first.rendererProcessId() != second.rendererProcessId());
        QVERIFY(first.rendererProcessId() != QCoreApplication::applicationPid());
        for (qint64 pid : {first.rendererProcessId(), second.rendererProcessId()}) {
            QFile process(QString("/proc/%1/comm").arg(pid));
            QVERIFY(process.open(QIODevice::ReadOnly));
            QVERIFY(process.readAll().startsWith("WPEWebProcess"));
        }
        const qint64 previous = first.rendererProcessId();
        webkit_web_view_terminate_web_process(first.nativeView());
        QTRY_COMPARE_WITH_TIMEOUT(first.rendererProcessId(), 0, 15000);
        first.reload();
        QTRY_VERIFY_WITH_TIMEOUT(first.rendererProcessId() > 0 && first.rendererProcessId() != previous, 15000);
        QVERIFY(second.rendererProcessId() > 0);
    }

    void systemColorSchemeUpdatesWithoutReload() {
        auto *hints = QGuiApplication::styleHints();
        const auto original = hints->colorScheme();
        const auto restore = qScopeGuard([&] { hints->setColorScheme(original); });
        hints->setColorScheme(Qt::ColorScheme::Dark);
        QQuickWindow window;
        window.resize(640, 480);
        window.show();
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        view.attach(window.contentItem());
        view.load(url("color-scheme"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("color-scheme"), 15000);
        QTRY_VERIFY(evaluate(view, "matchMedia('(prefers-color-scheme: dark)').matches").toBool());
        evaluate(
            view,
            "window.schemeChanges=0;matchMedia('(prefers-color-scheme: "
            "dark)').addEventListener('change',()=>++window.schemeChanges)"
        );
        hints->setColorScheme(Qt::ColorScheme::Light);
        QTRY_VERIFY(!evaluate(view, "matchMedia('(prefers-color-scheme: dark)').matches").toBool());
        QTRY_COMPARE(evaluate(view, "window.schemeChanges").toInt(), 1);
        hints->setColorScheme(Qt::ColorScheme::Dark);
        QTRY_VERIFY(evaluate(view, "matchMedia('(prefers-color-scheme: dark)').matches").toBool());
        QTRY_COMPARE(evaluate(view, "window.schemeChanges").toInt(), 2);
    }

    void nativeContextMenusHaveIconsAndInspect() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(1000, 700);
        QQuickItem page(window.contentItem());
        page.setSize(QSizeF(600, 700));
        QQuickItem inspector(window.contentItem());
        inspector.setPosition(QPointF(600, 0));
        inspector.setSize(QSizeF(400, 700));
        view.attach(&page);
        view.attachDevTools(&inspector);
        window.show();
        view.load(url("input"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("Input"), 15000);
        QSignalSpy menus(&view, &EngineView::contextMenuRequested);
        QTest::mouseClick(&window, Qt::RightButton, {}, QPoint(50, 20));
        QTRY_VERIFY_WITH_TIMEOUT(!menus.isEmpty(), 15000);
        const auto info = qvariant_cast<ContextMenuInfo>(menus.takeLast().first());
        QVERIFY(info.editable);
        QCOMPARE(info.surface, QString("page"));
        bool hasPaste = false;
        bool hasInspect = false;
        for (const QVariant &entry : info.actions) {
            const auto action = entry.toMap();
            if (action.value("separator").toBool()) {
                continue;
            }
            const QString icon = action.value("icon").toString();
            QVERIFY(!icon.isEmpty());
            QVERIFY(icon != "menu-dots");
            hasPaste |= icon == "clipboard";
            hasInspect |= action.value("id") == "inspect";
        }
        QVERIFY(hasPaste);
        QVERIFY(hasInspect);
        view.executeContextMenuCommand("inspect");
        QTRY_VERIFY_WITH_TIMEOUT(view.devToolsOpen(), 15000);
        QVERIFY(!inspector.childItems().isEmpty());
        auto *surface = qobject_cast<WpeSurface *>(inspector.childItems().first());
        QVERIFY(surface);
        QTRY_VERIFY_WITH_TIMEOUT(surface->textureImportCount() > 0, 15000);
        menus.clear();
        const auto contentReady = [&] {
            const QImage frame = window.grabWindow();
            if (frame.isNull()) {
                return false;
            }
            QSet<QRgb> colors;
            for (int y = 60; y < 650; y += 5) {
                for (int x = 620; x < 980; x += 5) {
                    colors.insert(
                        frame.pixel(qRound(x * window.devicePixelRatio()), qRound(y * window.devicePixelRatio()))
                    );
                }
            }
            return colors.size() > 40;
        };
        QTRY_VERIFY_WITH_TIMEOUT(contentReady(), 15000);
        const auto selectedElement = [&](const QString &id) {
            QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(795, 14));
            QTest::qWait(100);
            QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(710, 685));
            QTRY_VERIFY(surface->hasActiveFocus());
            QTest::qWait(100);
            QGuiApplication::clipboard()->setText("document.title='Selected '+$0.id");
            QTest::keyClick(&window, Qt::Key_V, Qt::ControlModifier);
            QTest::qWait(200);
            QTest::keyClick(&window, Qt::Key_Return);
            QTRY_COMPARE_WITH_TIMEOUT(view.title(), "Selected " + id, 15000);
        };
        selectedElement("text");
        QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(700, 14));
        const QString initialArtifacts = qEnvironmentVariable("EDEN_WPE_TEST_ARTIFACT_DIR");
        if (!initialArtifacts.isEmpty()) {
            QVERIFY(QDir().mkpath(initialArtifacts));
            QVERIFY(window.grabWindow().save(QDir(initialArtifacts).filePath("inspector-before-menu.png")));
        }
        QTest::mouseClick(&window, Qt::RightButton, {}, QPoint(720, 150));
        QTRY_VERIFY_WITH_TIMEOUT(!menus.isEmpty(), 15000);
        QCOMPARE(qvariant_cast<ContextMenuInfo>(menus.last().first()).surface, QString("devtools"));
        QVERIFY(!qvariant_cast<ContextMenuInfo>(menus.last().first()).actions.isEmpty());
        view.dismissContextMenu();
        menus.clear();
        QTest::mouseClick(&window, Qt::RightButton, {}, QPoint(50, 65));
        QTRY_VERIFY_WITH_TIMEOUT(!menus.isEmpty(), 15000);
        view.executeContextMenuCommand("inspect");
        QVERIFY(view.devToolsOpen());
        selectedElement("file");
        QCOMPARE(inspector.childItems().size(), 1);
        const QString artifacts = qEnvironmentVariable("EDEN_WPE_TEST_ARTIFACT_DIR");
        if (!artifacts.isEmpty()) {
            QVERIFY(QDir().mkpath(artifacts));
            QVERIFY(window.grabWindow().save(QDir(artifacts).filePath("inspect-selection.png")));
        }
        view.closeDevTools();
        QVERIFY(!view.devToolsOpen());
    }

    void pageSourceUsesNativeInspector() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(1000, 700);
        QQuickItem page(window.contentItem());
        page.setSize(QSizeF(600, 700));
        QQuickItem inspector(window.contentItem());
        inspector.setPosition(QPointF(600, 0));
        inspector.setSize(QSizeF(400, 700));
        view.attach(&page);
        view.attachDevTools(&inspector);
        window.show();
        view.load(url("input"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("Input"), 15000);
        QSignalSpy menus(&view, &EngineView::contextMenuRequested);
        QTest::mouseClick(&window, Qt::RightButton, {}, QPoint(400, 300));
        QTRY_VERIFY_WITH_TIMEOUT(!menus.isEmpty(), 10000);
        const auto info = qvariant_cast<ContextMenuInfo>(menus.last().first());
        QVERIFY(std::any_of(info.actions.cbegin(), info.actions.cend(), [](const QVariant &value) {
            return value.toMap().value("id") == "view_source";
        }));
        view.executeContextMenuCommand("view_source");
        QTRY_VERIFY_WITH_TIMEOUT(view.devToolsOpen(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!inspector.childItems().isEmpty(), 10000);
        auto *surface = qobject_cast<WpeSurface *>(inspector.childItems().first());
        QVERIFY(surface);
        QTRY_VERIFY_WITH_TIMEOUT(surface->textureImportCount() > 0, 10000);
        QTest::qWait(500);
        const QString artifacts = qEnvironmentVariable("EDEN_WPE_TEST_ARTIFACT_DIR");
        if (!artifacts.isEmpty()) {
            QVERIFY(QDir().mkpath(artifacts));
            QVERIFY(window.grabWindow().save(QDir(artifacts).filePath("page-source.png")));
        }
        QGuiApplication::clipboard()->setText("awaiting source");
        QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(920, 180));
        QTest::qWait(100);
        QTest::keyClick(&window, Qt::Key_A, Qt::ControlModifier);
        QTest::qWait(100);
        QTest::keyClick(&window, Qt::Key_C, Qt::ControlModifier);
        QTRY_VERIFY2_WITH_TIMEOUT(
            QGuiApplication::clipboard()->text().contains("<title>Input</title>"),
            qPrintable(QGuiApplication::clipboard()->text().left(500)),
            10000
        );
        QVERIFY(QGuiApplication::clipboard()->text().contains("autocomplete=username"));
    }

    void nativeSpellCheckingOffersCorrections() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(640, 480);
        view.attach(window.contentItem());
        window.show();
        view.load(url("spelling"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("spelling"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(webkit_web_context_get_spell_checking_enabled(profile.context()), 10000);
        evaluate(
            view,
            "document.body.innerHTML='<textarea id=editor spellcheck=true "
            "style=width:400px;height:100px;font-size:24px></textarea>'"
        );
        QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(30, 25));
        QTRY_COMPARE_WITH_TIMEOUT(evaluate(view, "document.activeElement.id").toString(), QString("editor"), 10000);
        for (int key : {Qt::Key_H, Qt::Key_L, Qt::Key_E, Qt::Key_L, Qt::Key_O, Qt::Key_Space}) {
            QTest::keyClick(&window, Qt::Key(key));
        }
        QTRY_COMPARE_WITH_TIMEOUT(evaluate(view, "editor.value").toString(), QString("hlelo "), 10000);
        QTest::qWait(300);
        QSignalSpy menus(&view, &EngineView::contextMenuRequested);
        QTest::mouseClick(&window, Qt::RightButton, {}, QPoint(30, 25));
        QTRY_VERIFY_WITH_TIMEOUT(!menus.isEmpty(), 10000);
        const auto info = qvariant_cast<ContextMenuInfo>(menus.last().first());
        QString correction;
        for (const auto &value : info.actions) {
            const auto action = value.toMap();
            if (action.value("title").toString() == "hello") {
                correction = action.value("id").toString();
            }
        }
        QVERIFY(!correction.isEmpty());
        view.executeContextMenuCommand(correction);
        QTRY_COMPARE_WITH_TIMEOUT(evaluate(view, "editor.value").toString(), QString("hello "), 10000);
    }

    void richTextPasteKeepsTheCaret() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(700, 500);
        view.attach(window.contentItem());
        window.show();
        view.load(url("editor"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("editor"), 15000);
        evaluate(
            view,
            "document.body.innerHTML='<div id=editor contenteditable style=width:600px;height:200px>Before after</div>'"
        );
        QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(30, 25));
        for (int offset : {12, 7}) {
            evaluate(
                view,
                QString(
                    "(()=>{const e=document.querySelector('#editor');e.textContent='Before "
                    "after';e.focus();getSelection().setPosition(e.firstChild,%1)})()"
                )
                    .arg(offset)
            );
            QGuiApplication::clipboard()->setText("https://example.test/link");
            evaluate(
                view,
                "document.addEventListener('paste',e=>{window.pasteTrusted=e.isTrusted;window.pasteText=e."
                "clipboardData.getData('text/plain')},{once:true})"
            );
            QTest::keyClick(&window, Qt::Key_V, Qt::ControlModifier);
            QTest::keyClick(&window, Qt::Key_Home);
            const QString expected = QString("Before after").insert(offset, "https://example.test/link");
            QTRY_COMPARE_WITH_TIMEOUT(
                evaluate(view, "document.querySelector('#editor').textContent").toString(),
                expected,
                10000
            );
            QVERIFY(evaluate(view, "window.pasteTrusted").toBool());
            QCOMPARE(evaluate(view, "window.pasteText").toString(), QString("https://example.test/link"));
        }
    }

    void browserShortcutsLeaveFocusedEditors() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(700, 500);
        view.attach(window.contentItem());
        window.show();
        view.load(url("input"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("Input"), 15000);
        QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(30, 25));
        QSignalSpy shortcuts(&view, &EngineView::shortcutRequested);
        QTest::keyClick(&window, Qt::Key_R, Qt::ControlModifier);
        QTest::keyClick(&window, Qt::Key_R, Qt::ControlModifier | Qt::ShiftModifier);
        QTest::keyClick(&window, Qt::Key_N, Qt::ControlModifier);
        QTest::keyClick(&window, Qt::Key_I, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(shortcuts.size(), 4);
        QCOMPARE(shortcuts.at(0).first().toString(), QString("reload"));
        QCOMPARE(shortcuts.at(1).first().toString(), QString("reload_bypass_cache"));
        QCOMPARE(shortcuts.at(2).first().toString(), QString("new_window"));
        QCOMPARE(shortcuts.at(3).first().toString(), QString("devtools"));
        QCOMPARE(evaluate(view, "document.getElementById('text').value").toString(), QString());
    }

    void pasteHandlersReceiveRichClipboard() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(700, 500);
        view.attach(window.contentItem());
        window.show();
        view.load(url("editor"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("editor"), 15000);
        evaluate(
            view,
            "document.body.innerHTML='<div id=editor contenteditable "
            "style=width:600px;height:200px></"
            "div>';editor.addEventListener('paste',e=>{e.preventDefault();editor.textContent=e.clipboardData.getData('"
            "text/plain')+'|'+e.clipboardData.getData('text/html')+'|'+e.isTrusted})"
        );
        QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(30, 25));
        auto *mime = new QMimeData;
        mime->setText("https://example.test/task");
        mime->setHtml("<b>Task</b>");
        QGuiApplication::clipboard()->setMimeData(mime);
        QTest::keyClick(&window, Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE_WITH_TIMEOUT(
            evaluate(
                view,
                "(()=>{const parts=editor.textContent.split('|');const html=new "
                "DOMParser().parseFromString(parts[1]||'', 'text/html');return "
                "parts[0]+'|'+html.body.textContent+'|'+parts[2]})()"
            )
                .toString(),
            QString("https://example.test/task|Task|true"),
            10000
        );
    }

    void focusedEditorsAlwaysUseAVisibleCaretColor() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        view.load(url("caret-color"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("caret-color"), 15000);
        QVERIFY(evaluate(view, R"JS(
document.body.innerHTML='<div id="editor" contenteditable style="color:rgb(12,34,56);caret-color:transparent!important"><p><span id="nested" style="caret-color:transparent!important">Editable text</span></p></div>';
const editor=document.getElementById('editor');
editor.focus();
getSelection().setPosition(document.getElementById('nested').firstChild,4);
true
)JS")
                    .toBool());
        QTRY_COMPARE(
            evaluate(view, "getComputedStyle(document.getElementById('editor')).caretColor").toString(),
            QString("rgb(12, 34, 56)")
        );
        QTRY_COMPARE(
            evaluate(view, "getComputedStyle(document.getElementById('nested')).caretColor").toString(),
            QString("rgb(12, 34, 56)")
        );
        evaluate(
            view,
            "window.caretMutations=0;new "
            "MutationObserver(records=>window.caretMutations+=records.length).observe(editor,{attributes:true,subtree:"
            "true});getSelection().setPosition(document.getElementById('nested').firstChild,5)"
        );
        QTest::qWait(100);
        QCOMPARE(evaluate(view, "window.caretMutations").toInt(), 0);
    }

    void displayCaptureUsesTemporaryConsent() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(700, 500);
        view.attach(window.contentItem());
        window.show();
        webkit_settings_set_enable_mock_capture_devices(webkit_web_view_get_settings(view.nativeView()), TRUE);
        view.load(url("display-capture"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("display-capture"), 15000);
        evaluate(
            view,
            "document.body.innerHTML='<button id=share "
            "style=position:absolute;left:10px;top:10px;width:200px;height:60px>Share</button>';"
            "share.onclick=()=>navigator.mediaDevices.getDisplayMedia({video:true}).then(s=>{window.shared=s;"
            "const video=document.createElement('video');video.muted=true;video.autoplay=true;video.srcObject=s;"
            "video.style.cssText='position:absolute;left:10px;top:100px;width:160px;height:90px';"
            "document.body.append(video);"
            "document.title='sharing:'+s.getVideoTracks()[0].getSettings().displaySurface}).catch(e=>document.title=e."
            "name)"
        );
        QSignalSpy requests(&view, &EngineView::displayCaptureRequested);
        QSignalSpy permissions(&view, &EngineView::permissionRequested);
        QSignalSpy closed(&view, &EngineView::displayCaptureRequestClosed);
        QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(60, 30));
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 1, 10000);
        auto request = qvariant_cast<DisplayCaptureRequestInfo>(requests.takeFirst().first());
        QCOMPARE(request.origin, autofillOrigin(url("display-capture")));
        QVERIFY(permissions.isEmpty());
        view.resolveDisplayCaptureRequest(request.id, {});
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("NotAllowedError"), 10000);
        QCOMPARE(closed.size(), 1);
        for (const QString &choice : {QString("window"), QString("screen")}) {
            QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(60, 30));
            QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 1, 10000);
            request = qvariant_cast<DisplayCaptureRequestInfo>(requests.takeFirst().first());
            view.resolveDisplayCaptureRequest(request.id, choice);
            QTRY_COMPARE_WITH_TIMEOUT(
                view.title(),
                choice == "window" ? QString("sharing:window") : QString("sharing:monitor"),
                10000
            );
            QTRY_VERIFY_WITH_TIMEOUT(view.isCapturing(), 10000);
            evaluate(view, "shared.getTracks().forEach(t=>t.stop())");
            QTRY_VERIFY_WITH_TIMEOUT(!view.isCapturing(), 10000);
        }
        QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(60, 30));
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 1, 10000);
        request = qvariant_cast<DisplayCaptureRequestInfo>(requests.takeFirst().first());
        view.load(url("capture-cancelled"));
        QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 4, 10000);
        view.resolveDisplayCaptureRequest(request.id, "screen");
        QVERIFY(!view.isCapturing());
    }

    void displayCaptureRequestsTheDesktopPortal() {
        const QString source = qEnvironmentVariable("EDEN_TEST_DESKTOP_PORTAL");
        if (source != "window" && source != "screen") {
            QSKIP("Requires a private desktop portal session");
        }
        qint64 firstRenderer = 0;
        {
            WpeProfile firstProfile(privateParameters());
            WpeEngineView firstView(&firstProfile);
            QQuickWindow firstWindow;
            firstWindow.resize(300, 200);
            firstView.attach(firstWindow.contentItem());
            firstWindow.show();
            firstView.load(url("display-portal-first"));
            QTRY_COMPARE_WITH_TIMEOUT(firstView.title(), QString("display-portal-first"), 15000);
            firstRenderer = firstView.rendererProcessId();
        }
        QVERIFY(firstRenderer > 0);
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(QString("/proc/%1").arg(firstRenderer)), 15000);
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(700, 500);
        view.attach(window.contentItem());
        window.show();
        view.load(url("display-portal"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("display-portal"), 15000);
        evaluate(
            view,
            "document.body.innerHTML='<button id=share "
            "style=position:absolute;left:10px;top:10px;width:200px;height:60px>Share</button>';"
            "share.onclick=()=>navigator.mediaDevices.getDisplayMedia({video:true}).then(s=>{"
            "document.title='sharing:'+s.getVideoTracks()[0].getSettings().displaySurface}).catch(e=>document.title=e."
            "name)"
        );
        QSignalSpy requests(&view, &EngineView::displayCaptureRequested);
        QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(60, 30));
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 1, 10000);
        const auto request = qvariant_cast<DisplayCaptureRequestInfo>(requests.takeFirst().first());
        view.resolveDisplayCaptureRequest(request.id, source);
        QTest::qWait(qEnvironmentVariableIntValue("EDEN_TEST_DESKTOP_PORTAL_WAIT_MS"));
        qInfo("Desktop portal display capture outcome: %s", qPrintable(view.title()));
    }

    void displayCaptureShowsTheSharedSurface() {
        const QString source = qEnvironmentVariable("EDEN_TEST_DISPLAY_FRAMES");
        const QString control = qEnvironmentVariable("EDEN_TEST_DISPLAY_FRAMES_CONTROL");
        if ((source != "window" && source != "screen") || control.isEmpty()) {
            QSKIP("Requires a private desktop session with a shared probe window");
        }
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(700, 500);
        view.attach(window.contentItem());
        window.show();
        webkit_settings_set_media_playback_requires_user_gesture(webkit_web_view_get_settings(view.nativeView()), FALSE);
        const bool maximizedFirst = qEnvironmentVariableIntValue("EDEN_TEST_DISPLAY_FRAMES_MAXIMIZE_FIRST") > 0;
        if (maximizedFirst) {
            QFile marker(QDir(control).filePath("maximize"));
            QVERIFY(marker.open(QIODevice::WriteOnly));
            marker.close();
            QTest::qWait(1500);
        }
        view.load(url("display-frames"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("display-frames"), 15000);
        QJsonObject options;
        options.insert("codec", qEnvironmentVariable("EDEN_TEST_DISPLAY_FRAMES_CODEC"));
        options.insert("hint", qEnvironmentVariable("EDEN_TEST_DISPLAY_FRAMES_HINT"));
        options.insert("maximizedFirst", maximizedFirst);
        evaluate(view, "window.displayFramesOptions = " + QString::fromUtf8(QJsonDocument(options).toJson(QJsonDocument::Compact)) + "; true");
        evaluate(view, R"JS(
window.displayFrames = null;
window.displayFramesStage = 'waiting for the share button';
const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
const classify = pixel => {
    const [r, g, b] = pixel;
    if (r > 180 && g > 180 && b < 90) return 'Y';
    if (r > 180 && g < 90 && b < 90) return 'R';
    if (g > 180 && r < 90 && b < 90) return 'G';
    if (b > 180 && r < 90 && g < 90) return 'B';
    return '.';
};
const presented = (video, count) => new Promise(resolve => {
    let frames = 0;
    const next = () => { if (++frames >= count) resolve(); else video.requestVideoFrameCallback(next); };
    video.requestVideoFrameCallback(next);
});
const trackSampler = `
const classify = (r, g, b) => r > 180 && g > 180 && b < 90 ? 'Y' : r > 180 && g < 90 && b < 90 ? 'R' : g > 180 && r < 90 && b < 90 ? 'G' : b > 180 && r < 90 && g < 90 ? 'B' : '.';
self.onmessage = async ({data}) => {
    const reader = new MediaStreamTrackProcessor({track: data.track}).readable.getReader();
    const deadline = performance.now() + 20000;
    let result = {error: 'no frame arrived'};
    while (performance.now() < deadline) {
        const {value: frame, done} = await reader.read();
        if (done)
            break;
        if (frame.displayWidth === data.skipWidth) {
            frame.close();
            continue;
        }
        const width = frame.displayWidth;
        const height = frame.displayHeight;
        try {
            const pixels = new Uint8Array(frame.allocationSize());
            const layout = await frame.copyTo(pixels);
            const clamp = value => Math.max(0, Math.min(255, Math.round(value)));
            const rgbAt = (x, y) => {
                switch (frame.format) {
                case 'BGRA':
                case 'BGRX': {
                    const offset = layout[0].offset + y * layout[0].stride + x * 4;
                    return [pixels[offset + 2], pixels[offset + 1], pixels[offset]];
                }
                case 'RGBA':
                case 'RGBX': {
                    const offset = layout[0].offset + y * layout[0].stride + x * 4;
                    return [pixels[offset], pixels[offset + 1], pixels[offset + 2]];
                }
                case 'I420':
                case 'NV12': {
                    const luma = pixels[layout[0].offset + y * layout[0].stride + x] - 16;
                    let u;
                    let v;
                    if (frame.format === 'I420') {
                        u = pixels[layout[1].offset + (y >> 1) * layout[1].stride + (x >> 1)] - 128;
                        v = pixels[layout[2].offset + (y >> 1) * layout[2].stride + (x >> 1)] - 128;
                    } else {
                        const chroma = layout[1].offset + (y >> 1) * layout[1].stride + (x >> 1) * 2;
                        u = pixels[chroma] - 128;
                        v = pixels[chroma + 1] - 128;
                    }
                    return [clamp(1.164 * luma + 1.596 * v), clamp(1.164 * luma - 0.392 * u - 0.813 * v), clamp(1.164 * luma + 2.017 * u)];
                }
                }
                return [0, 0, 0];
            };
            const grid = [];
            for (let row = 0; row < 4; row++) {
                let line = '';
                for (let column = 0; column < 4; column++)
                    line += classify(...rgbAt(Math.floor((column + 0.5) * width / 4), Math.floor((row + 0.5) * height / 4)));
                grid.push(line);
            }
            result = {width, height, grid, format: frame.format, center: rgbAt(width >> 1, height >> 1)};
        } catch (error) {
            result = {width, height, error: String(error)};
        }
        frame.close();
        break;
    }
    reader.cancel();
    data.track.stop();
    self.postMessage(result);
};`;
const sampleTrack = (track, skipWidth) => new Promise(resolve => {
    const worker = new Worker(URL.createObjectURL(new Blob([trackSampler], {type: 'text/javascript'})));
    const clone = track.clone();
    worker.onmessage = event => { worker.terminate(); resolve(event.data); };
    worker.onerror = event => { worker.terminate(); resolve({error: event.message}); };
    worker.postMessage({track: clone, skipWidth}, [clone]);
});
const sample = async (video, track, skipWidth) => {
    const frame = await sampleTrack(track, skipWidth);
    const canvas = document.createElement('canvas');
    canvas.width = video.videoWidth;
    canvas.height = video.videoHeight;
    const context = canvas.getContext('2d', {willReadFrequently: true});
    context.drawImage(video, 0, 0);
    const grid = [];
    for (let row = 0; row < 4; row++) {
        let line = '';
        for (let column = 0; column < 4; column++)
            line += classify(context.getImageData(Math.floor((column + 0.5) * canvas.width / 4), Math.floor((row + 0.5) * canvas.height / 4), 1, 1).data);
        grid.push(line);
    }
    const settings = track.getSettings();
    return {videoWidth: frame.width, videoHeight: frame.height, grid: frame.grid, frameError: frame.error, frameFormat: frame.format, centerPixel: frame.center, elementWidth: video.videoWidth, elementHeight: video.videoHeight, canvasGrid: grid,
        settings: {width: settings.width, height: settings.height, displaySurface: settings.displaySurface}};
};
const makeVideo = stream => {
    const video = document.createElement('video');
    video.muted = true;
    video.playsInline = true;
    video.srcObject = stream;
    video.style.cssText = 'width:160px;height:100px';
    document.body.append(video);
    video.play().catch(() => {});
    return video;
};
const button = document.createElement('button');
button.id = 'share';
button.style.cssText = 'position:absolute;left:10px;top:10px;width:200px;height:60px';
button.textContent = 'Share';
document.body.append(button);
button.onclick = () => navigator.mediaDevices.getDisplayMedia({video: true}).then(async stream => {
    const options = window.displayFramesOptions || {};
    const track = stream.getVideoTracks()[0];
    if (options.hint)
        track.contentHint = options.hint;
    window.displayFramesStage = 'waiting for local frames';
    const video = makeVideo(stream);
    const decodedFrames = async connection => { let frames = 0; (await connection.getStats()).forEach(item => { if (item.type === 'inbound-rtp' && item.kind === 'video') frames = item.framesDecoded || 0; }); return frames; };
    const waitFor = async (condition, what) => {
        const deadline = performance.now() + 30000;
        while (!(await condition())) {
            if (performance.now() > deadline)
                throw new Error(what);
            await delay(50);
        }
    };
    if (options.maximizedFirst)
        await waitFor(() => video.readyState >= 2 && video.videoWidth > 0, 'the shared window never reached the local video');
    else
        await presented(video, 10);
    await delay(500);
    const local = await sample(video, track);
    window.displayFramesPartial = {local};
    window.displayFramesStage = 'connecting a loopback call';
    const sender = new RTCPeerConnection();
    const receiver = new RTCPeerConnection();
    sender.onicecandidate = event => event.candidate && receiver.addIceCandidate(event.candidate);
    receiver.onicecandidate = event => event.candidate && sender.addIceCandidate(event.candidate);
    const received = new Promise(resolve => receiver.ontrack = event => resolve(event.track));
    sender.addTrack(track, stream);
    if (options.codec) {
        const capabilities = RTCRtpReceiver.getCapabilities('video').codecs;
        const wanted = capabilities.filter(codec => codec.mimeType.toLowerCase() === 'video/' + options.codec.toLowerCase());
        if (!wanted.length)
            throw new Error(options.codec + ' is not offered');
        sender.getTransceivers()[0].setCodecPreferences([...wanted, ...capabilities.filter(codec => !wanted.includes(codec))]);
    }
    await sender.setLocalDescription(await sender.createOffer());
    await receiver.setRemoteDescription(sender.localDescription);
    await receiver.setLocalDescription(await receiver.createAnswer());
    await sender.setRemoteDescription(receiver.localDescription);
    const remoteTrack = await received;
    const remoteVideo = makeVideo(new MediaStream([remoteTrack]));
    window.displayFramesStage = 'waiting for remote frames';
    if (options.maximizedFirst)
        await waitFor(async () => remoteVideo.videoWidth > 0 && await decodedFrames(receiver) >= 30, 'the call never decoded 30 frames');
    else
        await presented(remoteVideo, 30);
    await delay(500);
    const remote = await sample(remoteVideo, remoteTrack);
    window.displayFramesStage = 'measuring the shared frame rate';
    const encodedBefore = await (async () => { let frames = 0; (await sender.getStats()).forEach(item => { if (item.type === 'outbound-rtp' && item.kind === 'video') frames = item.framesEncoded || 0; }); return frames; })();
    const decodedBefore = await decodedFrames(receiver);
    let presentedFrames = 0;
    const countPresented = () => { presentedFrames++; remoteVideo.requestVideoFrameCallback(countPresented); };
    remoteVideo.requestVideoFrameCallback(countPresented);
    let capturedFrames = 0;
    const countCaptured = () => { capturedFrames++; video.requestVideoFrameCallback(countCaptured); };
    video.requestVideoFrameCallback(countCaptured);
    await delay(4000);
    let encodedAfter = 0;
    let encodeTime = 0;
    let qualityLimitation = '';
    let codecId = '';
    const report = await sender.getStats();
    report.forEach(item => {
        if (item.type === 'outbound-rtp' && item.kind === 'video') {
            encodedAfter = item.framesEncoded || 0;
            encodeTime = item.totalEncodeTime || 0;
            qualityLimitation = item.qualityLimitationReason || '';
            codecId = item.codecId || '';
            remote.encoder = item.encoderImplementation || '';
            remote.limitationDurations = item.qualityLimitationDurations || null;
            remote.sentSize = [item.frameWidth || 0, item.frameHeight || 0];
            remote.sentFps = item.framesPerSecond || 0;
        }
    });
    remote.codec = codecId && report.get(codecId) ? report.get(codecId).mimeType : '';
    remote.decodedFps = (await decodedFrames(receiver) - decodedBefore) / 4;
    remote.presentedFps = presentedFrames / 4;
    remote.capturedFps = capturedFrames / 4;
    remote.encodedFps = (encodedAfter - encodedBefore) / 4;
    remote.encodeMsPerFrame = encodedAfter ? Math.round(1000 * encodeTime / encodedAfter * 10) / 10 : 0;
    remote.qualityLimitation = qualityLimitation;
    let resized = null;
    if (track.getSettings().displaySurface === 'window' && !options.maximizedFirst) {
        window.displayFramesStage = 'waiting for the window to be maximized';
        window.displayFramesMaximize = true;
        const deadline = performance.now() + 15000;
        while (performance.now() < deadline && track.getSettings().width === local.settings.width)
            await delay(100);
        await delay(500);
        resized = await sample(video, track, local.videoWidth);
    }
    sender.close();
    receiver.close();
    stream.getTracks().forEach(item => item.stop());
    window.displayFrames = {local, remote, resized};
}).catch(error => window.displayFrames = {error: String(error), stage: window.displayFramesStage});
document.title = 'display-frames';
)JS");
        QSignalSpy requests(&view, &EngineView::displayCaptureRequested);
        QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(60, 30));
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 1, 10000);
        const auto request = qvariant_cast<DisplayCaptureRequestInfo>(requests.takeFirst().first());
        view.resolveDisplayCaptureRequest(request.id, source);
        QElapsedTimer waited;
        waited.start();
        bool maximizeRequested = false;
        while (!evaluate(view, "!!window.displayFrames").toBool() && waited.elapsed() < 90000) {
            if (!maximizeRequested && evaluate(view, "!!window.displayFramesMaximize").toBool()) {
                QFile marker(QDir(control).filePath("maximize"));
                QVERIFY(marker.open(QIODevice::WriteOnly));
                marker.close();
                maximizeRequested = true;
            }
            QTest::qWait(100);
        }
        const QString frames = evaluate(view, "JSON.stringify(window.displayFrames || {error: 'timed out', stage: window.displayFramesStage, partial: window.displayFramesPartial})").toString();
        qInfo("Display capture frames: %s", qPrintable(frames));
        const QJsonObject result = QJsonDocument::fromJson(frames.toUtf8()).object();
        QVERIFY2(!result.contains("error"), qPrintable(frames));
    }

    void microphoneLevelFollowsGainControl() {
        const QString page = qEnvironmentVariable("EDEN_TEST_MICROPHONE_LEVEL_PAGE");
        if (page.isEmpty()) {
            QSKIP("Requires a private audio session with a quiet talker");
        }
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(700, 500);
        view.attach(window.contentItem());
        window.show();
        webkit_settings_set_media_playback_requires_user_gesture(webkit_web_view_get_settings(view.nativeView()), FALSE);
        QObject::connect(
            &view,
            &EngineView::permissionRequested,
            &view,
            [&view](const PermissionRequestInfo &info) { view.resolvePermissionRequest(info.id, true); }
        );
        view.load(QUrl(page));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("microphone-level"), 15000);
        QElapsedTimer waited;
        waited.start();
        while (!evaluate(view, "!!window.microphoneLevel").toBool() && waited.elapsed() < 90000) {
            QTest::qWait(200);
        }
        const QJsonObject result = QJsonDocument::fromJson(evaluate(view, "JSON.stringify(window.microphoneLevel || {error: 'no result'})").toString().toUtf8()).object();
        qInfo().noquote() << "Microphone level" << QJsonDocument(result).toJson(QJsonDocument::Compact);
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
    }

    void voiceProcessingCancelsEcho() {
        const QString microphone = qEnvironmentVariable("EDEN_TEST_VOICE_PROCESSING");
        const QString control = qEnvironmentVariable("EDEN_TEST_VOICE_PROCESSING_CONTROL");
        if (microphone.isEmpty() || control.isEmpty()) {
            QSKIP("Requires a private audio session with an echo path");
        }
        QJsonObject results;
        for (const bool echoCancellation : {true, false}) {
            WpeProfile profile(privateParameters());
            WpeEngineView view(&profile);
            QQuickWindow window;
            window.resize(700, 500);
            view.attach(window.contentItem());
            window.show();
            webkit_settings_set_media_playback_requires_user_gesture(
                webkit_web_view_get_settings(view.nativeView()),
                FALSE
            );
            QObject::connect(
                &view,
                &EngineView::permissionRequested,
                &view,
                [&view](const PermissionRequestInfo &info) { view.resolvePermissionRequest(info.id, true); }
            );
            view.load(url("voice-processing"));
            QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("voice-processing"), 15000);
            evaluate(
                view,
                QString(R"JS(
window.voiceResult = null;
(async () => {
    const control = '%3';
    const context = new AudioContext();
    await context.resume();
    const remote = new RTCPeerConnection();
    const local = new RTCPeerConnection();
    remote.onicecandidate = event => event.candidate && local.addIceCandidate(event.candidate);
    local.onicecandidate = event => event.candidate && remote.addIceCandidate(event.candidate);
    const speech = await context.decodeAudioData(await (await fetch(control + '/far-end.wav')).arrayBuffer());
    const filter = (type, frequency) => {
        const node = context.createBiquadFilter();
        node.type = type;
        node.frequency.value = frequency;
        return node;
    };
    const level = context.createGain();
    level.gain.value = 0.5;
    const farEnd = context.createMediaStreamDestination();
    const shaping = filter('lowpass', 3400);
    shaping.connect(filter('lowpass', 3400)).connect(level).connect(farEnd);
    let talker = null;
    const startFarEnd = () => {
        talker = context.createBufferSource();
        talker.buffer = speech;
        talker.loop = true;
        talker.connect(shaping);
        talker.start();
    };
    const stopFarEnd = () => {
        talker.stop();
        talker.disconnect();
        talker = null;
    };
    remote.addTrack(farEnd.stream.getAudioTracks()[0], farEnd.stream);
    (await navigator.mediaDevices.getUserMedia({audio: true})).getTracks().forEach(track => track.stop());
    const device = (await navigator.mediaDevices.enumerateDevices()).find(item => item.kind === 'audioinput' && item.label.includes('%1'));
    if (!device) {
        throw new Error('microphone missing');
    }
    const microphone = await navigator.mediaDevices.getUserMedia({audio: {deviceId: {exact: device.deviceId}, echoCancellation: %2}});
    const room = await navigator.mediaDevices.getUserMedia({audio: {deviceId: {exact: device.deviceId}, echoCancellation: false, noiseSuppression: false, autoGainControl: false}});
    local.addTrack(microphone.getAudioTracks()[0], microphone);
    const speaker = new Audio();
    local.ontrack = event => {
        speaker.srcObject = event.streams[0];
        speaker.play();
    };
    let received = null;
    remote.ontrack = event => {
        received = event.track;
    };
    await remote.setLocalDescription(await remote.createOffer());
    await local.setRemoteDescription(remote.localDescription);
    await local.setLocalDescription(await local.createAnswer());
    await remote.setRemoteDescription(local.localDescription);
    const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
    for (let attempt = 0; attempt < 200 && (!received || local.connectionState !== 'connected'); attempt++) {
        await delay(50);
    }
    const analyserFor = track => {
        const analyser = context.createAnalyser();
        analyser.fftSize = 8192;
        context.createMediaStreamSource(new MediaStream([track])).connect(analyser);
        return analyser;
    };
    const analysers = [analyserFor(received), analyserFor(room.getAudioTracks()[0])];
    const bins = new Float32Array(analysers[0].frequencyBinCount);
    const measure = async ranges => {
        const totals = analysers.map(() => ranges.map(() => 0));
        const samples = 30;
        for (let sample = 0; sample < samples; sample++) {
            analysers.forEach((analyser, which) => {
                analyser.getFloatFrequencyData(bins);
                ranges.forEach(([low, high], range) => {
                    const first = Math.round(low * analyser.fftSize / context.sampleRate);
                    const last = Math.round(high * analyser.fftSize / context.sampleRate);
                    for (let index = first; index <= last; index++) {
                        totals[which][range] += Math.pow(10, bins[index] / 10) / samples;
                    }
                });
            });
            await delay(100);
        }
        return totals;
    };
    const echoBand = [200, 3400];
    await delay(1000);
    startFarEnd();
    await delay(6000);
    const [[echo], [roomEcho]] = await measure([echoBand]);
    stopFarEnd();
    await delay(1500);
    const nearEndFrequency = Number(await (await fetch(control + '/near-end/on')).text());
    const nearEndBand = [nearEndFrequency - 12, nearEndFrequency + 12];
    await delay(2000);
    const [[nearEnd], [roomNearEnd]] = await measure([nearEndBand]);
    startFarEnd();
    await delay(3000);
    const [[doubleTalkEcho, doubleTalkNearEnd], [roomDoubleTalkEcho, roomDoubleTalkNearEnd]] = await measure([echoBand, nearEndBand]);
    stopFarEnd();
    await fetch(control + '/near-end/off');
    room.getTracks().forEach(track => track.stop());
    window.voiceResult = {
        echo,
        nearEnd,
        roomEcho,
        roomNearEnd,
        doubleTalkEcho,
        doubleTalkNearEnd,
        roomDoubleTalkEcho,
        roomDoubleTalkNearEnd,
        echoCancellation: microphone.getAudioTracks()[0].getSettings().echoCancellation,
        playing: !speaker.paused,
        connected: local.connectionState
    };
})().catch(error => window.voiceResult = {error: String(error)});
)JS")
                    .arg(microphone, echoCancellation ? "true" : "false", control)
            );
            QTRY_VERIFY_WITH_TIMEOUT(evaluate(view, "window.voiceResult !== null").toBool(), 90000);
            const QString raw = evaluate(view, "JSON.stringify(window.voiceResult)").toString();
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(raw.toUtf8(), &parseError);
            QVERIFY2(
                parseError.error == QJsonParseError::NoError && document.isObject(),
                qPrintable(QString("Voice processing returned no result object: %1").arg(raw))
            );
            const QJsonObject result = document.object();
            QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
            results.insert(echoCancellation ? "enabled" : "disabled", result);
        }
        qInfo().noquote() << "Voice processing results:" << QJsonDocument(results).toJson(QJsonDocument::Compact);
    }

    void cameraSelfViewWhileSendingSimulcast() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(900, 600);
        view.attach(window.contentItem());
        window.show();
        auto *settings = webkit_web_view_get_settings(view.nativeView());
        webkit_settings_set_enable_mock_capture_devices(settings, TRUE);
        webkit_settings_set_media_playback_requires_user_gesture(settings, FALSE);
        QObject::connect(
            &view,
            &EngineView::permissionRequested,
            &view,
            [&view](const PermissionRequestInfo &info) { view.resolvePermissionRequest(info.id, true); }
        );
        bool terminated = false;
        g_signal_connect(
            view.nativeView(),
            "web-process-terminated",
            G_CALLBACK(+[](WebKitWebView *, WebKitWebProcessTerminationReason, gpointer flag) {
                *static_cast<bool *>(flag) = true;
            }),
            &terminated
        );
        view.load(url("camera-call"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("camera-call"), 15000);
        evaluate(view, cameraCallScript());
        QTRY_VERIFY_WITH_TIMEOUT(!terminated && evaluate(view, "!!(window.cameraCall && (window.cameraCall.ready || window.cameraCall.error))").toBool(), 20000);
        QCOMPARE(evaluate(view, "String(window.cameraCall.error || '')").toString(), QString());
        const auto sample = [&]() -> QJsonObject {
            evaluate(view, "window.cameraSample = null; window.cameraCall.sample().then(value => window.cameraSample = value)");
            QElapsedTimer waited;
            waited.start();
            while (!terminated && !evaluate(view, "!!window.cameraSample").toBool() && waited.elapsed() < 5000) {
                QTest::qWait(50);
            }
            return QJsonDocument::fromJson(evaluate(view, "JSON.stringify(window.cameraSample || {})").toString().toUtf8())
                .object();
        };
        QTest::qWait(4000);
        QVERIFY(!terminated);
        QJsonObject previous = sample();
        QVERIFY(!previous.isEmpty());
        QCOMPARE(previous.value("connection").toString(), QString("connected"));
        const QStringList steps = {
            "",
            "window.cameraCall.stream.getVideoTracks()[0].enabled = false",
            "window.cameraCall.stream.getVideoTracks()[0].enabled = true",
            "window.cameraCall.video.sender.replaceTrack(null); window.cameraCall.direct.sender.replaceTrack(null)",
            "window.cameraCall.video.sender.replaceTrack(window.cameraCall.stream.getVideoTracks()[0]); "
            "window.cameraCall.direct.sender.replaceTrack(window.cameraCall.stream.getVideoTracks()[0])",
            "",
        };
        for (const QString &step : steps) {
            if (!step.isEmpty()) {
                evaluate(view, step);
            }
            QTest::qWait(3000);
            QVERIFY2(!terminated, qPrintable(QString("The web process terminated after: %1").arg(step)));
            const QJsonObject current = sample();
            qInfo().noquote() << "Camera call after" << (step.isEmpty() ? QString("idle") : step) << QJsonDocument(current).toJson(QJsonDocument::Compact);
            QVERIFY2(!current.isEmpty(), qPrintable(QString("No call statistics after: %1").arg(step)));
            QVERIFY2(current.value("selfTime").toDouble() > previous.value("selfTime").toDouble() + 1, qPrintable(QString("The self view stalled after: %1").arg(step)));
            QVERIFY2(current.value("audioBytes").toDouble() > previous.value("audioBytes").toDouble(), qPrintable(QString("Audio stopped after: %1").arg(step)));
            if (!step.contains("replaceTrack(null)")) {
                QVERIFY2(current.value("remoteFrames").toInt() > previous.value("remoteFrames").toInt(), qPrintable(QString("Remote video stalled after: %1").arg(step)));
            }
            previous = current;
        }
        QCOMPARE(previous.value("layers").toInt(), 3);
        evaluate(view, "window.cameraCall.stream.getTracks().forEach(track => track.stop())");
        QTest::qWait(1000);
        QVERIFY(!terminated);
    }

    void audioContextsResumeAfterConcurrentContextsClose() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(400, 300);
        view.attach(window.contentItem());
        window.show();
        webkit_settings_set_media_playback_requires_user_gesture(webkit_web_view_get_settings(view.nativeView()), FALSE);
        view.load(url("audio-context-lifecycle"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("audio-context-lifecycle"), 15000);
        evaluate(view, R"JS(window.contextLifecycle = null;
window.contextLifecycleStage = 'starting';
(async () => {
    const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
    const renders = async context => {
        const before = context.currentTime;
        await delay(300);
        return context.currentTime > before;
    };
    const resumes = async context => {
        const outcome = await Promise.race([context.resume().then(() => 'resumed'), delay(5000).then(() => 'timed out')]);
        return {outcome, state: context.state, renders: outcome === 'resumed' ? await renders(context) : false};
    };
    const steps = {};
    window.contextLifecycleSteps = steps;
    const origin = performance.now();
    const step = async (name, work) => {
        window.contextLifecycleStage = name;
        const activation = navigator.userActivation ? {isActive: navigator.userActivation.isActive, hasBeenActive: navigator.userActivation.hasBeenActive} : null;
        const at = Math.round(performance.now() - origin);
        const value = await work();
        steps[name] = Array.isArray(value) ? value : {...value, activation, at};
    };
    await step('idle', async () => { await delay(1500); return {}; });
    const first = new AudioContext();
    await step('first', () => resumes(first));
    await first.close();
    const single = new AudioContext();
    await step('single', () => resumes(single));
    const concurrent = new AudioContext();
    await step('concurrent', () => resumes(concurrent));
    await single.close();
    await concurrent.close();
    const afterPair = new AudioContext();
    await step('afterPair', () => resumes(afterPair));
    await afterPair.close();
    const trio = [new AudioContext(), new AudioContext(), new AudioContext()];
    await step('trio', async () => Promise.all(trio.map(resumes)));
    for (const context of trio.slice().reverse())
        await context.close();
    const afterTrio = new AudioContext();
    await step('afterTrio', () => resumes(afterTrio));
    const overlapping = new AudioContext();
    await step('overlapping', () => resumes(overlapping));
    await afterTrio.close();
    const whileOneRuns = new AudioContext();
    await step('whileOneRuns', () => resumes(whileOneRuns));
    await overlapping.close();
    await whileOneRuns.close();
    const autostarted = new AudioContext();
    await step('autostarts', async () => {
        await delay(500);
        return {state: autostarted.state, renders: await renders(autostarted)};
    });
    await autostarted.close();
    const suspendedPair = [new AudioContext(), new AudioContext()];
    await step('suspendedPair', async () => {
        await Promise.all(suspendedPair.map(resumes));
        await Promise.all(suspendedPair.map(context => context.suspend()));
        return suspendedPair.map(context => context.state);
    });
    await step('resumedPair', async () => Promise.all(suspendedPair.map(resumes)));
    await Promise.all(suspendedPair.map(context => context.close()));
    const frameScript = `
(async () => {
    const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
    const context = new AudioContext();
    const outcome = await Promise.race([context.resume().then(() => 'resumed'), delay(1500).then(() => 'timed out')]);
    const state = context.state;
    let renders = false;
    if (outcome === 'resumed') {
        const before = context.currentTime;
        await delay(300);
        renders = context.currentTime > before;
    }
    const suspended = await Promise.race([context.suspend().then(() => 'suspended'), delay(1500).then(() => 'timed out')]);
    const afterSuspend = context.state;
    await context.close();
    parent.postMessage({frameAudio: {outcome, state, renders, suspended, afterSuspend, hasBeenActive: navigator.userActivation ? navigator.userActivation.hasBeenActive : null}}, '*');
})().catch(error => parent.postMessage({frameAudio: {error: String(error && error.message || error)}}, '*'));
`;
    const frameAudio = attributes => new Promise(resolve => {
        const iframe = document.createElement('iframe');
        for (const [name, value] of Object.entries(attributes))
            iframe.setAttribute(name, value);
        const listener = event => {
            if (event.source !== iframe.contentWindow || !event.data || !event.data.frameAudio)
                return;
            window.removeEventListener('message', listener);
            iframe.remove();
            resolve(event.data.frameAudio);
        };
        window.addEventListener('message', listener);
        setTimeout(() => resolve({timeout: true}), 15000);
        iframe.srcdoc = '<script>' + frameScript + '</' + 'script>';
        document.body.appendChild(iframe);
    });
    await step('sameOriginFrame', () => frameAudio({}));
    await step('crossOriginFrame', () => frameAudio({sandbox: 'allow-scripts'}));
    await step('delegatedFrame', () => frameAudio({sandbox: 'allow-scripts', allow: 'autoplay *'}));
    const last = new AudioContext();
    await step('last', () => resumes(last));
    await last.close();
    window.contextLifecycle = steps;
})().catch(error => window.contextLifecycle = {error: String(error && error.message || error), stage: window.contextLifecycleStage});
)JS");
        QElapsedTimer waited;
        waited.start();
        while (!evaluate(view, "!!window.contextLifecycle").toBool() && waited.elapsed() < 180000) {
            QTest::qWait(100);
        }
        const QJsonObject result = QJsonDocument::fromJson(evaluate(view, "JSON.stringify(window.contextLifecycle || {stage: window.contextLifecycleStage, steps: window.contextLifecycleSteps})").toString().toUtf8()).object();
        qInfo().noquote() << "AudioContext lifecycle" << QJsonDocument(result).toJson(QJsonDocument::Compact);
        QVERIFY2(!result.contains("error") && !result.contains("stage"), qPrintable(QString("The lifecycle fixture stopped at %1: %2").arg(result.value("stage").toString(), result.value("error").toString())));
        const QJsonObject lateActivation = result.value("single").toObject().value("activation").toObject();
        QVERIFY2(!lateActivation.value("isActive").toBool() && lateActivation.value("hasBeenActive").toBool(), "The fixture must run after transient activation expired, with sticky activation left");
        const auto expectRunning = [&](const QString &name, const QJsonObject &step) {
            QVERIFY2(step.value("outcome").toString() == "resumed" && step.value("state").toString() == "running" && step.value("renders").toBool(),
                qPrintable(QString("%1: resume() gave %2, state %3, rendering %4").arg(name, step.value("outcome").toString(), step.value("state").toString()).arg(step.value("renders").toBool())));
        };
        for (const char *name : {"first", "single", "concurrent", "afterPair", "afterTrio", "overlapping", "whileOneRuns", "last"}) {
            expectRunning(name, result.value(name).toObject());
        }
        for (const char *name : {"trio", "resumedPair"}) {
            const QJsonArray contexts = result.value(name).toArray();
            QVERIFY2(!contexts.isEmpty(), name);
            for (qsizetype index = 0; index < contexts.size(); ++index) {
                expectRunning(QString("%1 %2").arg(name).arg(index), contexts.at(index).toObject());
            }
        }
        const QJsonObject autostarts = result.value("autostarts").toObject();
        QCOMPARE(autostarts.value("state").toString(), QString("running"));
        QVERIFY(autostarts.value("renders").toBool());
        QCOMPARE(result.value("suspendedPair").toArray(), QJsonArray({"suspended", "suspended"}));
        const auto expectFrame = [&](const QString &name, bool allowed) {
            const QJsonObject frame = result.value(name).toObject();
            const QString description = QString::fromUtf8(QJsonDocument(frame).toJson(QJsonDocument::Compact));
            QVERIFY2(!frame.contains("timeout") && !frame.contains("error"), qPrintable(name + ": " + description));
            QCOMPARE(frame.value("outcome").toString(), QString(allowed ? "resumed" : "timed out"));
            QCOMPARE(frame.value("state").toString(), QString(allowed ? "running" : "suspended"));
            QCOMPARE(frame.value("renders").toBool(), allowed);
            QCOMPARE(frame.value("suspended").toString(), QString("suspended"));
            QCOMPARE(frame.value("afterSuspend").toString(), QString("suspended"));
        };
        expectFrame("sameOriginFrame", true);
        expectFrame("crossOriginFrame", false);
        expectFrame("delegatedFrame", true);
    }

    void sentAudioSurvivesResampling() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(700, 500);
        view.attach(window.contentItem());
        window.show();
        webkit_settings_set_media_playback_requires_user_gesture(webkit_web_view_get_settings(view.nativeView()), FALSE);
        bool terminated = false;
        g_signal_connect(
            view.nativeView(),
            "web-process-terminated",
            G_CALLBACK(+[](WebKitWebView *, WebKitWebProcessTerminationReason, gpointer flag) {
                *static_cast<bool *>(flag) = true;
            }),
            &terminated
        );
        view.load(url("resampled-audio"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("resampled-audio"), 15000);
        evaluate(view, R"JS(
window.resampledAudio = null;
window.resampledAudioStage = 'starting';
(async () => {
    const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
    const frequency = 997;
    const toneToNoise = (samples, sampleRate, toneFrequency) => {
        const mean = samples.reduce((sum, value) => sum + value, 0) / samples.length;
        const step = 2 * Math.PI * toneFrequency / sampleRate;
        let inPhase = 0;
        let quadrature = 0;
        let total = 0;
        samples.forEach((value, index) => {
            const centered = value - mean;
            total += centered * centered;
            inPhase += centered * Math.cos(step * index);
            quadrature += centered * Math.sin(step * index);
        });
        const tone = 2 * (inPhase * inPhase + quadrature * quadrature) / samples.length;
        const amplitude = 2 * Math.sqrt(inPhase * inPhase + quadrature * quadrature) / samples.length;
        const phase = Math.atan2(-quadrature, inPhase);
        let silentRun = 0;
        let longestSilentRun = 0;
        let strayed = 0;
        samples.forEach((value, index) => {
            silentRun = Math.abs(value) < 1e-4 ? silentRun + 1 : 0;
            longestSilentRun = Math.max(longestSilentRun, silentRun);
            if (Math.abs(value - mean - amplitude * Math.cos(step * index + phase)) > 0.05)
                strayed++;
        });
        return {snr: 10 * Math.log10(tone / Math.max(total - tone, 1e-12)), level: Math.sqrt(total / samples.length), longestSilentRun, strayed};
    };
    const results = {};
    const contexts = [new AudioContext({sampleRate: 44100}), new AudioContext({sampleRate: 48000})];
    window.resampledAudioStage = 'starting the audio contexts';
    await Promise.all(contexts.map(context => context.resume()));
    const connections = [];
    for (const source of contexts) {
        const rate = source.sampleRate;
        window.resampledAudioStage = rate + ': starting the tone';
        const oscillator = source.createOscillator();
        oscillator.frequency.value = frequency;
        const level = source.createGain();
        level.gain.value = 0.25;
        const destination = source.createMediaStreamDestination();
        oscillator.connect(level).connect(destination);
        oscillator.start();
        const sender = new RTCPeerConnection();
        const receiver = new RTCPeerConnection();
        connections.push(sender, receiver);
        sender.onicecandidate = event => event.candidate && receiver.addIceCandidate(event.candidate);
        receiver.onicecandidate = event => event.candidate && sender.addIceCandidate(event.candidate);
        const received = new Promise(resolve => receiver.ontrack = event => resolve(event.track));
        sender.addTrack(destination.stream.getAudioTracks()[0], destination.stream);
        window.resampledAudioStage = rate + ': negotiating';
        await sender.setLocalDescription(await sender.createOffer());
        await receiver.setRemoteDescription(sender.localDescription);
        await receiver.setLocalDescription(await receiver.createAnswer());
        await sender.setRemoteDescription(receiver.localDescription);
        window.resampledAudioStage = rate + ': waiting for the remote track';
        const track = await received;
        window.resampledAudioStage = rate + ': listening';
        const analyser = source.createAnalyser();
        analyser.fftSize = 32768;
        analyser.smoothingTimeConstant = 0;
        source.createMediaStreamSource(new MediaStream([track])).connect(analyser);
        await delay(4000);
        const measurements = [];
        for (let round = 0; round < 5; round++) {
            const samples = new Float32Array(analyser.fftSize);
            analyser.getFloatTimeDomainData(samples);
            const spectrum = new Float32Array(analyser.frequencyBinCount);
            analyser.getFloatFrequencyData(spectrum);
            let peak = 1;
            for (let bin = 2; bin < spectrum.length; bin++) {
                if (spectrum[bin] > spectrum[peak])
                    peak = bin;
            }
            const peakFrequency = peak * source.sampleRate / analyser.fftSize;
            measurements.push({...toneToNoise(samples, source.sampleRate, frequency), peakFrequency});
            await delay(800);
        }
        results[rate] = {
            contextRate: source.sampleRate,
            connection: sender.connectionState,
            worstSnr: Math.min(...measurements.map(item => item.snr)),
            snrs: measurements.map(item => Math.round(item.snr * 10) / 10),
            silentRuns: measurements.map(item => item.longestSilentRun),
            strayedSamples: measurements.map(item => item.strayed),
            peakFrequencies: measurements.map(item => Math.round(item.peakFrequency)),
            quietestLevel: Math.min(...measurements.map(item => item.level))
        };
        oscillator.stop();
        sender.getSenders().forEach(item => item.track && item.track.stop());
    }
    window.resampledAudioStage = 'closing';
    connections.forEach(connection => connection.close());
    await Promise.all(contexts.map(context => context.close()));
    window.resampledAudio = results;
})().catch(error => window.resampledAudio = {error: String(error)});
)JS");
        QElapsedTimer waited;
        waited.start();
        while (!terminated && !evaluate(view, "!!window.resampledAudio").toBool() && waited.elapsed() < 60000) {
            QTest::qWait(100);
        }
        QVERIFY2(!terminated, "The web process terminated while sending resampled audio");
        QVERIFY2(
            evaluate(view, "!!window.resampledAudio").toBool(),
            qPrintable(QString("The audio fixture stalled at: %1").arg(evaluate(view, "String(window.resampledAudioStage)").toString()))
        );
        const QJsonObject result = QJsonDocument::fromJson(evaluate(view, "JSON.stringify(window.resampledAudio)").toString().toUtf8()).object();
        qInfo().noquote() << "Resampled audio" << QJsonDocument(result).toJson(QJsonDocument::Compact);
        QCOMPARE(result.value("error").toString(), QString());
        const QJsonObject resampled = result.value("44100").toObject();
        const QJsonObject native = result.value("48000").toObject();
        QCOMPARE(resampled.value("contextRate").toInt(), 44100);
        QCOMPARE(native.value("contextRate").toInt(), 48000);
        for (const QJsonObject &run : {resampled, native}) {
            QCOMPARE(run.value("connection").toString(), QString("connected"));
            QVERIFY2(run.value("quietestLevel").toDouble() > 0.1, qPrintable(QString("The tone arrived at level %1").arg(run.value("quietestLevel").toDouble())));
            for (const QJsonValue &peak : run.value("peakFrequencies").toArray()) {
                QVERIFY2(std::abs(peak.toDouble() - 997) <= 3, qPrintable(QString("The received audio peaked at %1 Hz instead of 997 Hz").arg(peak.toDouble())));
            }
        }
        const auto windowSnrs = [](const QJsonObject &run) {
            QList<double> snrs;
            for (const QJsonValue &value : run.value("snrs").toArray()) {
                snrs.append(value.toDouble());
            }
            std::sort(snrs.begin(), snrs.end());
            return snrs;
        };
        for (const auto &[label, run] : {std::pair{QString("48 kHz"), native}, std::pair{QString("44.1 kHz"), resampled}}) {
            const QList<double> snrs = windowSnrs(run);
            QVERIFY2(snrs.size() == 5, qPrintable(QString("The %1 run measured %2 windows").arg(label).arg(snrs.size())));
            QVERIFY2(snrs[2] >= 25, qPrintable(QString("A %1 tone arrived at a median of %2 dB SNR").arg(label).arg(snrs[2])));
            QVERIFY2(snrs[1] >= 20, qPrintable(QString("A %1 tone fell below 20 dB SNR in more than one window: %2").arg(label, QString::fromUtf8(QJsonDocument(run.value("snrs").toArray()).toJson(QJsonDocument::Compact)))));
        }
    }

    void cameraFramesLookTheSameOnEveryPath() {
        for (const QString &constraints : {QString("{width: 640, height: 480, frameRate: 30}"), QString("{width: 1280, height: 720, frameRate: 30}")}) {
            runFramePaths(constraints, true);
            if (QTest::currentTestFailed()) {
                return;
            }
        }
    }

    void realCameraFramesLookTheSameOnEveryPath() {
        if (qEnvironmentVariable("EDEN_TEST_REAL_CAPTURE") != "1") {
            QSKIP("Requires a real camera");
        }
        for (const QString &constraints : {QString("{width: {ideal: 640}, height: {ideal: 480}, frameRate: {ideal: 30}}"), QString("{width: {ideal: 1280}, height: {ideal: 720}, frameRate: {ideal: 30}}")}) {
            runFramePaths(constraints, false);
            if (QTest::currentTestFailed()) {
                return;
            }
        }
    }

    void webProcessExitsWhileTheGpuProcessIsUnresponsive() {
        const QByteArray previousMode = qgetenv("EDEN_WPE_GPU_PROCESS");
        const auto restoreMode = qScopeGuard([&] {
            if (previousMode.isNull()) {
                qunsetenv("EDEN_WPE_GPU_PROCESS");
            } else {
                qputenv("EDEN_WPE_GPU_PROCESS", previousMode);
            }
        });
        qputenv("EDEN_WPE_GPU_PROCESS", "1");
        const auto running = [](pid_t pid) {
            QFile stat(QString("/proc/%1/stat").arg(pid));
            if (!stat.open(QIODevice::ReadOnly)) {
                return false;
            }
            const QByteArray line = stat.readAll();
            return line.mid(line.lastIndexOf(')') + 2, 1) != "Z";
        };
        const auto webBefore = descendantProcessesNamed("WPEWebProcess");
        QList<pid_t> webProcesses;
        QList<pid_t> gpuProcesses;
        const auto resumeGpu = qScopeGuard([&] {
            for (const pid_t pid : std::as_const(gpuProcesses)) {
                kill(pid, SIGCONT);
            }
        });
        {
            auto profile = std::make_unique<WpeProfile>(privateParameters());
            auto view = std::make_unique<WpeEngineView>(profile.get());
            QQuickWindow window;
            window.resize(400, 300);
            view->attach(window.contentItem());
            window.show();
            view->load(url("webgl-teardown"));
            QTRY_COMPARE_WITH_TIMEOUT(view->title(), QString("webgl-teardown"), 15000);
            QCOMPARE(evaluate(*view, "window.gl = document.createElement('canvas').getContext('webgl2'); gl.clearColor(1, 0, 0, 1); gl.clear(gl.COLOR_BUFFER_BIT); gl.getError() === gl.NO_ERROR && !gl.isContextLost()").toBool(), true);
            QTRY_VERIFY_WITH_TIMEOUT(
                (gpuProcesses = descendantProcessesNamed("WPEGPUProcess"), gpuProcesses.removeIf([&](pid_t pid) { return !running(pid); }), !gpuProcesses.isEmpty()),
                10000
            );
            webProcesses = descendantProcessesNamed("WPEWebProcess");
            webProcesses.removeIf([&](pid_t pid) { return webBefore.contains(pid); });
            QVERIFY(!webProcesses.isEmpty());
            for (const pid_t pid : std::as_const(gpuProcesses)) {
                QCOMPARE(kill(pid, SIGSTOP), 0);
            }
            view.reset();
            profile.reset();
        }
        QElapsedTimer exiting;
        exiting.start();
        const auto alive = [&] {
            return std::count_if(webProcesses.cbegin(), webProcesses.cend(), running);
        };
        while (alive() && exiting.elapsed() < 5000) {
            QTest::qWait(50);
        }
        QVERIFY2(!alive(), qPrintable(QString("%1 web process(es) were still running %2 ms after their profile closed while the GPU process was unresponsive").arg(alive()).arg(exiting.elapsed())));
    }

    void hangRecorderReportsMainThreadStalls() {
        QTemporaryFile capture;
        QVERIFY(capture.open());
        fflush(stderr);
        const int savedError = dup(STDERR_FILENO);
        QVERIFY(savedError >= 0);
        QVERIFY(dup2(capture.handle(), STDERR_FILENO) >= 0);
        bool restored = false;
        const auto restoreError = [&] {
            if (restored) {
                return;
            }
            restored = true;
            fflush(stderr);
            dup2(savedError, STDERR_FILENO);
            close(savedError);
        };
        const auto restoreErrorOnExit = qScopeGuard(restoreError);
        qputenv("EDEN_WPE_HANG_LOG", "200");
        const auto restoreEnvironment = qScopeGuard([] { qunsetenv("EDEN_WPE_HANG_LOG"); });
        {
            WpeProfile profile(privateParameters());
            WpeEngineView view(&profile);
            QQuickWindow window;
            window.resize(400, 300);
            view.attach(window.contentItem());
            window.show();
            view.load(url("hang"));
            QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("hang"), 15000);
            evaluate(view, "setTimeout(() => { const start = performance.now(); while (performance.now() - start < 1000) {} document.title = 'stalled'; }, 0); true");
            QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("stalled"), 15000);
            QTest::qWait(500);
        }
        restoreError();
        QFile file(capture.fileName());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString log = QString::fromUtf8(file.readAll());
        const auto excerpt = log.left(4000);
        QVERIFY2(log.contains("Eden hang recorder: WPEWebProcess reports main thread stalls over 200 ms"), qPrintable(excerpt));
        const QRegularExpression stall("Eden hang recorder: WPEWebProcess main thread has not run for \\d+ ms, thread stacks follow");
        const auto stallMatch = stall.match(log);
        QVERIFY2(stallMatch.hasMatch(), qPrintable(excerpt));
        const auto afterStall = log.mid(stallMatch.capturedEnd());
        QVERIFY2(QRegularExpression("thread \\d+ \\(WPEWebProcess\\)").match(afterStall).hasMatch(), qPrintable(afterStall.left(4000)));
        QVERIFY2(QRegularExpression("\\n    libWPEWebKit-2\\.0\\.so\\.1\\+0x[0-9a-f]+").match(afterStall).hasMatch(), qPrintable(afterStall.left(4000)));
        const auto resumed = QRegularExpression("Eden hang recorder: WPEWebProcess main thread resumed after (\\d+) ms").match(afterStall);
        QVERIFY2(resumed.hasMatch(), qPrintable(excerpt));
        const int stalledFor = resumed.captured(1).toInt();
        QVERIFY2(stalledFor >= 700 && stalledFor <= 3000, qPrintable(QString("The recorder measured a %1 ms stall for a 1000 ms busy loop").arg(stalledFor)));
    }

    void animatedCanvasesKeepTheDisplayRate() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(1400, 900);
        view.attach(window.contentItem());
        window.show();
        view.load(url("webgl-source"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("webgl-source"), 15000);
        evaluate(view, R"JS(window.animationRates = null;
(async () => {
    const measure = (paint, milliseconds) => new Promise(resolve => {
        const intervals = [];
        let last = 0;
        let begin = 0;
        const tick = now => {
            paint();
            if (last)
                intervals.push(now - last);
            else
                begin = now;
            last = now;
            if (now - begin >= milliseconds) {
                intervals.sort((a, b) => a - b);
                resolve({fps: intervals.length / ((now - begin) / 1000), p90: intervals[Math.floor(intervals.length * 0.9)]});
            } else
                requestAnimationFrame(tick);
        };
        requestAnimationFrame(tick);
    });
    const attach = (width, height, css) => {
        const canvas = document.createElement('canvas');
        canvas.width = width;
        canvas.height = height;
        canvas.style.cssText = css;
        document.body.appendChild(canvas);
        return canvas;
    };
    const rates = {empty: await measure(() => {}, 1500)};
    let frame = 0;
    for (const [name, width, height, css] of [['webgl720', 1280, 720, 'width:320px;height:180px'], ['webgl180', 320, 180, 'width:320px;height:180px']]) {
        const canvas = attach(width, height, css);
        const gl = canvas.getContext('webgl2');
        const paint = () => { frame++; gl.clearColor(frame % 2, 0.5, 0.2, 1); gl.clear(gl.COLOR_BUFFER_BIT); };
        await measure(paint, 300);
        rates[name] = await measure(paint, 2000);
        canvas.remove();
    }
    const canvas = attach(1280, 720, 'width:320px;height:180px');
    const context = canvas.getContext('2d');
    const paint = () => { frame++; context.fillStyle = frame % 2 ? 'red' : 'blue'; context.fillRect(0, 0, 1280, 720); };
    await measure(paint, 300);
    rates.canvas720 = await measure(paint, 2000);
    canvas.remove();
    window.animationRates = rates;
})().catch(error => window.animationRates = {error: String(error)});
)JS");
        QTRY_VERIFY_WITH_TIMEOUT(evaluate(view, "!!window.animationRates").toBool(), 60000);
        const QJsonObject rates = QJsonDocument::fromJson(evaluate(view, "JSON.stringify(window.animationRates)").toString().toUtf8()).object();
        qInfo().noquote() << "Animation rates" << QJsonDocument(rates).toJson(QJsonDocument::Compact);
        QCOMPARE(rates.value("error").toString(), QString());
        const double displayFps = rates.value("empty").toObject().value("fps").toDouble();
        QVERIFY2(displayFps >= 50, qPrintable(QString("An empty animation loop ran at %1 fps").arg(displayFps)));
        for (const char *name : {"webgl720", "webgl180", "canvas720"}) {
            const QJsonObject rate = rates.value(name).toObject();
            QVERIFY2(rate.value("fps").toDouble() >= displayFps * 0.9, qPrintable(QString("A page redrawing a %1 canvas every frame ran at %2 of %3 fps").arg(name).arg(rate.value("fps").toDouble()).arg(displayFps)));
            QVERIFY2(rate.value("p90").toDouble() <= 1500 / displayFps, qPrintable(QString("A page redrawing a %1 canvas every frame had a 90th percentile frame of %2 ms").arg(name).arg(rate.value("p90").toDouble())));
        }
    }

    void audibleMediaPlaysAfterStickyActivation_data() {
        QTest::addColumn<bool>("denyAutoplay");
        QTest::newRow("default policy") << false;
        QTest::newRow("deny policy") << true;
    }

    void audibleMediaPlaysAfterStickyActivation() {
        QFETCH(bool, denyAutoplay);
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(400, 300);
        view.attach(window.contentItem());
        window.show();
        gulong policyHandler = 0;
        if (denyAutoplay) {
            policyHandler = g_signal_connect(
                view.nativeView(),
                "decide-policy",
                G_CALLBACK(+[](WebKitWebView *, WebKitPolicyDecision *decision, WebKitPolicyDecisionType type, gpointer) -> gboolean {
                    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
                        return FALSE;
                    }
                    WebKitWebsitePolicies *policies = webkit_website_policies_new_with_policies("autoplay", WEBKIT_AUTOPLAY_DENY, nullptr);
                    webkit_policy_decision_use_with_policies(decision, policies);
                    g_object_unref(policies);
                    return TRUE;
                }),
                nullptr
            );
        }
        const auto disconnectPolicy = qScopeGuard([&] {
            if (policyHandler) {
                g_signal_handler_disconnect(view.nativeView(), policyHandler);
            }
        });
        view.load(url("media-autoplay"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("media-autoplay"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(evaluate(view, "!!(window.initialAutoplay && window.initialAutoplay.audible && window.initialAutoplay.muted)").toBool(), 15000);
        evaluate(view, R"JS(window.mediaAutoplay = null;
window.mediaAutoplayStage = 'starting';
(async () => {
    const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
    const result = {initial: window.initialAutoplay};
    window.mediaAutoplayPartial = result;
    const stage = name => { window.mediaAutoplayStage = name; };
    await delay(1500);
    result.activation = {isActive: navigator.userActivation.isActive, hasBeenActive: navigator.userActivation.hasBeenActive};
    const plays = async audio => {
        let playing = false;
        audio.addEventListener('playing', () => { playing = true; }, {once: true});
        const outcome = await Promise.race([audio.play().then(() => 'played', error => error.name), delay(5000).then(() => 'timed out')]);
        const playingAtResolve = playing;
        const before = audio.currentTime;
        await delay(400);
        const summary = {outcome, playingAtResolve, paused: audio.paused, advanced: audio.currentTime > before};
        audio.pause();
        return summary;
    };
    stage('audible');
    const audible = new Audio(window.toneURL);
    audible.loop = true;
    result.audible = await plays(audible);
    stage('element');
    const element = document.createElement('audio');
    element.loop = true;
    element.src = window.toneURL;
    document.body.append(element);
    result.element = await plays(element);
    stage('autoplay attribute');
    result.autoplayAttribute = await new Promise(resolve => {
        const autoplaying = document.createElement('audio');
        autoplaying.autoplay = true;
        autoplaying.loop = true;
        const timer = setTimeout(() => resolve({outcome: 'timed out', paused: autoplaying.paused}), 5000);
        autoplaying.addEventListener('playing', () => {
            clearTimeout(timer);
            const summary = {outcome: 'playing', paused: autoplaying.paused};
            autoplaying.pause();
            resolve(summary);
        }, {once: true});
        autoplaying.src = window.toneURL;
        document.body.append(autoplaying);
    });
    stage('unmute');
    const unmuted = new Audio(window.toneURL);
    unmuted.loop = true;
    unmuted.muted = true;
    const mutedOutcome = await unmuted.play().then(() => 'played', error => error.name);
    unmuted.muted = false;
    await delay(400);
    result.unmute = {mutedOutcome, paused: unmuted.paused, muted: unmuted.muted};
    unmuted.pause();
    stage('frames');
    const frameScript = `
(async () => {
    const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
    const tone = ${window.toneSource};
    const audio = new Audio(tone());
    audio.loop = true;
    const outcome = await Promise.race([audio.play().then(() => 'played', error => error.name), delay(5000).then(() => 'timed out')]);
    audio.pause();
    parent.postMessage({frameMedia: {outcome, hasBeenActive: navigator.userActivation.hasBeenActive}}, '*');
})().catch(error => parent.postMessage({frameMedia: {error: String(error && error.message || error)}}, '*'));
`;
    const frameMedia = attributes => new Promise(resolve => {
        const iframe = document.createElement('iframe');
        for (const [name, value] of Object.entries(attributes))
            iframe.setAttribute(name, value);
        const listener = event => {
            if (event.source !== iframe.contentWindow || !event.data || !event.data.frameMedia)
                return;
            window.removeEventListener('message', listener);
            iframe.remove();
            resolve(event.data.frameMedia);
        };
        window.addEventListener('message', listener);
        setTimeout(() => resolve({timeout: true}), 15000);
        iframe.srcdoc = '<script>' + frameScript + '</' + 'script>';
        document.body.appendChild(iframe);
    });
    result.sameOriginFrame = await frameMedia({});
    result.crossOriginFrame = await frameMedia({sandbox: 'allow-scripts'});
    result.delegatedFrame = await frameMedia({sandbox: 'allow-scripts', allow: 'autoplay *'});
    window.mediaAutoplay = result;
})().catch(error => window.mediaAutoplay = {error: String(error && error.message || error), stage: window.mediaAutoplayStage});
)JS");
        QElapsedTimer waited;
        waited.start();
        while (!evaluate(view, "!!window.mediaAutoplay").toBool() && waited.elapsed() < 120000) {
            QTest::qWait(100);
        }
        const QJsonObject result = QJsonDocument::fromJson(evaluate(view, "JSON.stringify(window.mediaAutoplay || {stage: window.mediaAutoplayStage, partial: window.mediaAutoplayPartial})").toString().toUtf8()).object();
        qInfo().noquote() << "Media autoplay" << (denyAutoplay ? "deny" : "default") << QJsonDocument(result).toJson(QJsonDocument::Compact);
        QVERIFY2(!result.contains("error") && !result.contains("stage"), qPrintable(QString("The media fixture stopped at %1: %2").arg(result.value("stage").toString(), result.value("error").toString())));
        const QJsonObject initial = result.value("initial").toObject();
        QVERIFY2(!initial.value("hasBeenActive").toBool(true), "The page must try playback before any user activation");
        QCOMPARE(initial.value("audible").toString(), QString("NotAllowedError"));
        QCOMPARE(initial.value("muted").toString(), QString("played"));
        const QJsonObject activation = result.value("activation").toObject();
        QVERIFY2(!activation.value("isActive").toBool(true) && activation.value("hasBeenActive").toBool(), "The checks must run after transient activation expired, with sticky activation left");
        const auto describe = [](const QJsonObject &object) { return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)); };
        for (const char *name : {"audible", "element"}) {
            const QJsonObject step = result.value(name).toObject();
            if (denyAutoplay) {
                QVERIFY2(step.value("outcome").toString() == "NotAllowedError" && step.value("paused").toBool(), qPrintable(QString("%1: %2").arg(name, describe(step))));
            } else {
                QVERIFY2(step.value("outcome").toString() == "played" && step.value("playingAtResolve").toBool() && !step.value("paused").toBool(true) && step.value("advanced").toBool(), qPrintable(QString("%1: %2").arg(name, describe(step))));
            }
        }
        const QJsonObject autoplayAttribute = result.value("autoplayAttribute").toObject();
        QCOMPARE(autoplayAttribute.value("outcome").toString(), QString(denyAutoplay ? "timed out" : "playing"));
        QVERIFY2(autoplayAttribute.value("paused").toBool() == denyAutoplay, qPrintable(describe(autoplayAttribute)));
        const QJsonObject unmute = result.value("unmute").toObject();
        QCOMPARE(unmute.value("mutedOutcome").toString(), QString("played"));
        QVERIFY2(unmute.value("paused").toBool() == denyAutoplay && !unmute.value("muted").toBool(true), qPrintable(describe(unmute)));
        const auto expectFrame = [&](const QString &name, bool allowed) {
            const QJsonObject frame = result.value(name).toObject();
            QVERIFY2(!frame.contains("timeout") && !frame.contains("error"), qPrintable(name + ": " + describe(frame)));
            QCOMPARE(frame.value("outcome").toString(), QString(allowed ? "played" : "NotAllowedError"));
        };
        expectFrame("sameOriginFrame", !denyAutoplay);
        expectFrame("crossOriginFrame", false);
        expectFrame("delegatedFrame", !denyAutoplay);
    }

    void webglCanvasIsAVideoSource() {
        const QByteArray previousMode = qgetenv("EDEN_WPE_GPU_PROCESS");
        const auto restoreMode = qScopeGuard([&] {
            if (previousMode.isNull()) {
                qunsetenv("EDEN_WPE_GPU_PROCESS");
            } else {
                qputenv("EDEN_WPE_GPU_PROCESS", previousMode);
            }
        });
        QList<QPair<QString, QJsonObject>> modes;
        for (const char *mode : {"1", "0"}) {
            qputenv("EDEN_WPE_GPU_PROCESS", mode);
            const QString label = QString(mode) == "1" ? "WebGL in the GPU process" : "WebGL in the web process";
            WpeProfile profile(privateParameters());
            WpeEngineView view(&profile);
            QQuickWindow window;
            window.resize(900, 600);
            view.attach(window.contentItem());
            window.show();
            webkit_settings_set_media_playback_requires_user_gesture(webkit_web_view_get_settings(view.nativeView()), FALSE);
            view.load(url("webgl-source"));
            QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("webgl-source"), 15000);
            evaluate(view, R"JS(window.webglSource = null;
window.webglSourceStage = 'starting';
(async () => {
    const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
    const classify = (r, g, b) => r > 180 && g > 180 && b < 90 ? 'Y' : r > 180 && g < 90 && b < 90 ? 'R' : g > 180 && r < 90 && b < 90 ? 'G' : b > 180 && r < 90 && g < 90 ? 'B' : '.';
    const gridOf = (pixelAt, width, height) => [0, 1].map(row => [0, 1].map(column => classify(...pixelAt(Math.floor((column + 0.5) * width / 2), Math.floor((row + 0.5) * height / 2)))).join(''));
    const canvasGrid = source => {
        const probe = document.createElement('canvas');
        probe.width = 320;
        probe.height = 180;
        const context = probe.getContext('2d', {willReadFrequently: true});
        context.drawImage(source, 0, 0, 320, 180);
        const data = context.getImageData(0, 0, 320, 180).data;
        return gridOf((x, y) => [data[(y * 320 + x) * 4], data[(y * 320 + x) * 4 + 1], data[(y * 320 + x) * 4 + 2]], 320, 180);
    };
    const presented = (video, count, timeout) => new Promise(resolve => {
        let frames = 0;
        const timer = setTimeout(() => resolve(frames), timeout);
        const next = () => {
            if (++frames >= count) {
                clearTimeout(timer);
                resolve(frames);
            } else
                video.requestVideoFrameCallback(next);
        };
        video.requestVideoFrameCallback(next);
    });
    const makeVideo = stream => {
        const video = document.createElement('video');
        video.muted = true;
        video.playsInline = true;
        video.srcObject = stream;
        video.style.cssText = 'width:160px;height:90px';
        document.body.appendChild(video);
        video.play().catch(() => {});
        return video;
    };
    const result = {};
    window.webglSourcePartial = result;
    const stage = name => { window.webglSourceStage = name; };
    stage('empty frames');
    result.emptyFrameFps = await new Promise(resolve => {
        let frames = 0;
        let begin = 0;
        const tick = now => {
            if (!begin)
                begin = now;
            else
                frames++;
            if (now - begin >= 1500)
                resolve(Math.round(frames / ((now - begin) / 1000)));
            else
                requestAnimationFrame(tick);
        };
        requestAnimationFrame(tick);
    });
    stage('cold context');
    let start = performance.now();
    const cold = document.createElement('canvas').getContext('webgl2');
    cold.clearColor(0, 0, 0, 1);
    cold.clear(cold.COLOR_BUFFER_BIT);
    cold.readPixels(0, 0, 1, 1, cold.RGBA, cold.UNSIGNED_BYTE, new Uint8Array(4));
    result.coldContextMs = Math.round(performance.now() - start);
    start = performance.now();
    const warm = document.createElement('canvas').getContext('webgl2');
    warm.clearColor(0, 0, 0, 1);
    warm.clear(warm.COLOR_BUFFER_BIT);
    warm.readPixels(0, 0, 1, 1, warm.RGBA, warm.UNSIGNED_BYTE, new Uint8Array(4));
    result.warmContextMs = Math.round(performance.now() - start);
    stage('painting');
    const canvas = document.createElement('canvas');
    canvas.width = 1280;
    canvas.height = 720;
    canvas.style.cssText = 'width:320px;height:180px';
    document.body.appendChild(canvas);
    const gl = canvas.getContext('webgl2');
    const quadrants = [[0, 360, [1, 0, 0]], [640, 360, [0, 1, 0]], [0, 0, [0, 0, 1]], [640, 0, [1, 1, 0]]];
    const paint = () => {
        gl.enable(gl.SCISSOR_TEST);
        for (const [x, y, [r, g, b]] of quadrants) {
            gl.scissor(x, y, 640, 360);
            gl.clearColor(r, g, b, 1);
            gl.clear(gl.COLOR_BUFFER_BIT);
        }
        gl.disable(gl.SCISSOR_TEST);
    };
    let painting = true;
    let paints = 0;
    const loop = () => {
        if (!painting)
            return;
        paint();
        paints++;
        requestAnimationFrame(loop);
    };
    requestAnimationFrame(loop);
    stage('measuring painting alone');
    await delay(500);
    const baselinePaints = paints;
    const baselineStart = performance.now();
    await delay(2000);
    result.baselinePaintFps = Math.round((paints - baselinePaints) / ((performance.now() - baselineStart) / 1000));
    const stream = canvas.captureStream(30);
    const track = stream.getVideoTracks()[0];
    const local = makeVideo(stream);
    stage('measuring capture');
    let worstLag = 0;
    let lastTick = performance.now();
    const ticker = setInterval(() => {
        const now = performance.now();
        worstLag = Math.max(worstLag, now - lastTick - 10);
        lastTick = now;
    }, 10);
    const paintsBefore = paints;
    const measureStart = performance.now();
    result.capturedFrames = await presented(local, 1000, 3000);
    const measuredSeconds = (performance.now() - measureStart) / 1000;
    clearInterval(ticker);
    result.capturedFps = Math.round(result.capturedFrames / measuredSeconds);
    result.paintFps = Math.round((paints - paintsBefore) / measuredSeconds);
    result.worstLagMs = Math.round(worstLag);
    result.captureGrid = canvasGrid(local);
    stage('video frame');
    try {
        start = performance.now();
        let frame = null;
        for (let index = 0; index < 10; index++) {
            if (frame)
                frame.close();
            paint();
            frame = new VideoFrame(canvas, {timestamp: index});
        }
        result.videoFrameMs = Math.round((performance.now() - start) / 10 * 10) / 10;
        const pixels = new Uint8Array(frame.allocationSize({format: 'RGBA'}));
        const layout = await frame.copyTo(pixels, {format: 'RGBA'});
        result.videoFrameGrid = gridOf((x, y) => {
            const offset = layout[0].offset + y * layout[0].stride + x * 4;
            return [pixels[offset], pixels[offset + 1], pixels[offset + 2]];
        }, frame.displayWidth, frame.displayHeight);
        result.videoFrameCanvasGrid = canvasGrid(frame);
        frame.close();
    } catch (error) {
        result.videoFrameError = String(error && error.message || error);
    }
    stage('worker frames');
    result.workerGrid = await new Promise(resolve => {
        const source = `
            const classify = (r, g, b) => r > 180 && g > 180 && b < 90 ? 'Y' : r > 180 && g < 90 && b < 90 ? 'R' : g > 180 && r < 90 && b < 90 ? 'G' : b > 180 && r < 90 && g < 90 ? 'B' : '.';
            self.onmessage = async ({data}) => {
                const reader = new MediaStreamTrackProcessor({track: data.track}).readable.getReader();
                const {value: frame} = await reader.read();
                const pixels = new Uint8Array(frame.allocationSize({format: 'RGBA'}));
                const layout = await frame.copyTo(pixels, {format: 'RGBA'});
                const width = frame.displayWidth;
                const height = frame.displayHeight;
                const grid = [0, 1].map(row => [0, 1].map(column => {
                    const offset = layout[0].offset + Math.floor((row + 0.5) * height / 2) * layout[0].stride + Math.floor((column + 0.5) * width / 2) * 4;
                    return classify(pixels[offset], pixels[offset + 1], pixels[offset + 2]);
                }).join(''));
                frame.close();
                reader.cancel();
                self.postMessage(grid);
            };
        `;
        const worker = new Worker(URL.createObjectURL(new Blob([source], {type: 'text/javascript'})));
        const timer = setTimeout(() => { worker.terminate(); resolve('timeout'); }, 8000);
        worker.onmessage = event => { clearTimeout(timer); worker.terminate(); resolve(event.data); };
        worker.onerror = event => { clearTimeout(timer); worker.terminate(); resolve(String(event.message)); };
        const clone = track.clone();
        worker.postMessage({track: clone}, [clone]);
    });
    stage('remote frames');
    try {
        const sender = new RTCPeerConnection();
        const receiver = new RTCPeerConnection();
        sender.onicecandidate = event => event.candidate && receiver.addIceCandidate(event.candidate);
        receiver.onicecandidate = event => event.candidate && sender.addIceCandidate(event.candidate);
        const remoteTrack = new Promise(resolve => { receiver.ontrack = event => resolve(event.track); });
        sender.addTrack(track.clone());
        await sender.setLocalDescription();
        await receiver.setRemoteDescription(sender.localDescription);
        await receiver.setLocalDescription();
        await sender.setRemoteDescription(receiver.localDescription);
        const remote = makeVideo(new MediaStream([await remoteTrack]));
        result.remoteFrames = await presented(remote, 45, 10000);
        result.remoteGrid = canvasGrid(remote);
        sender.close();
        receiver.close();
    } catch (error) {
        result.remoteError = String(error && error.message || error);
    }
    painting = false;
    stream.getTracks().forEach(item => item.stop());
    result.contextLost = gl.isContextLost();
    window.webglSource = result;
})().catch(error => window.webglSource = {fatal: String(error && error.message || error), stage: window.webglSourceStage});
)JS");
            QElapsedTimer waited;
            waited.start();
            while (!evaluate(view, "!!window.webglSource").toBool() && waited.elapsed() < 60000) {
                QTest::qWait(100);
            }
            const QJsonObject result = QJsonDocument::fromJson(evaluate(view, "JSON.stringify(window.webglSource || {stage: window.webglSourceStage, partial: window.webglSourcePartial})").toString().toUtf8()).object();
            qInfo().noquote() << "WebGL source" << label << QJsonDocument(result).toJson(QJsonDocument::Compact);
            modes.append({label, result});
        }
        for (const auto &[label, result] : std::as_const(modes)) {
            QVERIFY2(!result.contains("fatal") && !result.contains("stage"), qPrintable(QString("%1: the fixture stopped at %2 %3").arg(label, result.value("stage").toString(), result.value("fatal").toString())));
            QVERIFY2(!result.value("contextLost").toBool(true), qPrintable(label + ": the WebGL context was lost"));
            const QJsonArray upright({"RG", "BY"});
            for (const char *name : {"captureGrid", "videoFrameGrid", "videoFrameCanvasGrid", "workerGrid", "remoteGrid"}) {
                QVERIFY2(result.value(name).toArray() == upright, qPrintable(QString("%1: %2 is %3 instead of upright RG/BY").arg(label, name, QString::fromUtf8(QJsonDocument(result.value(name).toArray()).toJson(QJsonDocument::Compact)))));
            }
            QVERIFY2(!result.contains("videoFrameError") && !result.contains("remoteError"), qPrintable(label + ": " + result.value("videoFrameError").toString() + result.value("remoteError").toString()));
            QVERIFY2(result.value("capturedFps").toInt() >= 25, qPrintable(QString("%1: a captured 720p WebGL canvas presented %2 fps").arg(label).arg(result.value("capturedFps").toInt())));
            QVERIFY2(result.value("paintFps").toInt() >= 50, qPrintable(QString("%1: the page painted at %2 fps while capturing").arg(label).arg(result.value("paintFps").toInt())));
            QVERIFY2(result.value("worstLagMs").toInt() <= 50, qPrintable(QString("%1: capturing stalled the main thread for %2 ms").arg(label).arg(result.value("worstLagMs").toInt())));
            QVERIFY2(result.value("remoteFrames").toInt() >= 45, qPrintable(QString("%1: the remote side presented %2 frames").arg(label).arg(result.value("remoteFrames").toInt())));
        }
    }

    void webglFramesStayOnTheGpu_data() {
        QTest::addColumn<bool>("hideVideoAcceleration");
        QTest::newRow("installed decoders and encoders") << false;
        QTest::newRow("without video acceleration") << true;
    }

    void webglFramesStayOnTheGpu() {
        QFETCH(bool, hideVideoAcceleration);
        QProcess inspect;
        inspect.start("gst-inspect-1.0", {"va"});
        QVERIFY(inspect.waitForFinished(60000));
        const auto filename = QRegularExpression("Filename\\s+(\\S+)").match(QString::fromUtf8(inspect.readAllStandardOutput()));
        const QString videoAccelerationPlugin = filename.hasMatch() ? filename.captured(1) : QString();
        if (!hideVideoAcceleration && videoAccelerationPlugin.isEmpty()) {
            QSKIP("The GStreamer VA plugin is not installed");
        }

        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        QVERIFY(QDir().mkpath(workspace.filePath("logs")));
        QList<QPair<QByteArray, QByteArray>> environment {
            {"GST_DEBUG", "webkitvideoframe:5,webkitvideoframeconverter:4,webkitlibwebrtcvideoencoder:4,webkitvideoencoderprivate:4" + (qEnvironmentVariableIsSet("EDEN_TEST_GST_DEBUG") ? "," + qgetenv("EDEN_TEST_GST_DEBUG") : QByteArray())},
            {"GST_DEBUG_NO_COLOR", "1"},
            {"GST_DEBUG_FILE", workspace.filePath("logs/gst-%r.log").toUtf8()},
        };
        if (hideVideoAcceleration && !videoAccelerationPlugin.isEmpty()) {
            const QFileInfo plugin(videoAccelerationPlugin);
            QVERIFY(QDir().mkpath(workspace.filePath("plugins")));
            QVERIFY(QDir().mkpath(workspace.filePath("registry")));
            for (const QFileInfo &entry : QDir(plugin.absolutePath()).entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
                if (entry.fileName() != plugin.fileName()) {
                    QVERIFY(QFile::link(entry.absoluteFilePath(), workspace.filePath("plugins/" + entry.fileName())));
                }
            }
            environment.append({"GST_PLUGIN_SYSTEM_PATH_1_0", workspace.filePath("plugins").toUtf8()});
            environment.append({"GST_REGISTRY", workspace.filePath("registry/registry.bin").toUtf8()});
            auto hiddenEnvironment = QProcessEnvironment::systemEnvironment();
            hiddenEnvironment.insert("GST_PLUGIN_SYSTEM_PATH_1_0", workspace.filePath("plugins"));
            hiddenEnvironment.insert("GST_REGISTRY", workspace.filePath("registry/registry.bin"));
            QProcess seed;
            seed.setProcessEnvironment(hiddenEnvironment);
            seed.start("gst-inspect-1.0", {"--exists", "capsfilter"});
            QVERIFY(seed.waitForFinished(300000));
            QVERIFY(QFile::exists(workspace.filePath("registry/registry.bin")));
            QProcess hidden;
            hidden.setProcessEnvironment(hiddenEnvironment);
            hidden.start("gst-inspect-1.0", {"--exists", "vapostproc"});
            QVERIFY(hidden.waitForFinished(60000));
            QVERIFY2(hidden.exitCode() != 0, "The video post-processor is still visible after hiding the VA plugin");
        }
        QList<QPair<QByteArray, QByteArray>> previous;
        for (const auto &[name, value] : std::as_const(environment)) {
            previous.append({name, qEnvironmentVariableIsSet(name.constData()) ? qgetenv(name.constData()) : QByteArray()});
            qputenv(name.constData(), value);
        }
        const QByteArray previousMode = qgetenv("EDEN_WPE_GPU_PROCESS");
        const auto restoreEnvironment = qScopeGuard([&] {
            for (const auto &[name, value] : std::as_const(previous)) {
                if (value.isNull()) {
                    qunsetenv(name.constData());
                } else {
                    qputenv(name.constData(), value);
                }
            }
            if (previousMode.isNull()) {
                qunsetenv("EDEN_WPE_GPU_PROCESS");
            } else {
                qputenv("EDEN_WPE_GPU_PROCESS", previousMode);
            }
        });

        QList<QPair<QString, QJsonObject>> modes;
        for (const char *mode : {"1", "0"}) {
            qputenv("EDEN_WPE_GPU_PROCESS", mode);
            const QString label = QString(mode) == "1" ? "WebGL in the GPU process" : "WebGL in the web process";
            {
                WpeProfile profile(privateParameters());
                WpeEngineView view(&profile);
                QQuickWindow window;
                window.resize(1400, 900);
                view.attach(window.contentItem());
                window.show();
                webkit_settings_set_media_playback_requires_user_gesture(webkit_web_view_get_settings(view.nativeView()), FALSE);
                view.load(url("webgl-dmabuf"));
                QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("webgl-dmabuf"), 15000);
                evaluate(view, R"JS(window.dmabufFrames = null;
window.dmabufFramesStage = 'starting';
(async () => {
    const result = {};
    window.dmabufFramesPartial = result;
    const stage = name => { window.dmabufFramesStage = name; };
    const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
    const classify = (r, g, b) => r > 180 && g > 180 && b < 90 ? 'Y' : r > 180 && g < 90 && b < 90 ? 'R' : g > 180 && r < 90 && b < 90 ? 'G' : b > 180 && r < 90 && g < 90 ? 'B' : '.';
    const gridOf = (pixelAt, width, height) => [0, 1].map(row => [0, 1].map(column => classify(...pixelAt(Math.floor((column + 0.5) * width / 2), Math.floor((row + 0.5) * height / 2)))).join(''));
    const canvasGrid = source => {
        const probe = document.createElement('canvas');
        probe.width = 320;
        probe.height = 180;
        const context = probe.getContext('2d', {willReadFrequently: true});
        context.drawImage(source, 0, 0, 320, 180);
        const data = context.getImageData(0, 0, 320, 180).data;
        return gridOf((x, y) => [data[(y * 320 + x) * 4], data[(y * 320 + x) * 4 + 1], data[(y * 320 + x) * 4 + 2]], 320, 180);
    };
    const presented = (video, count, timeout) => new Promise(resolve => {
        let frames = 0;
        const timer = setTimeout(() => resolve(frames), timeout);
        const next = () => {
            if (++frames >= count) {
                clearTimeout(timer);
                resolve(frames);
            } else
                video.requestVideoFrameCallback(next);
        };
        video.requestVideoFrameCallback(next);
    });
    const makeVideo = stream => {
        const video = document.createElement('video');
        video.muted = true;
        video.playsInline = true;
        video.srcObject = stream;
        video.style.cssText = 'width:160px;height:90px';
        document.body.appendChild(video);
        video.play().catch(() => {});
        return video;
    };
    const canvas = document.createElement('canvas');
    canvas.width = 1280;
    canvas.height = 720;
    canvas.style.cssText = 'width:320px;height:180px';
    document.body.appendChild(canvas);
    const gl = canvas.getContext('webgl2');
    const paint = () => {
        const width = canvas.width;
        const height = canvas.height;
        gl.viewport(0, 0, width, height);
        gl.enable(gl.SCISSOR_TEST);
        for (const [x, y, [r, g, b]] of [[0, height / 2, [1, 0, 0]], [width / 2, height / 2, [0, 1, 0]], [0, 0, [0, 0, 1]], [width / 2, 0, [1, 1, 0]]]) {
            gl.scissor(x, y, width / 2, height / 2);
            gl.clearColor(r, g, b, 1);
            gl.clear(gl.COLOR_BUFFER_BIT);
        }
        gl.disable(gl.SCISSOR_TEST);
    };
    let painting = true;
    const loop = () => {
        if (!painting)
            return;
        paint();
        requestAnimationFrame(loop);
    };
    requestAnimationFrame(loop);

    stage('local preview');
    const stream = canvas.captureStream(30);
    const track = stream.getVideoTracks()[0];
    const local = makeVideo(stream);
    result.localFrames = await presented(local, 60, 6000);
    result.localGrid = canvasGrid(local);

    stage('remote H.264 at half size');
    try {
        const sender = new RTCPeerConnection();
        const receiver = new RTCPeerConnection();
        sender.onicecandidate = event => event.candidate && receiver.addIceCandidate(event.candidate);
        receiver.onicecandidate = event => event.candidate && sender.addIceCandidate(event.candidate);
        const remoteTrack = new Promise(resolve => { receiver.ontrack = event => resolve(event.track); });
        const transceiver = sender.addTransceiver(track.clone(), {direction: 'sendonly', sendEncodings: [{scaleResolutionDownBy: 2}]});
        const codecs = RTCRtpSender.getCapabilities('video').codecs;
        const h264 = codecs.filter(codec => codec.mimeType === 'video/H264');
        result.h264Offered = h264.length > 0;
        transceiver.setCodecPreferences([...h264, ...codecs.filter(codec => codec.mimeType !== 'video/H264')]);
        await sender.setLocalDescription();
        await receiver.setRemoteDescription(sender.localDescription);
        await receiver.setLocalDescription();
        await sender.setRemoteDescription(receiver.localDescription);
        const remote = makeVideo(new MediaStream([await remoteTrack]));
        result.remoteFrames = await presented(remote, 45, 12000);
        result.remoteSize = [remote.videoWidth, remote.videoHeight];
        result.remoteGrid = canvasGrid(remote);
        const stats = await sender.getStats();
        stats.forEach(report => {
            if (report.type === 'outbound-rtp' && report.kind === 'video') {
                result.remoteEncoder = report.encoderImplementation || '';
                result.remoteCodec = (stats.get(report.codecId) || {}).mimeType || '';
                result.remoteSentSize = [report.frameWidth || 0, report.frameHeight || 0];
                result.remoteBytesSent = report.bytesSent || 0;
                result.remoteFramesEncoded = report.framesEncoded || 0;
            }
        });
        sender.close();
        receiver.close();
    } catch (error) {
        result.remoteError = String(error && error.message || error);
    }

    stage('resized 2D canvas call');
    try {
        const source2d = document.createElement('canvas');
        source2d.width = 1280;
        source2d.height = 720;
        const context2d = source2d.getContext('2d');
        let frameIndex = 0;
        let painting2d = true;
        const paint2d = () => {
            if (!painting2d)
                return;
            context2d.fillStyle = `hsl(${(frameIndex++ * 7) % 360}, 80%, 50%)`;
            context2d.fillRect(0, 0, source2d.width, source2d.height);
            requestAnimationFrame(paint2d);
        };
        requestAnimationFrame(paint2d);
        const stream2d = source2d.captureStream(30);
        const sender = new RTCPeerConnection();
        const receiver = new RTCPeerConnection();
        sender.onicecandidate = event => event.candidate && receiver.addIceCandidate(event.candidate);
        receiver.onicecandidate = event => event.candidate && sender.addIceCandidate(event.candidate);
        const remoteTrack = new Promise(resolve => { receiver.ontrack = event => resolve(event.track); });
        const transceiver = sender.addTransceiver(stream2d.getVideoTracks()[0], {direction: 'sendonly'});
        const codecs = RTCRtpSender.getCapabilities('video').codecs;
        transceiver.setCodecPreferences([...codecs.filter(codec => codec.mimeType === 'video/H264'), ...codecs.filter(codec => codec.mimeType !== 'video/H264')]);
        await sender.setLocalDescription();
        await receiver.setRemoteDescription(sender.localDescription);
        await receiver.setLocalDescription();
        await sender.setRemoteDescription(receiver.localDescription);
        const remote = makeVideo(new MediaStream([await remoteTrack]));
        result.resizedCall = {before: await presented(remote, 20, 10000), sizeBefore: [remote.videoWidth, remote.videoHeight]};
        source2d.width = 640;
        source2d.height = 360;
        await delay(1000);
        result.resizedCall.after = await presented(remote, 20, 10000);
        result.resizedCall.sizeAfter = [remote.videoWidth, remote.videoHeight];
        painting2d = false;
        stream2d.getTracks().forEach(item => item.stop());
        sender.close();
        receiver.close();
    } catch (error) {
        result.resizedCall = {error: String(error && error.message || error)};
    }

    const record = async mimeType => {
        const recorder = new MediaRecorder(stream, mimeType ? {mimeType} : {});
        const chunks = [];
        recorder.ondataavailable = event => event.data.size && chunks.push(event.data);
        const stopped = new Promise(resolve => { recorder.onstop = resolve; });
        recorder.start(500);
        await delay(2500);
        recorder.stop();
        await stopped;
        const bytes = chunks.reduce((total, chunk) => total + chunk.size, 0);
        const playback = document.createElement('video');
        playback.muted = true;
        playback.src = URL.createObjectURL(new Blob(chunks, {type: recorder.mimeType}));
        document.body.appendChild(playback);
        await Promise.race([new Promise(resolve => playback.addEventListener('loadeddata', resolve, {once: true})), delay(8000)]);
        await playback.play().catch(() => {});
        await Promise.race([presented(playback, 3, 4000), delay(4000)]);
        const grid = canvasGrid(playback);
        playback.pause();
        playback.remove();
        return {bytes, grid, mimeType: recorder.mimeType};
    };

    stage('media recorder');
    try {
        const recording = await record(['video/webm;codecs=vp8', 'video/webm', 'video/mp4'].find(candidate => MediaRecorder.isTypeSupported(candidate)));
        result.recordedBytes = recording.bytes;
        result.recordedGrid = recording.grid;
    } catch (error) {
        result.recorderError = String(error && error.message || error);
    }

    stage('mp4 media recorder');
    try {
        if (MediaRecorder.isTypeSupported('video/mp4'))
            result.mp4Recording = await record('video/mp4');
        else
            result.mp4Recording = {unsupported: true};
    } catch (error) {
        result.mp4Recording = {error: String(error && error.message || error)};
    }

    stage('webcodecs');
    {
        const opaque = new VideoFrame(canvas, {timestamp: 0});
        result.opaqueFormat = opaque.format;
        try {
            const bytes = new Uint8Array(opaque.allocationSize());
            await opaque.copyTo(bytes);
            result.opaqueCopy = 'ok';
        } catch (error) {
            result.opaqueCopy = String(error && error.name || error);
        }
        opaque.close();
    }
    result.encoders = {};
    for (const codec of ['vp8', 'avc1.42001f']) {
        const config = {codec, width: 1280, height: 720, bitrate: 2000000, framerate: 30, latencyMode: 'realtime'};
        if (codec.startsWith('avc1'))
            config.avc = {format: 'annexb'};
        const support = await VideoEncoder.isConfigSupported(config).catch(() => ({supported: false}));
        if (!support.supported) {
            result.encoders[codec] = {unsupported: true};
            continue;
        }
        const chunks = [];
        let decoderConfig = null;
        let error = '';
        const encoder = new VideoEncoder({output: (chunk, metadata) => {
            if (metadata && metadata.decoderConfig)
                decoderConfig = metadata.decoderConfig;
            chunks.push(chunk);
        }, error: failure => { error = String(failure); }});
        encoder.configure(config);
        for (let index = 0; index < 10; index++) {
            paint();
            const frame = new VideoFrame(canvas, {timestamp: index * 33333});
            encoder.encode(frame, {keyFrame: index === 0});
            frame.close();
            await delay(15);
        }
        await encoder.flush().catch(failure => { error = String(failure); });
        encoder.close();
        let decodedGrid = null;
        if (decoderConfig && chunks.length) {
            decodedGrid = await new Promise(resolve => {
                const decoder = new VideoDecoder({output: frame => {
                    const grid = canvasGrid(frame);
                    frame.close();
                    resolve(grid);
                }, error: failure => resolve(String(failure))});
                decoder.configure(decoderConfig);
                decoder.decode(chunks[0]);
                decoder.flush().catch(() => {});
                setTimeout(() => resolve('timeout'), 5000);
            });
        }
        const first = chunks[0];
        result.encoders[codec] = {chunks: chunks.length, error, decodedGrid, firstType: first && first.type, firstBytes: first && first.byteLength, decoderConfig: decoderConfig && {codec: decoderConfig.codec, codedWidth: decoderConfig.codedWidth, codedHeight: decoderConfig.codedHeight, description: !!decoderConfig.description}};
    }

    stage('webcodecs bitrate');
    try {
        const config = {codec: 'avc1.42001f', width: 640, height: 360, bitrate: 200000, framerate: 30, avc: {format: 'annexb'}};
        const support = await VideoEncoder.isConfigSupported(config).catch(() => ({supported: false}));
        if (support.supported) {
            const source = document.createElement('canvas');
            source.width = 640;
            source.height = 360;
            const context = source.getContext('2d');
            const pattern = document.createElement('canvas');
            pattern.width = 640;
            pattern.height = 360;
            const patternContext = pattern.getContext('2d');
            const bars = ['#c0c0c0', '#c0c000', '#00c0c0', '#00c000', '#c000c0', '#c00000', '#0000c0'];
            bars.forEach((color, bar) => {
                patternContext.fillStyle = color;
                patternContext.fillRect(Math.floor(bar * 640 / 7), 0, Math.ceil(640 / 7), 240);
                patternContext.fillStyle = bar % 2 ? '#131313' : bars[6 - bar];
                patternContext.fillRect(Math.floor(bar * 640 / 7), 240, Math.ceil(640 / 7), 40);
            });
            const ramp = patternContext.createLinearGradient(0, 0, 640, 0);
            ramp.addColorStop(0, '#000000');
            ramp.addColorStop(1, '#ffffff');
            patternContext.fillStyle = ramp;
            patternContext.fillRect(0, 280, 640, 80);
            let steadyBytes = 0;
            let count = 0;
            let error = '';
            const encoder = new VideoEncoder({output: chunk => {
                if (count >= 30)
                    steadyBytes += chunk.byteLength;
                count++;
            }, error: failure => { error = String(failure); }});
            encoder.configure(config);
            for (let index = 0; index < 90; index++) {
                const offset = (index * 7) % 640;
                context.drawImage(pattern, offset, 0, 640 - offset, 360, 0, 0, 640 - offset, 360);
                if (offset)
                    context.drawImage(pattern, 0, 0, offset, 360, 640 - offset, 0, offset, 360);
                const frame = new VideoFrame(source, {timestamp: index * 33333});
                encoder.encode(frame, {keyFrame: index === 0});
                frame.close();
                if (index % 10 === 9)
                    await delay(10);
            }
            await encoder.flush().catch(failure => { error = String(failure); });
            encoder.close();
            result.qualityBitrate = {steadyBytes, chunks: count, error, steadyTargetBytes: config.bitrate / 8 * 2};
        } else
            result.qualityBitrate = {unsupported: true};
    } catch (error) {
        result.qualityBitrate = {error: String(error && error.message || error)};
    }

    stage('alpha');
    result.alpha = {};
    for (const premultipliedAlpha of [true, false]) {
        const alphaCanvas = document.createElement('canvas');
        alphaCanvas.width = 64;
        alphaCanvas.height = 64;
        const alphaContext = alphaCanvas.getContext('webgl2', {alpha: true, premultipliedAlpha});
        if (premultipliedAlpha)
            alphaContext.clearColor(0.5, 0, 0, 0.5);
        else
            alphaContext.clearColor(1, 0, 0, 0.5);
        alphaContext.clear(alphaContext.COLOR_BUFFER_BIT);
        const frame = new VideoFrame(alphaCanvas, {timestamp: 0});
        result.alphaFormats = result.alphaFormats || {};
        result.alphaFormats[premultipliedAlpha ? 'premultiplied' : 'straight'] = frame.format;
        const pixels = new Uint8Array(frame.allocationSize({format: 'RGBA'}));
        const layout = await frame.copyTo(pixels, {format: 'RGBA'});
        const offset = layout[0].offset + 32 * layout[0].stride + 32 * 4;
        result.alpha[premultipliedAlpha ? 'premultiplied' : 'straight'] = Array.from(pixels.slice(offset, offset + 4));
        frame.close();
    }

    stage('resize');
    canvas.width = 640;
    canvas.height = 360;
    await presented(local, 20, 4000);
    await delay(200);
    result.resizedSize = [local.videoWidth, local.videoHeight];
    result.resizedGrid = canvasGrid(local);

    painting = false;
    stream.getTracks().forEach(item => item.stop());
    result.contextLost = gl.isContextLost();
    window.dmabufFrames = result;
})().catch(error => window.dmabufFrames = {fatal: String(error && error.message || error), stage: window.dmabufFramesStage});
)JS");
                QElapsedTimer waited;
                waited.start();
                while (!evaluate(view, "!!window.dmabufFrames").toBool() && waited.elapsed() < 120000) {
                    QTest::qWait(100);
                }
                const QJsonObject result = QJsonDocument::fromJson(evaluate(view, "JSON.stringify(window.dmabufFrames || {stage: window.dmabufFramesStage, partial: window.dmabufFramesPartial})").toString().toUtf8()).object();
                qInfo().noquote() << "DMA-BUF frames" << label << QJsonDocument(result).toJson(QJsonDocument::Compact);
                modes.append({label, result});
            }
            QTest::qWait(500);
        }

        QString log;
        const QString artifacts = qEnvironmentVariable("EDEN_TEST_ARTIFACTS");
        for (const QFileInfo &entry : QDir(workspace.filePath("logs")).entryInfoList(QDir::Files)) {
            QFile file(entry.absoluteFilePath());
            if (file.open(QIODevice::ReadOnly)) {
                log += QString::fromUtf8(file.readAll());
            }
            if (!artifacts.isEmpty() && QDir().mkpath(artifacts)) {
                QFile::copy(entry.absoluteFilePath(), QDir(artifacts).filePath(QString(QTest::currentDataTag()).replace(' ', '-') + "-" + entry.fileName()));
            }
        }
        const auto describe = [](const QJsonValue &value) {
            return value.isArray() ? QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact))
                                   : value.isObject() ? QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact))
                                                      : value.toVariant().toString();
        };
        const QJsonArray upright({"RG", "BY"});
        for (const auto &[label, result] : std::as_const(modes)) {
            QVERIFY2(!result.contains("fatal") && !result.contains("stage"), qPrintable(QString("%1: the fixture stopped at %2 %3").arg(label, result.value("stage").toString(), result.value("fatal").toString())));
            QVERIFY2(!result.value("contextLost").toBool(true), qPrintable(label + ": the WebGL context was lost"));
            QVERIFY2(!result.contains("remoteError") && !result.contains("recorderError"), qPrintable(label + ": " + result.value("remoteError").toString() + result.value("recorderError").toString()));
            for (const char *name : {"localGrid", "remoteGrid", "recordedGrid", "resizedGrid"}) {
                QVERIFY2(result.value(name).toArray() == upright, qPrintable(QString("%1: %2 is %3 instead of upright RG/BY").arg(label, name, describe(result.value(name)))));
            }
            QVERIFY2(result.value("localFrames").toInt() >= 60, qPrintable(QString("%1: the local preview presented %2 frames").arg(label).arg(result.value("localFrames").toInt())));
            QVERIFY2(result.value("remoteFrames").toInt() >= 45, qPrintable(QString("%1: the remote side presented %2 frames").arg(label).arg(result.value("remoteFrames").toInt())));
            const QJsonArray remoteSize = result.value("remoteSize").toArray();
            const QJsonArray remoteSentSize = result.value("remoteSentSize").toArray();
            QVERIFY2(remoteSize.size() == 2 && remoteSize[0].toInt() > 0 && remoteSize[0].toInt() <= 640 && remoteSize[1].toInt() > 0 && remoteSize[1].toInt() <= 360 && qAbs(remoteSize[0].toInt() * 9 - remoteSize[1].toInt() * 16) <= 32, qPrintable(QString("%1: a half size 1280x720 send arrived as %2").arg(label, describe(remoteSize))));
            QVERIFY2(remoteSentSize.size() == 2 && remoteSentSize[0].toInt() > 0 && remoteSentSize[0].toInt() <= 640, qPrintable(QString("%1: the sender encoded %2 for a half size 1280x720 track").arg(label, describe(remoteSentSize))));
            if (result.value("h264Offered").toBool()) {
                QCOMPARE(result.value("remoteCodec").toString(), QString("video/H264"));
            }
            const QJsonObject resizedCall = result.value("resizedCall").toObject();
            QVERIFY2(resizedCall.value("before").toInt() >= 20 && resizedCall.value("after").toInt() >= 20, qPrintable(QString("%1: a call from a 2D canvas stopped when the canvas was resized: %2").arg(label, describe(resizedCall))));
            const double bytesPerFrame = result.value("remoteBytesSent").toDouble() / qMax(1, result.value("remoteFramesEncoded").toInt());
            QVERIFY2(result.value("remoteFramesEncoded").toInt() > 0 && bytesPerFrame < 2000, qPrintable(QString("%1: a call sending a still picture spent %2 bytes per frame over %3 frames").arg(label).arg(bytesPerFrame, 0, 'f', 0).arg(result.value("remoteFramesEncoded").toInt())));
            QVERIFY2(result.value("recordedBytes").toInt() > 10000, qPrintable(QString("%1: the recording holds %2 bytes").arg(label).arg(result.value("recordedBytes").toInt())));
            const QJsonObject mp4Recording = result.value("mp4Recording").toObject();
            if (!mp4Recording.contains("unsupported")) {
                QVERIFY2(mp4Recording.value("bytes").toInt() > 2000 && mp4Recording.value("grid").toArray() == upright, qPrintable(QString("%1: an MP4 recording came out as %2").arg(label, describe(mp4Recording))));
                QVERIFY2(mp4Recording.value("bytes").toInt() < 400000, qPrintable(QString("%1: 2.5 seconds of a still picture recorded as %2 bytes of MP4, so the encoder is padding its output").arg(label).arg(mp4Recording.value("bytes").toInt())));
            }
            QCOMPARE(result.value("resizedSize").toArray(), QJsonArray({640, 360}));
            const QJsonObject encoders = result.value("encoders").toObject();
            QVERIFY2(!encoders.value("vp8").toObject().contains("unsupported"), qPrintable(label + ": WebCodecs cannot encode VP8"));
            for (const QString &codec : encoders.keys()) {
                const QJsonObject encoder = encoders.value(codec).toObject();
                if (encoder.contains("unsupported")) {
                    continue;
                }
                QVERIFY2(encoder.value("chunks").toInt() == 10 && encoder.value("error").toString().isEmpty(), qPrintable(QString("%1: %2 encoding produced %3").arg(label, codec, describe(encoder))));
                QVERIFY2(encoder.value("decodedGrid").toArray() == upright, qPrintable(QString("%1: %2 decoded as %3").arg(label, codec, describe(encoder.value("decodedGrid")))));
            }
            const QJsonObject quality = result.value("qualityBitrate").toObject();
            if (!quality.contains("unsupported")) {
                const double ratio = quality.value("steadyBytes").toDouble() / qMax(1.0, quality.value("steadyTargetBytes").toDouble());
                QVERIFY2(quality.value("error").toString().isEmpty() && quality.value("chunks").toInt() == 90, qPrintable(QString("%1: quality mode H.264 encoding produced %2").arg(label, describe(quality))));
                QVERIFY2(ratio >= 0.2 && ratio <= 2.0, qPrintable(QString("%1: quality mode H.264 asked for 200 kbps and settled at %2 times that: %3").arg(label).arg(ratio, 0, 'f', 2).arg(describe(quality))));
            }
            const QStringList canvasFormats{"BGRA", "BGRX", "RGBA", "RGBX"};
            QVERIFY2(canvasFormats.contains(result.value("opaqueFormat").toString()) && result.value("opaqueCopy").toString() == "ok", qPrintable(QString("%1: a WebGL canvas frame has format %2 and copyTo() gave %3").arg(label, result.value("opaqueFormat").toString(), result.value("opaqueCopy").toString())));
            const QJsonObject alphaFormats = result.value("alphaFormats").toObject();
            for (const char *name : {"premultiplied", "straight"}) {
                QVERIFY2(QStringList({"BGRA", "RGBA"}).contains(alphaFormats.value(name).toString()), qPrintable(QString("%1: a translucent %2 WebGL canvas frame has format %3").arg(label, name, alphaFormats.value(name).toString())));
            }
            const QJsonObject alpha = result.value("alpha").toObject();
            for (const char *name : {"premultiplied", "straight"}) {
                const QJsonArray pixel = alpha.value(name).toArray();
                QVERIFY2(pixel.size() == 4 && pixel[0].toInt() >= 245 && pixel[1].toInt() <= 8 && pixel[2].toInt() <= 8 && qAbs(pixel[3].toInt() - 128) <= 3, qPrintable(QString("%1: a half transparent red %2 canvas copied as %3 instead of straight 255,0,0,128").arg(label, name, describe(pixel))));
            }
        }

        const QRegularExpression wrapped("Wrapped DMA-BUF pool buffer (\\d+) as a (\\d+)x(\\d+) (\\w+) video frame with modifier 0x([0-9a-f]+)");
        QHash<QString, QSet<QString>> buffersBySize;
        int wrappedFrames = 0;
        int tiledFrames = 0;
        for (auto match = wrapped.globalMatch(log); match.hasNext();) {
            const auto frame = match.next();
            ++wrappedFrames;
            tiledFrames += frame.captured(5) != "0";
            buffersBySize[frame.captured(2) + "x" + frame.captured(3)].insert(frame.captured(1));
        }
        const auto excerpt = log.right(6000);
        QVERIFY2(wrappedFrames >= 200, qPrintable(QString("Only %1 video frames wrapped a DMA-BUF\n%2").arg(wrappedFrames).arg(excerpt)));
        QVERIFY2(buffersBySize.contains("1280x720") && buffersBySize.contains("640x360"), qPrintable(QStringList(buffersBySize.keys()).join(", ")));
        for (auto it = buffersBySize.cbegin(); it != buffersBySize.cend(); ++it) {
            QVERIFY2(it.value().size() <= 12, qPrintable(QString("%1 frames used %2 distinct pool buffers across both WebGL modes").arg(it.key()).arg(it.value().size())));
        }
        QVERIFY2(!log.contains("through the CPU"), qPrintable(excerpt));
        QVERIFY2(!log.contains("on the GPU failed") && !log.contains("Unable to build a GPU pipeline"), qPrintable(excerpt));
        QVERIFY2(QRegularExpression("Built a GPU pipeline converting video/x-raw\\(memory:DMABuf\\)[^\\n]* to video/x-raw, format=\\(string\\)I420").match(log).hasMatch(), qPrintable(excerpt));
        if (hideVideoAcceleration) {
            QVERIFY2(!tiledFrames, qPrintable(QString("%1 frames used a tiled layout without video acceleration").arg(tiledFrames)));
            QVERIFY2(!log.contains("through the video post-processor"), qPrintable(excerpt));
        } else {
            QVERIFY2(tiledFrames == wrappedFrames, qPrintable(QString("Only %1 of %2 frames used the layout the video post-processor imports").arg(tiledFrames).arg(wrappedFrames)));
            QVERIFY2(log.contains("Feeding vah264enc through the video post-processor"), qPrintable(excerpt));
            if (log.contains("The H.264 encoder is")) {
                QVERIFY2(log.contains("The H.264 encoder is hardware accelerated and takes DMA-BUF frames"), qPrintable(excerpt));
            }
        }
    }

    void videoEffectsPipelinesWork() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(900, 600);
        view.attach(window.contentItem());
        window.show();
        auto *settings = webkit_web_view_get_settings(view.nativeView());
        webkit_settings_set_enable_mock_capture_devices(settings, TRUE);
        webkit_settings_set_media_playback_requires_user_gesture(settings, FALSE);
        QObject::connect(
            &view,
            &EngineView::permissionRequested,
            &view,
            [&view](const PermissionRequestInfo &info) { view.resolvePermissionRequest(info.id, true); }
        );
        view.load(url("video-effects"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("video-effects"), 15000);
        evaluate(view, R"JS(window.effectsResult = null;
(async () => {
    const result = {};
    const attempt = async (name, work) => {
        window.effectsStage = name;
        try {
            result[name] = await Promise.race([work(), new Promise(resolve => setTimeout(() => resolve({stalled: true}), 12000))]);
        } catch (error) {
            result[name] = {error: String(error && error.message || error), name: error && error.name};
        }
    };
    const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
    await attempt('globals', async () => ({
        MediaStreamTrackProcessor: typeof MediaStreamTrackProcessor,
        MediaStreamTrackGenerator: typeof MediaStreamTrackGenerator,
        VideoTrackGenerator: typeof VideoTrackGenerator,
        VideoFrame: typeof VideoFrame,
        VideoEncoder: typeof VideoEncoder,
        OffscreenCanvas: typeof OffscreenCanvas,
        gpu: typeof navigator.gpu,
        crossOriginIsolated: self.crossOriginIsolated,
        SharedArrayBuffer: typeof SharedArrayBuffer,
        RTCRtpScriptTransform: typeof RTCRtpScriptTransform,
        requestVideoFrameCallback: typeof HTMLVideoElement.prototype.requestVideoFrameCallback,
        captureStream: typeof HTMLCanvasElement.prototype.captureStream,
    }));
    await attempt('wasm', async () => {
        const simd = WebAssembly.validate(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 1, 5, 1, 96, 0, 1, 123, 3, 2, 1, 0, 10, 10, 1, 8, 0, 65, 0, 253, 15, 253, 98, 11]));
        let threads = false;
        try {
            threads = new WebAssembly.Memory({initial: 1, maximum: 2, shared: true}).buffer instanceof SharedArrayBuffer;
        } catch (error) {
            threads = String(error);
        }
        return {simd, threads};
    });
    await attempt('webgl2', async () => {
        const canvas = document.createElement('canvas');
        const gl = canvas.getContext('webgl2');
        if (!gl)
            return {context: false};
        const debug = gl.getExtension('WEBGL_debug_renderer_info');
        const wanted = ['EXT_color_buffer_float', 'EXT_color_buffer_half_float', 'OES_texture_float_linear', 'EXT_float_blend', 'OES_texture_half_float_linear', 'WEBGL_multi_draw', 'EXT_disjoint_timer_query_webgl2', 'KHR_parallel_shader_compile', 'WEBGL_lose_context'];
        const supported = gl.getSupportedExtensions() || [];
        const texture = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, texture);
        gl.getExtension('EXT_color_buffer_float');
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA16F, 64, 64, 0, gl.RGBA, gl.HALF_FLOAT, null);
        const framebuffer = gl.createFramebuffer();
        gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, texture, 0);
        const halfFloatTarget = gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE;
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, 64, 64, 0, gl.RGBA, gl.FLOAT, null);
        const floatTarget = gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE;
        return {
            renderer: debug ? gl.getParameter(debug.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER),
            missing: wanted.filter(name => !supported.includes(name)),
            maxTexture: gl.getParameter(gl.MAX_TEXTURE_SIZE),
            halfFloatTarget,
            floatTarget,
        };
    });
    await attempt('webgpu', async () => {
        if (!navigator.gpu)
            return {available: false};
        const adapter = await navigator.gpu.requestAdapter();
        if (!adapter)
            return {available: true, adapter: null};
        const device = await adapter.requestDevice();
        return {available: true, adapter: true, device: !!device, features: Array.from(adapter.features || [])};
    });
    const camera = await navigator.mediaDevices.getUserMedia({video: {width: 640, height: 480, frameRate: 30}});
    const cameraTrack = camera.getVideoTracks()[0];
    await attempt('videoElementToWebGL', async () => {
        const video = document.createElement('video');
        video.muted = true;
        video.playsInline = true;
        video.srcObject = new MediaStream([cameraTrack.clone()]);
        await video.play();
        await new Promise(resolve => video.requestVideoFrameCallback(() => resolve()));
        const canvas = document.createElement('canvas');
        canvas.width = 320;
        canvas.height = 240;
        const gl = canvas.getContext('webgl2');
        const texture = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, texture);
        const start = performance.now();
        for (let index = 0; index < 20; index++)
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, video);
        const error = gl.getError();
        const perUpload = (performance.now() - start) / 20;
        video.srcObject.getTracks().forEach(track => track.stop());
        return {glError: error, perUploadMs: Math.round(perUpload * 10) / 10};
    });
    await attempt('canvasCaptureStream', async () => {
        const canvas = document.createElement('canvas');
        canvas.width = 320;
        canvas.height = 240;
        const context = canvas.getContext('2d');
        const stream = canvas.captureStream(30);
        const video = document.createElement('video');
        video.muted = true;
        video.srcObject = stream;
        video.play().catch(() => {});
        let frames = 0;
        const count = () => { frames++; video.requestVideoFrameCallback(count); };
        video.requestVideoFrameCallback(count);
        const end = performance.now() + 1500;
        while (performance.now() < end) {
            context.fillStyle = `hsl(${Math.random() * 360},80%,50%)`;
            context.fillRect(0, 0, 320, 240);
            await delay(33);
        }
        stream.getTracks().forEach(track => track.stop());
        return {framesPresented: frames};
    });
    await attempt('webglCanvasFrames', async () => {
        const canvas = document.createElement('canvas');
        canvas.width = 320;
        canvas.height = 240;
        canvas.style.cssText = 'width:160px;height:120px';
        document.body.appendChild(canvas);
        const gl = canvas.getContext('webgl2');
        const paint = () => {
            gl.clearColor(0.8, 0.2, 0.4, 1);
            gl.clear(gl.COLOR_BUFFER_BIT);
        };
        paint();
        const stream = canvas.captureStream(30);
        const video = document.createElement('video');
        video.muted = true;
        video.srcObject = stream;
        document.body.appendChild(video);
        video.play().catch(() => {});
        let presented = 0;
        const count = () => { presented++; video.requestVideoFrameCallback(count); };
        video.requestVideoFrameCallback(count);
        const end = performance.now() + 2000;
        while (performance.now() < end) {
            paint();
            await delay(33);
        }
        const probe = document.createElement('canvas');
        probe.width = 320;
        probe.height = 240;
        const probeContext = probe.getContext('2d', {willReadFrequently: true});
        probeContext.drawImage(video, 0, 0, 320, 240);
        const capturedPixel = Array.from(probeContext.getImageData(160, 120, 1, 1).data);
        paint();
        let framePixel = null;
        let frameError = null;
        try {
            const frame = new VideoFrame(canvas, {timestamp: 0});
            const pixels = new Uint8Array(frame.allocationSize({format: 'RGBA'}));
            const layout = await frame.copyTo(pixels, {format: 'RGBA'});
            const offset = layout[0].offset + 120 * layout[0].stride + 160 * 4;
            framePixel = Array.from(pixels.slice(offset, offset + 4));
            frame.close();
        } catch (error) {
            frameError = String(error && error.message || error);
        }
        stream.getTracks().forEach(track => track.stop());
        video.remove();
        canvas.remove();
        return {presented, capturedPixel, framePixel, frameError, contextLost: gl.isContextLost()};
    });
    await attempt('workerPipeline', async () => {
        const source = `
            self.onmessage = async event => {
                const report = {};
                try {
                    report.hasProcessor = typeof MediaStreamTrackProcessor;
                    report.hasGenerator = typeof VideoTrackGenerator;
                    report.hasOffscreen = typeof OffscreenCanvas;
                    const processor = new MediaStreamTrackProcessor({track: event.data.track});
                    const reader = processor.readable.getReader();
                    const generator = typeof VideoTrackGenerator !== 'undefined' ? new VideoTrackGenerator() : null;
                    if (generator)
                        self.postMessage({generatorTrack: generator.track}, [generator.track]);
                    const writer = generator ? generator.writable.getWriter() : null;
                    const canvas = new OffscreenCanvas(320, 240);
                    const gl = canvas.getContext('webgl2');
                    report.offscreenWebGL2 = !!gl;
                    const texture = gl && gl.createTexture();
                    let frames = 0;
                    const deadline = performance.now() + 2500;
                    while (performance.now() < deadline) {
                        const {value: frame, done} = await reader.read();
                        if (done)
                            break;
                        if (!frames) {
                            report.firstFrame = {format: frame.format, codedWidth: frame.codedWidth, codedHeight: frame.codedHeight, displayWidth: frame.displayWidth, displayHeight: frame.displayHeight, timestamp: frame.timestamp, colorSpace: frame.colorSpace && frame.colorSpace.toJSON ? frame.colorSpace.toJSON() : null};
                            try {
                                const size = frame.allocationSize();
                                const buffer = new Uint8Array(size);
                                const layout = await frame.copyTo(buffer);
                                report.copyTo = {size, planes: layout.length};
                            } catch (error) { report.copyTo = String(error); }
                            try {
                                const size = frame.allocationSize({format: 'RGBA'});
                                const buffer = new Uint8Array(size);
                                await frame.copyTo(buffer, {format: 'RGBA'});
                                report.copyToRGBA = size;
                            } catch (error) { report.copyToRGBA = String(error); }
                            try {
                                const bitmap = await createImageBitmap(frame);
                                report.imageBitmap = [bitmap.width, bitmap.height];
                                bitmap.close();
                            } catch (error) { report.imageBitmap = String(error); }
                        }
                        if (gl) {
                            gl.bindTexture(gl.TEXTURE_2D, texture);
                            try {
                                gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, frame);
                                const error = gl.getError();
                                if (error)
                                    report.texImageError = error;
                            } catch (error) { report.texImageException = String(error); }
                            gl.clearColor(0.2, 0.4, 0.6, 1);
                            gl.clear(gl.COLOR_BUFFER_BIT);
                        }
                        if (writer) {
                            let output;
                            try {
                                output = new VideoFrame(canvas, {timestamp: frame.timestamp});
                            } catch (error) { report.newVideoFrame = String(error); }
                            if (output) {
                                try { await writer.write(output); } catch (error) { report.write = String(error); }
                            }
                        }
                        frame.close();
                        frames++;
                    }
                    report.frames = frames;
                    reader.cancel();
                } catch (error) {
                    report.error = String(error && error.message || error);
                }
                self.postMessage({report});
            };
        `;
        const worker = new Worker(URL.createObjectURL(new Blob([source], {type: 'text/javascript'})));
        const track = cameraTrack.clone();
        let generatedFrames = null;
        let generatedVideo = null;
        const report = await new Promise((resolve, reject) => {
            const timer = setTimeout(() => resolve({timeout: true}), 8000);
            worker.onerror = event => { clearTimeout(timer); resolve({workerError: event.message}); };
            worker.onmessage = async event => {
                if (event.data.generatorTrack) {
                    const video = document.createElement('video');
                    generatedVideo = video;
                    video.muted = true;
                    video.srcObject = new MediaStream([event.data.generatorTrack]);
                    video.play().catch(() => {});
                    generatedFrames = 0;
                    const count = () => { generatedFrames++; video.requestVideoFrameCallback(count); };
                    video.requestVideoFrameCallback(count);
                    return;
                }
                clearTimeout(timer);
                resolve(event.data.report);
            };
            try {
                worker.postMessage({track}, [track]);
            } catch (error) {
                clearTimeout(timer);
                resolve({transferError: String(error)});
            }
        });
        report.generatedFramesPresented = generatedFrames;
        if (generatedVideo && generatedVideo.videoWidth) {
            const probe = document.createElement('canvas');
            probe.width = generatedVideo.videoWidth;
            probe.height = generatedVideo.videoHeight;
            const context = probe.getContext('2d');
            context.drawImage(generatedVideo, 0, 0);
            report.generatedPixel = Array.from(context.getImageData(probe.width / 2, probe.height / 2, 1, 1).data);
            report.generatedSize = [generatedVideo.videoWidth, generatedVideo.videoHeight];
        }
        worker.terminate();
        return report;
    });
    await attempt('mainThreadProcessor', async () => {
        if (typeof MediaStreamTrackProcessor === 'undefined')
            return 'absent';
        const processor = new MediaStreamTrackProcessor({track: cameraTrack.clone()});
        const reader = processor.readable.getReader();
        const {value} = await reader.read();
        const info = {format: value.format, codedWidth: value.codedWidth};
        value.close();
        reader.cancel();
        return info;
    });
    camera.getTracks().forEach(track => track.stop());
    window.effectsResult = result;
})().catch(error => window.effectsResult = {fatal: String(error)});
)JS");
        QElapsedTimer waited;
        waited.start();
        while (!evaluate(view, "!!window.effectsResult").toBool() && waited.elapsed() < 150000) {
            QTest::qWait(200);
        }
        const QJsonObject result = QJsonDocument::fromJson(evaluate(view, "JSON.stringify(window.effectsResult || {stage: window.effectsStage})").toString().toUtf8()).object();
        qInfo().noquote() << "Video effects" << QJsonDocument(result).toJson(QJsonDocument::Compact);
        QVERIFY2(!result.contains("stage") && !result.contains("fatal"), qPrintable(QString("The effects fixture stopped at %1").arg(result.value("stage").toString())));
        const QJsonObject globals = result.value("globals").toObject();
        for (const char *name : {"VideoFrame", "OffscreenCanvas", "RTCRtpScriptTransform", "requestVideoFrameCallback", "captureStream"}) {
            QCOMPARE(globals.value(name).toString(), QString("function"));
        }
        const QJsonObject webgl = result.value("webgl2").toObject();
        QVERIFY(webgl.value("halfFloatTarget").toBool());
        QVERIFY(webgl.value("floatTarget").toBool());
        QCOMPARE(result.value("videoElementToWebGL").toObject().value("glError").toInt(-1), 0);
        QVERIFY2(result.value("canvasCaptureStream").toObject().value("framesPresented").toInt() >= 20, "canvas.captureStream() presented too few frames");
        const QJsonObject webglFrames = result.value("webglCanvasFrames").toObject();
        const auto closeTo = [](const QJsonArray &pixel, int red, int green, int blue) {
            return pixel.size() == 4 && std::abs(pixel[0].toInt() - red) <= 8 && std::abs(pixel[1].toInt() - green) <= 8 && std::abs(pixel[2].toInt() - blue) <= 8;
        };
        QVERIFY2(!webglFrames.value("contextLost").toBool(true), qPrintable(QString("Capturing a WebGL canvas lost the context: %1").arg(QString::fromUtf8(QJsonDocument(webglFrames).toJson(QJsonDocument::Compact)))));
        QVERIFY2(webglFrames.value("presented").toInt() >= 20, qPrintable(QString("A captured WebGL canvas presented %1 frames").arg(webglFrames.value("presented").toInt())));
        QVERIFY2(closeTo(webglFrames.value("capturedPixel").toArray(), 204, 51, 102), qPrintable(QString("A captured WebGL canvas showed %1").arg(QString::fromUtf8(QJsonDocument(webglFrames.value("capturedPixel").toArray()).toJson(QJsonDocument::Compact)))));
        QVERIFY2(webglFrames.value("frameError").isNull() && closeTo(webglFrames.value("framePixel").toArray(), 204, 51, 102),
            qPrintable(QString("new VideoFrame(webglCanvas) gave %1 %2").arg(webglFrames.value("frameError").toString(), QString::fromUtf8(QJsonDocument(webglFrames.value("framePixel").toArray()).toJson(QJsonDocument::Compact)))));
        const QJsonObject worker = result.value("workerPipeline").toObject();
        QCOMPARE(worker.value("hasProcessor").toString(), QString("function"));
        QCOMPARE(worker.value("hasGenerator").toString(), QString("function"));
        QVERIFY2(worker.value("offscreenWebGL2").toBool(), "A worker could not create a WebGL2 context on an OffscreenCanvas");
        for (const char *failure : {"error", "workerError", "transferError", "texImageError", "texImageException", "newVideoFrame", "write", "timeout"}) {
            QVERIFY2(!worker.contains(failure), qPrintable(QString("Worker pipeline %1: %2").arg(failure, worker.value(failure).toVariant().toString())));
        }
        QVERIFY2(worker.value("frames").toInt() >= 30, qPrintable(QString("The worker read %1 camera frames").arg(worker.value("frames").toInt())));
        QVERIFY(worker.value("copyTo").toObject().value("planes").toInt() >= 1);
        QVERIFY(worker.value("copyToRGBA").toInt() == 640 * 480 * 4);
        QCOMPARE(worker.value("imageBitmap").toArray(), QJsonArray({640, 480}));
        QVERIFY2(worker.value("generatedFramesPresented").toInt() >= 20, qPrintable(QString("The page presented %1 generated frames").arg(worker.value("generatedFramesPresented").toInt())));
        QCOMPARE(worker.value("generatedSize").toArray(), QJsonArray({320, 240}));
        const QJsonArray pixel = worker.value("generatedPixel").toArray();
        QVERIFY2(pixel.size() == 4 && std::abs(pixel[0].toInt() - 51) <= 6 && std::abs(pixel[1].toInt() - 102) <= 6 && std::abs(pixel[2].toInt() - 153) <= 6,
            qPrintable(QString("Generated frames showed %1 instead of the WebGL clear color").arg(QString::fromUtf8(QJsonDocument(pixel).toJson(QJsonDocument::Compact)))));
    }

    void joiningWithRealDevicesIsQuick() {
        if (qEnvironmentVariable("EDEN_TEST_REAL_CAPTURE") != "1") {
            QSKIP("Requires a real camera and microphone");
        }
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(900, 600);
        view.attach(window.contentItem());
        window.show();
        webkit_settings_set_media_playback_requires_user_gesture(webkit_web_view_get_settings(view.nativeView()), FALSE);
        QObject::connect(
            &view,
            &EngineView::permissionRequested,
            &view,
            [&view](const PermissionRequestInfo &info) { view.resolvePermissionRequest(info.id, true); }
        );
        view.load(url("join-latency"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("join-latency"), 15000);
        evaluate(view, R"JS(window.joinResult = null;
(async () => {
    let worstLag = 0;
    let lastTick = performance.now();
    const ticker = setInterval(() => {
        const now = performance.now();
        worstLag = Math.max(worstLag, now - lastTick - 10);
        lastTick = now;
    }, 10);
    const timed = async (name, work, marks) => {
        const lagBefore = worstLag;
        worstLag = 0;
        const start = performance.now();
        const value = await work();
        marks[name] = {ms: Math.round(performance.now() - start), worstLag: Math.round(worstLag)};
        worstLag = Math.max(worstLag, lagBefore);
        return value;
    };
    const marks = {};
    const kinds = list => list.map(device => device.kind).sort().join(',');
    const coldLists = await timed('enumerateColdMs', () => Promise.all([0, 1, 2].map(() => navigator.mediaDevices.enumerateDevices())), marks);
    marks.coldListsAgree = coldLists.every(list => kinds(list) === kinds(coldLists[0]));
    marks.coldKinds = kinds(coldLists[0]);
    const audio = await timed('audioGetUserMedia', () => navigator.mediaDevices.getUserMedia({audio: true}), marks);
    const video = await timed('videoGetUserMedia', () => navigator.mediaDevices.getUserMedia({video: {width: {ideal: 1280}, height: {ideal: 720}, frameRate: {ideal: 30}}}), marks);
    const warmList = await timed('enumerateWarm', () => navigator.mediaDevices.enumerateDevices(), marks);
    marks.warmKinds = kinds(warmList);
    marks.warmDuplicates = warmList.length - new Set(warmList.map(device => device.kind + '/' + device.deviceId)).size;
    const element = document.createElement('video');
    element.muted = true;
    element.playsInline = true;
    element.srcObject = video;
    document.body.append(element);
    await timed('firstFrame', () => new Promise(resolve => {
        element.play();
        element.requestVideoFrameCallback(() => resolve());
    }), marks);
    const both = await timed('combinedGetUserMedia', () => navigator.mediaDevices.getUserMedia({audio: true, video: true}), marks);
    const connection = await timed('peerConnection', async () => {
        const sender = new RTCPeerConnection();
        const receiver = new RTCPeerConnection();
        sender.onicecandidate = event => event.candidate && receiver.addIceCandidate(event.candidate);
        receiver.onicecandidate = event => event.candidate && sender.addIceCandidate(event.candidate);
        sender.addTransceiver(video.getVideoTracks()[0], {direction: 'sendonly', sendEncodings: [{rid: 'q', scaleResolutionDownBy: 4}, {rid: 'h', scaleResolutionDownBy: 2}, {rid: 'f'}]});
        sender.addTrack(audio.getAudioTracks()[0]);
        const offer = await sender.createOffer();
        await sender.setLocalDescription(offer);
        await receiver.setRemoteDescription(offer);
        const answer = await receiver.createAnswer();
        await receiver.setLocalDescription(answer);
        await sender.setRemoteDescription(answer);
        for (let i = 0; i < 400 && sender.connectionState !== 'connected'; i++)
            await new Promise(r => setTimeout(r, 10));
        return {sender, receiver, state: sender.connectionState};
    }, marks);
    marks.connectionState = connection.state;
    await new Promise(r => setTimeout(r, 3000));
    marks.steadyWorstLag = Math.round(worstLag);
    clearInterval(ticker);
    connection.sender.close();
    connection.receiver.close();
    [audio, video, both].forEach(stream => stream.getTracks().forEach(track => track.stop()));
    window.joinResult = marks;
})().catch(error => window.joinResult = {error: String(error)});
)JS");
        QTRY_VERIFY_WITH_TIMEOUT(evaluate(view, "!!window.joinResult").toBool(), 60000);
        const QJsonObject result = QJsonDocument::fromJson(evaluate(view, "JSON.stringify(window.joinResult)").toString().toUtf8()).object();
        qInfo().noquote() << "Join latency" << QJsonDocument(result).toJson(QJsonDocument::Compact);
        QCOMPARE(result.value("error").toString(), QString());
        QCOMPARE(result.value("connectionState").toString(), QString("connected"));
        QVERIFY2(result.value("coldListsAgree").toBool(), "Concurrent first enumerations disagreed about the devices");
        QVERIFY2(result.value("coldKinds").toString().contains("audioinput") && result.value("coldKinds").toString().contains("videoinput"), qPrintable(QString("The first enumeration listed %1").arg(result.value("coldKinds").toString())));
        QVERIFY2(result.value("warmKinds").toString().contains("audioinput") && result.value("warmKinds").toString().contains("videoinput"), qPrintable(QString("Enumeration after capture listed %1").arg(result.value("warmKinds").toString())));
        QCOMPARE(result.value("warmDuplicates").toInt(-1), 0);
        const QList<QPair<QString, int>> budgets = {
            {"enumerateColdMs", 400}, {"audioGetUserMedia", 300}, {"videoGetUserMedia", 600}, {"enumerateWarm", 100},
            {"firstFrame", 300}, {"combinedGetUserMedia", 400}, {"peerConnection", 400},
        };
        for (const auto &[step, budget] : budgets) {
            const QJsonObject mark = result.value(step).toObject();
            QVERIFY2(mark.value("ms").toInt() <= budget, qPrintable(QString("%1 took %2 ms against a %3 ms budget").arg(step).arg(mark.value("ms").toInt()).arg(budget)));
            QVERIFY2(mark.value("worstLag").toInt() <= 50, qPrintable(QString("%1 stalled the main thread for %2 ms").arg(step).arg(mark.value("worstLag").toInt())));
        }
        QVERIFY2(result.value("steadyWorstLag").toInt() <= 50, qPrintable(QString("The main thread stalled for %1 ms during the call").arg(result.value("steadyWorstLag").toInt())));
    }

    void cameraSelfViewWithRealDeviceKeepsCaptureRate() {
        if (qEnvironmentVariable("EDEN_TEST_REAL_CAPTURE") != "1") {
            QSKIP("Requires a real camera");
        }
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(900, 600);
        view.attach(window.contentItem());
        window.show();
        webkit_settings_set_media_playback_requires_user_gesture(webkit_web_view_get_settings(view.nativeView()), FALSE);
        QObject::connect(
            &view,
            &EngineView::permissionRequested,
            &view,
            [&view](const PermissionRequestInfo &info) { view.resolvePermissionRequest(info.id, true); }
        );
        view.load(url("camera-self-view"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("camera-self-view"), 15000);
        evaluate(view, R"JS(
window.selfView = null;
(async () => {
    const stream = await navigator.mediaDevices.getUserMedia({video: {width: {ideal: 1280}, height: {ideal: 720}, frameRate: {ideal: 30}}});
    const video = document.createElement('video');
    video.muted = true;
    video.autoplay = true;
    video.playsInline = true;
    video.style.cssText = 'width:640px;height:360px';
    video.srcObject = stream;
    document.body.append(video);
    await video.play();
    let presented = 0;
    const count = () => {
        presented++;
        video.requestVideoFrameCallback(count);
    };
    video.requestVideoFrameCallback(count);
    window.selfView = {stream, presented: () => presented, frameRate: () => stream.getVideoTracks()[0].getSettings().frameRate};
})().catch(error => window.selfView = {error: String(error)});
)JS");
        QTRY_VERIFY_WITH_TIMEOUT(evaluate(view, "!!window.selfView").toBool(), 20000);
        QCOMPARE(evaluate(view, "String(window.selfView.error || '')").toString(), QString());
        QTest::qWait(3000);
        const int before = evaluate(view, "window.selfView.presented()").toInt();
        QTest::qWait(10000);
        const double presentedFps = (evaluate(view, "window.selfView.presented()").toInt() - before) / 10.0;
        const double captureFps = evaluate(view, "window.selfView.frameRate()").toDouble();
        qInfo().noquote() << QString("Real camera self view: %1 fps presented, capture set to %2 fps").arg(presentedFps).arg(captureFps);
        evaluate(view, "window.selfView.stream.getTracks().forEach(track => track.stop())");
        QVERIFY2(presentedFps >= captureFps * 0.8, qPrintable(QString("The self view presented %1 of %2 fps").arg(presentedFps).arg(captureFps)));
    }

    void cameraCallWithRealDevicesStaysSmooth() {
        if (qEnvironmentVariable("EDEN_TEST_REAL_CAPTURE") != "1") {
            QSKIP("Requires a real camera and microphone");
        }
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(900, 600);
        view.attach(window.contentItem());
        window.show();
        webkit_settings_set_media_playback_requires_user_gesture(webkit_web_view_get_settings(view.nativeView()), FALSE);
        QObject::connect(
            &view,
            &EngineView::permissionRequested,
            &view,
            [&view](const PermissionRequestInfo &info) { view.resolvePermissionRequest(info.id, true); }
        );
        bool terminated = false;
        g_signal_connect(
            view.nativeView(),
            "web-process-terminated",
            G_CALLBACK(+[](WebKitWebView *, WebKitWebProcessTerminationReason, gpointer flag) {
                *static_cast<bool *>(flag) = true;
            }),
            &terminated
        );
        view.load(url("camera-call"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("camera-call"), 15000);
        evaluate(view, cameraCallScript());
        QTRY_VERIFY_WITH_TIMEOUT(!terminated && evaluate(view, "!!(window.cameraCall && (window.cameraCall.ready || window.cameraCall.error))").toBool(), 30000);
        QCOMPARE(evaluate(view, "String(window.cameraCall.error || '')").toString(), QString());
        const auto sample = [&]() -> QJsonObject {
            evaluate(view, "window.cameraSample = null; window.cameraCall.sample().then(value => window.cameraSample = value)");
            QElapsedTimer waited;
            waited.start();
            while (!terminated && !evaluate(view, "!!window.cameraSample").toBool() && waited.elapsed() < 5000) {
                QTest::qWait(50);
            }
            return QJsonDocument::fromJson(evaluate(view, "JSON.stringify(window.cameraSample || {})").toString().toUtf8())
                .object();
        };
        QTest::qWait(4000);
        QJsonObject previous = sample();
        QVERIFY(!previous.isEmpty());
        std::array<double, 4> slowest = {1000, 1000, 1000, 1000};
        double slowestSource = 1000;
        double worstTimerLag = 0;
        for (int round = 0; round < 15; ++round) {
            QTest::qWait(2000);
            QVERIFY2(!terminated, "The web process terminated");
            const QJsonObject current = sample();
            QVERIFY(!current.isEmpty());
            const double selfFps = (current.value("selfTime").toDouble() - previous.value("selfTime").toDouble()) > 0
                                       ? (current.value("selfFrames").toInt() - previous.value("selfFrames").toInt()) / 2.0
                                       : 0;
            const double remoteFps = (current.value("remoteFrames").toInt() - previous.value("remoteFrames").toInt()) / 2.0;
            const double encodedFps = (current.value("encodedFrames").toInt() - previous.value("encodedFrames").toInt()) / 2.0;
            const double directFps = (current.value("directEncoded").toInt() - previous.value("directEncoded").toInt()) / 2.0;
            const double sourceFps = (current.value("sourceFrames").toInt() - previous.value("sourceFrames").toInt()) / 2.0;
            const double timerLag = current.value("timerLag").toDouble();
            qInfo().noquote() << "Real camera call sample" << QJsonDocument(current).toJson(QJsonDocument::Compact);
            qInfo().noquote() << QString("Real camera call %1: source %2 fps, self %3 fps, remote %4 fps, direct encoded %5 fps, simulcast top layer %6 fps, timer lag %7 ms")
                                     .arg(round)
                                     .arg(sourceFps)
                                     .arg(selfFps)
                                     .arg(remoteFps)
                                     .arg(directFps)
                                     .arg(encodedFps)
                                     .arg(timerLag, 0, 'f', 1);
            slowest = {std::min(slowest[0], selfFps), std::min(slowest[1], remoteFps), std::min(slowest[2], directFps), std::min(slowest[3], encodedFps)};
            slowestSource = std::min(slowestSource, sourceFps);
            worstTimerLag = std::max(worstTimerLag, timerLag);
            previous = current;
        }
        QVERIFY2(slowestSource >= 20, qPrintable(QString("The camera delivered %1 fps to the sender").arg(slowestSource)));
        QVERIFY2(slowest[0] >= 0.8 * slowestSource, qPrintable(QString("The self view ran at %1 fps").arg(slowest[0])));
        QVERIFY2(slowest[1] >= 0.8 * slowestSource, qPrintable(QString("Remote video decoded at %1 fps").arg(slowest[1])));
        QVERIFY2(slowest[2] >= 0.8 * slowestSource, qPrintable(QString("The single-layer sender encoded at %1 fps").arg(slowest[2])));
        QVERIFY2(slowest[3] >= 20, qPrintable(QString("The simulcast top layer encoded at %1 fps").arg(slowest[3])));
        QVERIFY2(worstTimerLag < 100, qPrintable(QString("Main thread timers ran %1 ms late").arg(worstTimerLag)));
        evaluate(view, "window.cameraCall.stream.getTracks().forEach(track => track.stop())");
        QTest::qWait(1000);
        QVERIFY(!terminated);
    }

    void contextMenuTargetsTheClickedFormField() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(700, 500);
        view.attach(window.contentItem());
        window.show();
        view.load(url("input"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("Input"), 15000);
        evaluate(
            view,
            "document.body.innerHTML='<input id=username autocomplete=username><input id=contact type=email "
            "autocomplete=email>'"
        );
        QSignalSpy fields(&view, &EngineView::formFieldFocused);
        QSignalSpy menus(&view, &EngineView::contextMenuRequested);
        QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(50, 20));
        QTRY_VERIFY_WITH_TIMEOUT(!fields.isEmpty(), 5000);
        QTest::mouseClick(&window, Qt::RightButton, {}, QPoint(50, 70));
        QTRY_VERIFY_WITH_TIMEOUT(!menus.isEmpty(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fields.last().first().toMap().value("name").toString(), QString("contact"), 5000);
        QTest::qWait(100);
        QCOMPARE(fields.last().first().toMap().value("name").toString(), QString("contact"));
        QCOMPARE(evaluate(view, "document.activeElement.id").toString(), QString("contact"));
        AutofillTarget target;
        view.requestAutofillTarget([&](AutofillTarget value) { target = std::move(value); });
        QTRY_VERIFY(target.isValid());
        evaluate(
            view,
            "document.body.insertAdjacentHTML('afterbegin','<input id=hidden-email type=hidden value=secret>"
            "<input id=disabled-email disabled value=disabled><input id=readonly-email readonly value=readonly>');"
        );
        view.fillForm(target, {{"email", "person@example.test"}});
        QTRY_COMPARE_WITH_TIMEOUT(
            evaluate(view, "document.querySelector('#contact').value").toString(),
            QString("person@example.test"),
            5000
        );
        QCOMPARE(evaluate(view, "document.getElementById('hidden-email').value").toString(), QString("secret"));
        QCOMPARE(evaluate(view, "document.getElementById('disabled-email').value").toString(), QString("disabled"));
        QCOMPARE(evaluate(view, "document.getElementById('readonly-email').value").toString(), QString("readonly"));
    }

    void navigationHistoryAndMute() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        view.load(url("first"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("first"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
        QCOMPARE(view.backendName(), QString("WebKit"));
        QVERIFY(!view.canGoBack());
        view.setMuted(true);
        QTRY_VERIFY(view.isMuted());
        view.load(url("second"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("second"), 15000);
        QTRY_VERIFY(!view.isLoading());
        QVERIFY(view.canGoBack());
        QCOMPARE(view.navigationHistory(-1).first().toMap().value("url").toUrl(), url("first"));
        QCOMPARE(view.navigationHistory(-1).first().toMap().value("id").toInt(), -1);
        QCOMPARE(view.navigationHistory(-1).first().toMap().value("subtitle").toString(), url("first").toString());
        view.back();
        QTRY_COMPARE_WITH_TIMEOUT(view.url(), url("first"), 15000);
        QTRY_VERIFY(!view.isLoading());
        QVERIFY(view.canGoForward());
        view.goToHistoryOffset(1);
        QTRY_COMPARE_WITH_TIMEOUT(view.url(), url("second"), 15000);
        QTRY_VERIFY(!view.isLoading());
        const QVariantMap state = view.serializeState();
        QVERIFY(!state.value("wpeSession").toString().isEmpty());
        WpeEngineView restored(&profile);
        restored.restoreState(state);
        QTRY_COMPARE_WITH_TIMEOUT(restored.url(), url("second"), 15000);
        QTRY_VERIFY(restored.canGoBack());
    }

    void subframeLoadingDoesNotChangePageProgress() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QSignalSpy progress(&view, &EngineView::loadProgressChanged);
        view.load(url("main-progress"));
        QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading() && view.title() == "main-progress", 15000);
        QVERIFY(!progress.isEmpty());
        QCOMPARE(view.loadProgress(), 100);
        progress.clear();
        QSignalSpy loading(&view, &EngineView::loadingChanged);
        evaluate(
            view,
            "globalThis.frameLoads=0;globalThis.testFrame=document.createElement('iframe');"
            "testFrame.onload=()=>++frameLoads;document.body.append(testFrame)"
        );
        QTRY_COMPARE_WITH_TIMEOUT(evaluate(view, "frameLoads").toInt(), 1, 10000);
        for (int index = 0; index < 4; ++index) {
            evaluate(view, QString("testFrame.src='/child-progress-%1'").arg(index));
            QTRY_COMPARE_WITH_TIMEOUT(evaluate(view, "frameLoads").toInt(), index + 2, 10000);
        }
        QVERIFY(loading.isEmpty());
        QVERIFY(progress.isEmpty());
        QCOMPARE(view.loadProgress(), 100);
        view.reload();
        QTRY_VERIFY_WITH_TIMEOUT(loading.size() >= 2 && !view.isLoading(), 15000);
        QVERIFY(!progress.isEmpty());
        QCOMPARE(view.loadProgress(), 100);
    }

    void frameAcknowledgementDoesNotWaitForSwap_data() {
        QTest::addColumn<QByteArray>("policy");
        QTest::addColumn<bool>("acknowledgesBeforeSwap");
        QTest::newRow("default") << QByteArray() << true;
        QTest::newRow("import") << QByteArray("import") << true;
        QTest::newRow("swap-control") << QByteArray("swap") << false;
    }

    void frameAcknowledgementDoesNotWaitForImport_data() {
        QTest::addColumn<QByteArray>("policy");
        QTest::addColumn<bool>("acknowledgesOnArrival");
        QTest::newRow("default") << QByteArray() << true;
        QTest::newRow("import-control") << QByteArray("import") << false;
    }

    void frameAcknowledgementDoesNotWaitForImport() {
        QFETCH(QByteArray, policy);
        QFETCH(bool, acknowledgesOnArrival);
        const bool hadPolicy = qEnvironmentVariableIsSet("EDEN_WPE_FRAME_ACK");
        const QByteArray previousPolicy = qgetenv("EDEN_WPE_FRAME_ACK");
        const auto restorePolicy = qScopeGuard([&] {
            if (hadPolicy) {
                qputenv("EDEN_WPE_FRAME_ACK", previousPolicy);
            } else {
                qunsetenv("EDEN_WPE_FRAME_ACK");
            }
        });
        if (policy.isEmpty()) {
            qunsetenv("EDEN_WPE_FRAME_ACK");
        } else {
            qputenv("EDEN_WPE_FRAME_ACK", policy);
        }
        WPEView *native = wpe_view_new(WpeRuntime::display());
        QVERIFY(native);
        const quint32 pixel = 0xff20c040;
        GBytes *bytes = g_bytes_new(&pixel, sizeof(pixel));
        WPEBuffer *buffer = WPE_BUFFER(
            wpe_buffer_shm_new(WpeRuntime::display(), 1, 1, WPE_PIXEL_FORMAT_ARGB8888, bytes, sizeof(pixel))
        );
        g_bytes_unref(bytes);
        QVERIFY(buffer);
        QList<QByteArray> lifecycle;
        g_signal_connect(
            native,
            "buffer-rendered",
            G_CALLBACK(+[](WPEView *, WPEBuffer *, QList<QByteArray> *events) { events->append("rendered"); }),
            &lifecycle
        );
        g_signal_connect(
            native,
            "buffer-released",
            G_CALLBACK(+[](WPEView *, WPEBuffer *, QList<QByteArray> *events) { events->append("released"); }),
            &lifecycle
        );
        const auto cleanup = qScopeGuard([&] {
            g_object_unref(buffer);
            g_object_unref(native);
        });
        {
            WpeSurface surface(native);
            wpe_view_set_visible(native, TRUE);
            wpe_view_map(native);
            QVERIFY(wpe_view_get_mapped(native));
            QVERIFY(surface.submit(buffer));
            if (acknowledgesOnArrival) {
                QTRY_COMPARE_WITH_TIMEOUT(lifecycle, QList<QByteArray>{"rendered"}, 5000);
            } else {
                QTest::qWait(500);
                QVERIFY(lifecycle.isEmpty());
            }
            QCOMPARE(surface.frameImportCount(), quint64(0));
        }
        QTRY_COMPARE_WITH_TIMEOUT(lifecycle.size(), 2, 5000);
        QCOMPARE(lifecycle, (QList<QByteArray>{"rendered", "released"}));
    }

    void frameCallbacksArriveWhileTheEventLoopNeverBlocks() {
        WPEView *native = wpe_view_new(WpeRuntime::display());
        QVERIFY(native);
        const quint32 pixel = 0xff20c040;
        GBytes *bytes = g_bytes_new(&pixel, sizeof(pixel));
        WPEBuffer *buffer = WPE_BUFFER(
            wpe_buffer_shm_new(WpeRuntime::display(), 1, 1, WPE_PIXEL_FORMAT_ARGB8888, bytes, sizeof(pixel))
        );
        g_bytes_unref(bytes);
        QVERIFY(buffer);
        const auto cleanup = qScopeGuard([&] {
            g_object_unref(buffer);
            g_object_unref(native);
        });
        QList<QByteArray> lifecycle;
        g_signal_connect(
            native,
            "buffer-rendered",
            G_CALLBACK(+[](WPEView *, WPEBuffer *, QList<QByteArray> *events) { events->append("rendered"); }),
            &lifecycle
        );
        g_signal_connect(
            native,
            "buffer-released",
            G_CALLBACK(+[](WPEView *, WPEBuffer *, QList<QByteArray> *events) { events->append("released"); }),
            &lifecycle
        );
        QTest::qWait(300);
        const auto pump = [&](qsizetype count) {
            QElapsedTimer elapsed;
            elapsed.start();
            while (lifecycle.size() < count && elapsed.elapsed() < 1000) {
                QCoreApplication::processEvents();
                QThread::msleep(1);
            }
            return elapsed.elapsed();
        };
        {
            WpeFrame frame(native, buffer);
            frame.rendered();
            const auto acknowledged = pump(1);
            QVERIFY2(acknowledged < 200, qPrintable(QString("A frame acknowledged on the main thread reached WebKit after %1 ms").arg(acknowledged)));
            QCOMPARE(lifecycle, QList<QByteArray>{"rendered"});
        }
        const auto released = pump(2);
        QVERIFY2(released < 200, qPrintable(QString("A frame released on the main thread reached WebKit after %1 ms").arg(released)));
        QCOMPARE(lifecycle, (QList<QByteArray>{"rendered", "released"}));
    }

    void frameAcknowledgementRetainsBufferUntilRelease() {
        WPEView *native = wpe_view_new(WpeRuntime::display());
        QVERIFY(native);
        const quint32 pixel = 0xff20c040;
        GBytes *bytes = g_bytes_new(&pixel, sizeof(pixel));
        WPEBuffer *buffer = WPE_BUFFER(
            wpe_buffer_shm_new(WpeRuntime::display(), 1, 1, WPE_PIXEL_FORMAT_ARGB8888, bytes, sizeof(pixel))
        );
        g_bytes_unref(bytes);
        QVERIFY(buffer);
        const auto cleanup = qScopeGuard([&] {
            g_object_unref(buffer);
            g_object_unref(native);
        });
        QList<QByteArray> lifecycle;
        g_signal_connect(
            native,
            "buffer-rendered",
            G_CALLBACK(+[](WPEView *, WPEBuffer *, QList<QByteArray> *events) { events->append("rendered"); }),
            &lifecycle
        );
        g_signal_connect(
            native,
            "buffer-released",
            G_CALLBACK(+[](WPEView *, WPEBuffer *, QList<QByteArray> *events) { events->append("released"); }),
            &lifecycle
        );
        {
            WpeFrame frame(native, buffer);
            frame.rendered();
            frame.rendered();
            QVERIFY(lifecycle.isEmpty());
        }
        QTRY_COMPARE_WITH_TIMEOUT(lifecycle.size(), 2, 5000);
        QCOMPARE(lifecycle, (QList<QByteArray>{"rendered", "released"}));
        {
            WpeFrame dropped(native, buffer);
        }
        QTRY_COMPARE_WITH_TIMEOUT(lifecycle.size(), 4, 5000);
        QCOMPARE(lifecycle, (QList<QByteArray>{"rendered", "released", "rendered", "released"}));
    }

    void frameAcknowledgementDoesNotWaitForSwap() {
        QFETCH(QByteArray, policy);
        QFETCH(bool, acknowledgesBeforeSwap);
        const bool hadPolicy = qEnvironmentVariableIsSet("EDEN_WPE_FRAME_ACK");
        const QByteArray previousPolicy = qgetenv("EDEN_WPE_FRAME_ACK");
        const auto restorePolicy = qScopeGuard([&] {
            if (hadPolicy) {
                qputenv("EDEN_WPE_FRAME_ACK", previousPolicy);
            } else {
                qunsetenv("EDEN_WPE_FRAME_ACK");
            }
        });
        if (policy.isEmpty()) {
            qunsetenv("EDEN_WPE_FRAME_ACK");
        } else {
            qputenv("EDEN_WPE_FRAME_ACK", policy);
        }
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(640, 480);
        view.attach(window.contentItem());
        window.show();
        view.load(url("frame-ack"));
        QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading() && view.title() == "frame-ack", 15000);
        QTRY_VERIFY_WITH_TIMEOUT(view.surface()->textureImportCount() > 0, 15000);
        QSignalSpy settling(&window, &QQuickWindow::frameSwapped);
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                const auto before = settling.size();
                QTest::qWait(300);
                return settling.size() == before;
            }(),
            10000
        );
        struct Acknowledgement {
            std::atomic<int> count = 0;
            std::atomic<int> swaps = 0;
            std::atomic<int> swapsAtFirst = -1;
            QSemaphore resumeRendering;
            bool releasesRendering = false;
        } state;
        state.releasesRendering = acknowledgesBeforeSwap;
        WPEView *native = view.surface()->nativeView();
        const gulong handler = g_signal_connect(
            native,
            "buffer-rendered",
            G_CALLBACK(+[](WPEView *, WPEBuffer *, Acknowledgement *state) {
                if (!state->count.fetch_add(1)) {
                    state->swapsAtFirst = state->swaps.load();
                    if (state->releasesRendering) {
                        state->resumeRendering.release();
                    }
                }
            }),
            &state
        );
        const auto disconnectNative = qScopeGuard([&] { g_signal_handler_disconnect(native, handler); });
        std::atomic<bool> armed = true;
        std::atomic<bool> blocked = false;
        std::atomic<bool> singleThreaded = false;
        std::atomic<int> swapsWhenBlocked = 0;
        auto *surface = view.surface();
        const quint64 importsWhenArmed = surface->frameImportCount();
        const auto swapConnection =
            connect(&window, &QQuickWindow::frameSwapped, &window, [&] { ++state.swaps; }, Qt::DirectConnection);
        const auto blockConnection = connect(
            &window,
            &QQuickWindow::afterRendering,
            &window,
            [&] {
                if (QThread::isMainThread()) {
                    singleThreaded = true;
                    return;
                }
                if (surface->frameImportCount() == importsWhenArmed || !armed.exchange(false)) {
                    return;
                }
                swapsWhenBlocked = state.swaps.load();
                blocked = true;
                state.resumeRendering.tryAcquire(1, acknowledgesBeforeSwap ? 5000 : 1000);
            },
            Qt::DirectConnection
        );
        const auto resume = qScopeGuard([&] {
            disconnect(blockConnection);
            disconnect(swapConnection);
            state.resumeRendering.release();
            window.hide();
        });
        evaluate(view, "document.querySelector('#box').style.background='blue';true");
        QTRY_VERIFY_WITH_TIMEOUT(blocked || singleThreaded, 10000);
        if (singleThreaded) {
            QSKIP("This regression requires the threaded scene graph");
        }
        QTRY_VERIFY_WITH_TIMEOUT(state.count.load() > 0, 10000);
        if (acknowledgesBeforeSwap) {
            QVERIFY(state.swapsAtFirst.load() <= swapsWhenBlocked.load());
        } else {
            QVERIFY(state.swapsAtFirst.load() > swapsWhenBlocked.load());
        }
        QTRY_VERIFY_WITH_TIMEOUT(state.swaps.load() > swapsWhenBlocked.load(), 5000);
    }

    void thumbnailTimeoutDoesNotCaptureDuringNavigation() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        view.load(url("slow-thumbnail"));
        QTRY_VERIFY_WITH_TIMEOUT(view.isLoading(), 5000);
        bool completed = false;
        QImage thumbnail;
        view.requestThumbnail(QSize(160, 120), [&](const QImage &image) {
            thumbnail = image;
            completed = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(completed, 4000);
        QVERIFY(view.isLoading());
        QVERIFY(thumbnail.isNull());
        QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 5000);
    }

    void renderingResizeVisibilityAndThumbnail() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(640, 480);
        view.attach(window.contentItem());
        window.show();
        view.load(url("pixels"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("pixels"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
        QImage captured;
        QTRY_VERIFY_WITH_TIMEOUT(
            (captured = window.grabWindow(),
             !captured.isNull() &&
                 captured.pixelColor(qRound(50 * window.devicePixelRatio()), qRound(50 * window.devicePixelRatio()))
                         .green() > 150),
            15000
        );
        QVERIFY(
            captured.pixelColor(qRound(200 * window.devicePixelRatio()), qRound(200 * window.devicePixelRatio()))
                .red() > 150
        );
        if (qEnvironmentVariableIsSet("EDEN_WPE_REQUIRE_GPU")) {
            QVERIFY2(view.surface()->accelerated(), qPrintable(view.surface()->renderingDiagnostic()));
        }
        bool completed = false;
        QImage thumbnail;
        view.requestThumbnail(QSize(160, 120), [&](const QImage &image) {
            thumbnail = image;
            completed = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(completed, 15000);
        QCOMPARE(thumbnail.size(), QSize(160, 120));
        window.resize(800, 600);
        QTRY_COMPARE(wpe_view_get_width(view.surface()->nativeView()), 800);
        QTRY_VERIFY_WITH_TIMEOUT(
            (captured = window.grabWindow(),
             captured.width() == qRound(800 * window.devicePixelRatio()) &&
                 captured.pixelColor(qRound(50 * window.devicePixelRatio()), qRound(50 * window.devicePixelRatio()))
                         .green() > 150),
            15000
        );
        window.hide();
        QTRY_VERIFY(!wpe_view_get_visible(view.surface()->nativeView()));
        window.show();
        QTRY_VERIFY(wpe_view_get_visible(view.surface()->nativeView()));
        view.attach(nullptr);
        QVERIFY(!wpe_view_get_visible(view.surface()->nativeView()));
    }

    void credentialFillPreservesHiddenLoginTokens() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(800, 600);
        view.attach(window.contentItem());
        window.show();
        view.load(url("login-form"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("login-form"), 15000);
        QVERIFY(evaluate(view, R"JS(document.body.innerHTML=`
<form><input type="hidden" name="authenticity_token" value="synthetic-csrf">
<input type="hidden" name="required_field_secret" value="synthetic-trap">
<input name="login" id="login_field" autocomplete="username">
<input type="password" name="password" id="password" autocomplete="current-password">
<input name="otp" autocomplete="one-time-code" value="654321"></form>`;true)JS")
                    .toBool());
        AutofillTarget target;
        view.requestAutofillTarget([&](AutofillTarget value) { target = std::move(value); });
        QTRY_VERIFY_WITH_TIMEOUT(target.isValid(), 5000);
        view.fillCredential(target, "synthetic-user", "synthetic-password");
        QTRY_COMPARE(
            evaluate(view, "document.getElementById('password').value").toString(),
            QString("synthetic-password")
        );
        QCOMPARE(evaluate(view, "document.getElementById('login_field').value").toString(), QString("synthetic-user"));
        QCOMPARE(
            evaluate(view, "document.querySelector('[name=authenticity_token]').value").toString(),
            QString("synthetic-csrf")
        );
        QCOMPARE(
            evaluate(view, "document.querySelector('[name=required_field_secret]').value").toString(),
            QString("synthetic-trap")
        );
        QCOMPARE(evaluate(view, "document.querySelector('[name=otp]').value").toString(), QString("654321"));
        evaluate(view, "document.getElementById('password').focus();document.getElementById('password').select();true");
        view.surface()->forceActiveFocus();
        QGuiApplication::clipboard()->setText("synthetic-pasted-password");
        QTest::keyClick(&window, Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE(
            evaluate(view, "document.getElementById('password').value").toString(),
            QString("synthetic-pasted-password")
        );
    }

    void inspectorResizeReflowsWithoutPointerEvents() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(1200, 700);
        QQuickItem page(window.contentItem());
        page.setSize(window.size());
        QQuickItem inspector(window.contentItem());
        inspector.setPosition(QPointF(800, 0));
        inspector.setSize(QSizeF(400, 700));
        view.attach(&page);
        view.attachDevTools(&inspector);
        window.show();
        view.load(url("resize-inspector"));
        QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
        evaluate(
            view,
            "document.body.style.cssText='margin:0;background:#e03020';"
            "document.body.innerHTML='<div style=\"position:fixed;right:0;bottom:0;"
            "width:50vw;height:100vh;background:#20d040\"></div>';true"
        );
        QString reflowFailure;
        const auto matchesViewport = [&] {
            const auto size = evaluate(
                                  view,
                                  "[innerWidth,innerHeight,document.querySelector('div').getBoundingClientRect().x,"
                                  "document.querySelector('div').getBoundingClientRect().width]"
            )
                                  .toList();
            reflowFailure = QString("window=%1x%2 viewport=%3x%4 DOM=%5x%6 scale=%7")
                                .arg(window.width())
                                .arg(window.height())
                                .arg(page.width())
                                .arg(page.height())
                                .arg(size.value(0).toInt())
                                .arg(size.value(1).toInt())
                                .arg(window.devicePixelRatio());
            if (size.size() != 4 || size.at(0).toInt() != int(page.width()) ||
                size.at(1).toInt() != int(page.height()) || size.at(2).toDouble() != page.width() / 2 ||
                size.at(3).toDouble() != page.width() / 2) {
                return false;
            }
            const auto frame = window.grabWindow();
            const auto scale = window.devicePixelRatio();
            if (frame.isNull() || frame.width() <= qRound(page.width() * 0.6 * scale) ||
                frame.height() <= qRound(100 * scale)) {
                return false;
            }
            reflowFailure += QString(" box=%1,%2").arg(size.value(2).toDouble()).arg(size.value(3).toDouble());
            reflowFailure += QString(" frame=%1x%2 green=%3 red=%4")
                                 .arg(frame.width())
                                 .arg(frame.height())
                                 .arg(frame.pixelColor(qRound(page.width() * 0.6 * scale), qRound(100 * scale)).name())
                                 .arg(frame.pixelColor(qRound(page.width() * 0.4 * scale), qRound(100 * scale)).name());
            return !frame.isNull() &&
                   frame.pixelColor(qRound(page.width() * 0.6 * scale), qRound(100 * scale)).green() > 150 &&
                   frame.pixelColor(qRound(page.width() * 0.4 * scale), qRound(100 * scale)).red() > 150;
        };
        QTRY_VERIFY2_WITH_TIMEOUT(matchesViewport(), qPrintable(reflowFailure), 5000);
        for (int repeat = 0; repeat < 3; ++repeat) {
            view.openDevTools();
            QTRY_VERIFY2_WITH_TIMEOUT(view.devToolsOpen(), qPrintable(QString("repeat=%1").arg(repeat)), 3000);
            page.setWidth(800);
            QTRY_VERIFY2_WITH_TIMEOUT(matchesViewport(), qPrintable(reflowFailure), 2000);
            QVERIFY(!inspector.childItems().isEmpty());
            auto *surface = qobject_cast<WpeSurface *>(inspector.childItems().first());
            QVERIFY(surface);
            for (int width : {760, 640, 450, 550, 820, 700}) {
                page.setWidth(width);
                inspector.setX(width);
                inspector.setWidth(1200 - width);
                QCOMPARE(wpe_view_get_width(surface->nativeView()), 1200 - width);
                QTRY_VERIFY2_WITH_TIMEOUT(matchesViewport(), qPrintable(reflowFailure), 2000);
            }
            view.closeDevTools();
            page.setWidth(1200);
            QTRY_VERIFY2_WITH_TIMEOUT(matchesViewport(), qPrintable(reflowFailure), 2000);
        }
    }

    void popupKeepsOpener() {
        WpeProfile profile(privateParameters());
        WpeEngineView source(&profile);
        std::unique_ptr<WpeEngineView> popup;
        QQuickWindow window;
        window.resize(640, 480);
        source.attach(window.contentItem());
        window.show();
        connect(&source, &EngineView::newViewRequested, this, [&](EngineNewViewRequest *request) {
            popup = std::make_unique<WpeEngineView>(&profile);
            QVERIFY(request->openIn(popup.get()));
        });
        source.load(url("opener"));
        QTRY_COMPARE_WITH_TIMEOUT(source.title(), QString("opener"), 15000);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(40, 150));
        QTRY_VERIFY_WITH_TIMEOUT(popup != nullptr, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(popup->title(), QString("Popup"), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(source.title(), QString("popup-ready"), 15000);
    }

    void hangRecorderCoversTheUserInterfaceProcess() {
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("EDEN_WPE_HANG_LOG", "200");
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(), {"--ui-hang-child"});
        QVERIFY2(child.waitForStarted(10000), qPrintable(child.errorString()));
        QVERIFY2(child.waitForFinished(60000), "The stalled child never finished");
        const QString log = QString::fromUtf8(child.readAllStandardError() + child.readAllStandardOutput());
        const auto excerpt = log.left(4000);
        QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0, qPrintable(excerpt));
        const QString name = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
        QVERIFY2(log.contains(QString("Eden hang recorder: %1 reports main thread stalls over 200 ms").arg(name)), qPrintable(excerpt));
        const auto stallMatch =
            QRegularExpression(QString("Eden hang recorder: %1 main thread has not run for \\d+ ms, thread stacks follow").arg(QRegularExpression::escape(name))).match(log);
        QVERIFY2(stallMatch.hasMatch(), qPrintable(excerpt));
        const auto afterStall = log.mid(stallMatch.capturedEnd());
        QVERIFY2(QRegularExpression("\\n    libWPEWebKit-2\\.0\\.so\\.1\\+0x[0-9a-f]+").match(afterStall).hasMatch(), qPrintable(afterStall.left(4000)));
        const auto resumed = QRegularExpression(QString("Eden hang recorder: %1 main thread resumed after (\\d+) ms").arg(QRegularExpression::escape(name))).match(afterStall);
        QVERIFY2(resumed.hasMatch(), qPrintable(excerpt));
        const int stalledFor = resumed.captured(1).toInt();
        QVERIFY2(stalledFor >= 700 && stalledFor <= 3000, qPrintable(QString("The recorder measured a %1 ms stall for a 1000 ms busy loop").arg(stalledFor)));
    }

    void endingPlayedTracksDoesNotBlockThePage() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(900, 600);
        view.attach(window.contentItem());
        window.show();
        webkit_settings_set_media_playback_requires_user_gesture(webkit_web_view_get_settings(view.nativeView()), FALSE);
        view.load(url("track-ending"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("track-ending"), 15000);
        evaluate(view, R"JS(window.trackEnding = null;
(async () => {
    const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
    const waitUntil = async (condition, what) => {
        const deadline = performance.now() + 15000;
        while (!condition()) {
            if (performance.now() > deadline)
                throw new Error(what);
            await delay(20);
        }
    };
    const play = stream => {
        const video = document.createElement('video');
        video.muted = true;
        video.playsInline = true;
        video.width = 64;
        video.height = 48;
        video.srcObject = stream;
        document.body.appendChild(video);
        video.play();
        return video;
    };
    const playing = videos => videos.every(video => video.readyState >= 2 && video.videoWidth > 0 && !video.paused);
    const painters = [];
    const local = [];
    for (let index = 0; index < 12; ++index) {
        const canvas = document.createElement('canvas');
        canvas.width = 64;
        canvas.height = 48;
        const context = canvas.getContext('2d');
        painters.push({context, hue: index * 30});
        local.push(play(canvas.captureStream(30)));
    }
    const painter = setInterval(() => {
        for (const item of painters) {
            item.hue = (item.hue + 9) % 360;
            item.context.fillStyle = `hsl(${item.hue} 80% 50%)`;
            item.context.fillRect(0, 0, 64, 48);
        }
    }, 33);
    await waitUntil(() => playing(local), 'the canvas videos never played: ' + JSON.stringify(local.map(video => [video.readyState, video.videoWidth, video.paused])));
    await delay(300);
    const tracks = local.map(video => video.srcObject.getVideoTracks()[0]);
    let started = performance.now();
    for (const track of tracks)
        track.stop();
    const stopBlocked = performance.now() - started;
    await waitUntil(() => local.every(video => !video.srcObject.active), 'the stopped streams stayed active');
    const caller = new RTCPeerConnection();
    const callee = new RTCPeerConnection();
    caller.onicecandidate = event => event.candidate && callee.addIceCandidate(event.candidate);
    callee.onicecandidate = event => event.candidate && caller.addIceCandidate(event.candidate);
    const remote = [];
    callee.ontrack = event => remote.push(play(new MediaStream([event.track])));
    for (const item of painters.slice(0, 4)) {
        const stream = item.context.canvas.captureStream(30);
        caller.addTrack(stream.getVideoTracks()[0], stream);
    }
    await caller.setLocalDescription();
    await callee.setRemoteDescription(caller.localDescription);
    await callee.setLocalDescription();
    await caller.setRemoteDescription(callee.localDescription);
    await waitUntil(() => remote.length === 4 && playing(remote), 'the remote videos never played: ' + JSON.stringify(remote.map(video => [video.readyState, video.videoWidth, video.paused])));
    await delay(300);
    started = performance.now();
    callee.close();
    const closeBlocked = performance.now() - started;
    await waitUntil(() => remote.every(video => video.srcObject.getVideoTracks()[0].readyState === 'ended'),
        'the remote tracks never ended: ' + JSON.stringify(remote.map(video => video.srcObject.getVideoTracks()[0].readyState)));
    const remoteEnded = true;
    caller.close();
    clearInterval(painter);
    window.trackEnding = {stopBlocked, closeBlocked, ended: tracks.every(track => track.readyState === 'ended'), remoteEnded};
})().catch(error => window.trackEnding = {fatal: String(error && error.message || error)});)JS");
        QTRY_VERIFY_WITH_TIMEOUT(evaluate(view, "window.trackEnding !== null").toBool(), 60000);
        const QJsonObject result =
            QJsonDocument::fromJson(evaluate(view, "JSON.stringify(window.trackEnding)").toString().toUtf8()).object();
        const QByteArray summary = QJsonDocument(result).toJson(QJsonDocument::Compact);
        QVERIFY2(!result.contains("fatal"), summary.constData());
        QVERIFY2(result.value("stopBlocked").toDouble() < 100, qPrintable("Stopping 12 playing tracks blocked the page: " + summary));
        QVERIFY2(result.value("closeBlocked").toDouble() < 100, qPrintable("Closing a call with 4 playing remote tracks blocked the page: " + summary));
        QVERIFY2(result.value("ended").toBool(), summary.constData());
        QVERIFY2(result.value("remoteEnded").toBool(), summary.constData());
    }

    void automationProfilesControlTheirViews() {
        {
            WpeProfile browsing(privateParameters());
            WpeEngineView view(&browsing);
            QVERIFY(!browsing.controlledByAutomation());
            QVERIFY(!webkit_web_context_is_automation_allowed(browsing.context()));
            QVERIFY(!webkit_web_view_is_controlled_by_automation(view.nativeView()));
            QCOMPARE(webkit_web_view_get_network_session(view.nativeView()), browsing.session());
        }
        const QString storage = m_storage.filePath("automation");
        auto parameters = regularParameters(storage, "automation");
        parameters.controlledByAutomation = true;
        WpeProfile profile(parameters);
        QVERIFY(profile.isPrivate());
        QVERIFY(profile.controlledByAutomation());
        QVERIFY(!profile.supportsPortableCookies());
        QVERIFY(!QFileInfo::exists(storage));
        QCOMPARE(profile.session(), webkit_web_context_get_network_session_for_automation(profile.context()));
        QVERIFY(webkit_network_session_is_ephemeral(profile.session()));
        {
            auto competing = regularParameters(m_storage.filePath("automation-competing"), "automation-competing");
            competing.controlledByAutomation = true;
            QTest::ignoreMessage(
                QtWarningMsg,
                "WebKit automation is unavailable, so the automation profile uses its own private session"
            );
            WpeProfile other(competing);
            QVERIFY(!other.controlledByAutomation());
            QVERIFY(other.isPrivate());
            QVERIFY(webkit_network_session_is_ephemeral(other.session()));
            QVERIFY(other.session() != profile.session());
            WpeEngineView view(&other);
            QVERIFY(!webkit_web_view_is_controlled_by_automation(view.nativeView()));
        }
        WpeEngineView source(&profile);
        QVERIFY(webkit_web_view_is_controlled_by_automation(source.nativeView()));
        QCOMPARE(webkit_web_view_get_network_session(source.nativeView()), profile.session());
        std::unique_ptr<WpeEngineView> popup;
        QQuickWindow window;
        window.resize(640, 480);
        source.attach(window.contentItem());
        window.show();
        connect(&source, &EngineView::newViewRequested, this, [&](EngineNewViewRequest *request) {
            popup = std::make_unique<WpeEngineView>(&profile);
            QVERIFY(request->openIn(popup.get()));
        });
        source.load(url("opener"));
        QTRY_COMPARE_WITH_TIMEOUT(source.title(), QString("opener"), 15000);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(40, 150));
        QTRY_VERIFY_WITH_TIMEOUT(popup != nullptr, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(popup->title(), QString("Popup"), 15000);
        QVERIFY(webkit_web_view_is_controlled_by_automation(popup->nativeView()));
        QCOMPARE(webkit_web_view_get_network_session(popup->nativeView()), profile.session());
        QTRY_COMPARE_WITH_TIMEOUT(source.title(), QString("popup-ready"), 15000);
        QVERIFY(!QFileInfo::exists(storage));
    }

    void dialogsAndCancellation() {
        WpeProfile profile(privateParameters());
        auto view = std::make_unique<WpeEngineView>(&profile);
        view->load(url("dialogs"));
        QTRY_COMPARE_WITH_TIMEOUT(view->title(), QString("dialogs"), 15000);
        QSignalSpy requested(view.get(), &EngineView::javaScriptDialogRequested);
        bool completed = false;
        QVariant result;
        view->evaluate("prompt('Test prompt','initial')", [&](QVariant value) {
            result = value;
            completed = true;
        });
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 1, 15000);
        const auto info = qvariant_cast<JavaScriptDialogInfo>(requested.first().first());
        QCOMPARE(info.kind, QString("prompt"));
        QCOMPARE(info.defaultText, QString("initial"));
        view->resolveJavaScriptDialog(info.id, true, "resolved");
        QTRY_VERIFY_WITH_TIMEOUT(completed, 15000);
        QCOMPARE(result.toString(), QString("resolved"));
        int callbacks = 0;
        view->evaluate("document.title", [&](QVariant) { ++callbacks; });
        view.reset();
        QTRY_COMPARE_WITH_TIMEOUT(callbacks, 1, 15000);
        QTest::qWait(100);
        QCOMPARE(callbacks, 1);
    }

    void autofillRejectsStaleDocumentsAndPageForgeries() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        view.load(url("form"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("Form"), 15000);
        QTRY_VERIFY(!view.isLoading());
        AutofillTarget target;
        bool ready = false;
        view.requestAutofillTarget([&](AutofillTarget value) {
            target = std::move(value);
            ready = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(ready, 15000);
        QVERIFY(target.isValid());
        QSignalSpy credentials(&view, &EngineView::credentialSubmitted);
        QSignalSpy fields(&view, &EngineView::formFieldFocused);
        view.fillCredential(target, "synthetic-user", "synthetic-value");
        QVariant result;
        bool evaluated = false;
        view.evaluate("document.querySelector('#secret').value", [&](QVariant value) {
            result = value;
            evaluated = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(evaluated, 15000);
        QCOMPARE(result.toString(), QString("synthetic-value"));
        QCOMPARE(credentials.size(), 0);
        view.reload();
        bool pendingAnswered = false;
        view.requestAutofillTarget([&](AutofillTarget pending) {
            QVERIFY(!pending.isValid());
            pendingAnswered = true;
        });
        QVERIFY(pendingAnswered);
        QTRY_VERIFY_WITH_TIMEOUT(view.isLoading(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
        view.fillCredential(target, "stale-user", "stale-value");
        evaluated = false;
        view.evaluate("document.querySelector('#secret').value", [&](QVariant value) {
            result = value;
            evaluated = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(evaluated, 15000);
        QCOMPARE(result.toString(), QString());
        evaluated = false;
        view.evaluate(
            "typeof window.webkit?.messageHandlers?.forms",
            [&](QVariant value) {
                result = value;
                evaluated = true;
            },
            nullptr
        );
        QTRY_VERIFY_WITH_TIMEOUT(evaluated, 15000);
        QCOMPARE(result.toString(), QString("undefined"));
        QCOMPARE(credentials.size(), 0);
    }

    void webRtcEventsReachTheConfiguredFile() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto restore = qScopeGuard([] { qunsetenv("WEBKIT_WEBRTC_JSON_EVENTS_FILE"); });
        qputenv("WEBKIT_WEBRTC_JSON_EVENTS_FILE", "relative-events.jsonl");
        QTest::ignoreMessage(QtWarningMsg, "WEBKIT_WEBRTC_JSON_EVENTS_FILE must be an absolute path in a directory Eden can create");
        {
            WpeProfile rejected(privateParameters());
        }
        const QString events = directory.filePath("webrtc/events.jsonl");
        qputenv("WEBKIT_WEBRTC_JSON_EVENTS_FILE", QFile::encodeName(events));
        {
            WpeProfile profile(privateParameters());
            WpeEngineView view(&profile);
            QQuickWindow window;
            window.resize(400, 300);
            view.attach(window.contentItem());
            window.show();
            view.load(url("webrtc-events"));
            QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("webrtc-events"), 15000);
            evaluate(view, R"JS(window.webrtcEvents = '';
(async () => {
    const canvas = document.createElement('canvas');
    canvas.width = 160;
    canvas.height = 120;
    const context = canvas.getContext('2d');
    const painter = setInterval(() => {
        context.fillStyle = `hsl(${Date.now() % 360} 80% 50%)`;
        context.fillRect(0, 0, 160, 120);
    }, 33);
    const stream = canvas.captureStream(30);
    const caller = new RTCPeerConnection();
    const callee = new RTCPeerConnection();
    caller.onicecandidate = event => event.candidate && callee.addIceCandidate(event.candidate);
    callee.onicecandidate = event => event.candidate && caller.addIceCandidate(event.candidate);
    caller.addTrack(stream.getVideoTracks()[0], stream);
    await caller.setLocalDescription();
    await callee.setRemoteDescription(caller.localDescription);
    await callee.setLocalDescription();
    await caller.setRemoteDescription(callee.localDescription);
    await new Promise(resolve => setTimeout(resolve, 6000));
    clearInterval(painter);
    caller.close();
    callee.close();
    window.webrtcEvents = 'done';
})().catch(error => window.webrtcEvents = 'failed: ' + error);
true)JS");
            QTRY_VERIFY_WITH_TIMEOUT(!evaluate(view, "window.webrtcEvents").toString().isEmpty(), 30000);
            QCOMPARE(evaluate(view, "window.webrtcEvents").toString(), QString("done"));
        }
        QFile file(events);
        QTRY_VERIFY_WITH_TIMEOUT(file.exists() && file.size() > 0, 10000);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();
        QVERIFY2(content.contains("\"peer-connection\""), content.left(2000).constData());
        QVERIFY2(content.contains("outbound-rtp"), content.left(2000).constData());
    }

    void profilesIsolateAndReplaceCookies() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto firstParameters = regularParameters(directory.path() + "/first", "first");
        auto secondParameters = regularParameters(directory.path() + "/second", "second");
        WpeProfile first(firstParameters);
        WpeProfile second(secondParameters);
        PortableCookie cookie;
        cookie.name = "synthetic-session";
        cookie.value = "test-only";
        cookie.domain = "127.0.0.1";
        cookie.path = "/";
        cookie.hostOnly = true;
        cookie.httpOnly = true;
        cookie.sameSite = CookieSameSite::Lax;
        bool completed = false;
        CookieReplaceResult replaced;
        first.replacePortableCookies({cookie}, [&](CookieReplaceResult value) {
            replaced = value;
            completed = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(completed, 15000);
        QVERIFY2(replaced.errorCode.isEmpty(), qPrintable(replaced.errorCode));
        QCOMPARE(replaced.importedCookies, 1);
        CookieSnapshotResult snapshot;
        completed = false;
        second.exportPortableCookies([&](CookieSnapshotResult value) {
            snapshot = std::move(value);
            completed = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(completed, 15000);
        QVERIFY(snapshot.cookies.isEmpty());
        completed = false;
        first.exportPortableCookies([&](CookieSnapshotResult value) {
            snapshot = std::move(value);
            completed = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(completed, 15000);
        QCOMPARE(snapshot.cookies.size(), 1);
        QCOMPARE(snapshot.cookies.first().value, cookie.value);
        QVERIFY(snapshot.cookies.first().hostOnly);
        QVERIFY(snapshot.cookies.first().httpOnly);
        completed = false;
        second.replacePortableCookies(snapshot.cookies, [&](CookieReplaceResult value) {
            replaced = value;
            completed = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(completed, 15000);
        QVERIFY(replaced.errorCode.isEmpty());
        QCOMPARE(replaced.importedCookies, 1);
        completed = false;
        second.replacePortableCookies({}, [&](CookieReplaceResult value) {
            replaced = value;
            completed = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(completed, 15000);
        QVERIFY(replaced.errorCode.isEmpty());
        completed = false;
        second.exportPortableCookies([&](CookieSnapshotResult value) {
            snapshot = std::move(value);
            completed = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(completed, 15000);
        QVERIFY(snapshot.cookies.isEmpty());
        WpeProfile privateProfile(privateParameters());
        QVERIFY(!privateProfile.supportsPortableCookies());
        QVERIFY(webkit_network_session_is_ephemeral(privateProfile.session()));
    }

    void inputCompositionClipboardAndScrolling() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(640, 480);
        view.attach(window.contentItem());
        window.show();
        view.load(url("input"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("Input"), 15000);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(50, 25));
        QTRY_VERIFY(view.surface()->hasActiveFocus());
        QTRY_VERIFY(view.surface()->inputMethodQuery(Qt::ImEnabled).toBool());
        QKeyEvent keys(QEvent::KeyPress, 0, Qt::NoModifier, QString::fromUtf8("Aé𝄞"));
        QCoreApplication::sendEvent(view.surface(), &keys);
        QTRY_COMPARE(evaluate(view, "document.querySelector('#text').value").toString(), QString::fromUtf8("Aé𝄞"));
        QInputMethodEvent preedit(QString::fromUtf8("日本"), {});
        QCoreApplication::sendEvent(view.surface(), &preedit);
        QInputMethodEvent commit;
        commit.setCommitString(QString::fromUtf8("日本"));
        QCoreApplication::sendEvent(view.surface(), &commit);
        QTRY_COMPARE(evaluate(view, "document.querySelector('#text').value").toString(), QString::fromUtf8("Aé𝄞日本"));
        evaluate(view, "document.querySelector('#text').select()");
        view.executeContextMenuCommand("copy");
        QTRY_COMPARE(QGuiApplication::clipboard()->text(), QString::fromUtf8("Aé𝄞日本"));
        QWheelEvent wheel(
            QPointF(400, 350),
            QPointF(400, 350),
            QPoint(),
            QPoint(0, -120),
            Qt::NoButton,
            Qt::NoModifier,
            Qt::NoScrollPhase,
            false
        );
        QCoreApplication::sendEvent(view.surface(), &wheel);
        QTRY_VERIFY(evaluate(view, "window.scrollY").toDouble() > 0);
    }

    void fileChooserAndSelectMenu() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(640, 480);
        view.attach(window.contentItem());
        window.show();
        view.load(url("input"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("Input"), 15000);
        QSignalSpy files(&view, &EngineView::fileDialogRequested);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(40, 65));
        QTRY_COMPARE_WITH_TIMEOUT(files.size(), 1, 15000);
        const auto request = qvariant_cast<FileDialogInfo>(files.first().first());
        QCOMPARE(request.mode, QString("openMultiple"));
        QVERIFY(!request.nameFilters.isEmpty());
        QTemporaryDir directory;
        QFile selected(directory.filePath("selected.txt"));
        QVERIFY(selected.open(QIODevice::WriteOnly));
        selected.write("synthetic-selection");
        selected.close();
        view.resolveFileDialog(request.id, true, {QUrl::fromLocalFile(selected.fileName())});
        QTRY_COMPARE(
            evaluate(view, "document.querySelector('#file').files[0]?.name").toString(),
            QString("selected.txt")
        );
        QSignalSpy menus(&view, &EngineView::contextMenuRequested);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(40, 115));
        QTRY_COMPARE_WITH_TIMEOUT(menus.size(), 1, 15000);
        const auto menu = qvariant_cast<ContextMenuInfo>(menus.first().first());
        QCOMPARE(menu.actions.size(), 2);
        view.executeContextMenuCommand(menu.actions.last().toMap().value("id").toString());
        QTRY_COMPARE(evaluate(view, "document.querySelector('#select').selectedIndex").toInt(), 1);
    }

    void downloadsAndFirstRequestAuthentication() {
        WpeProfile profile(regularParameters(m_storage.path(), "download"));
        WpeEngineView view(&profile);
        PortableCookie cookie;
        cookie.name = "synthetic-session";
        cookie.value = "test-only";
        cookie.domain = "127.0.0.1";
        cookie.path = "/";
        cookie.hostOnly = true;
        cookie.httpOnly = true;
        bool imported = false;
        profile.replacePortableCookies({cookie}, [&](CookieReplaceResult result) {
            imported = result.errorCode.isEmpty();
        });
        QTRY_VERIFY_WITH_TIMEOUT(imported, 15000);
        view.load(url("cookie-check"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("Authenticated"), 15000);
        QSignalSpy started(&profile, &EngineProfile::downloadStarted);
        QSignalSpy updated(&profile, &EngineProfile::downloadUpdated);
        view.load(url("download"));
        QTRY_COMPARE_WITH_TIMEOUT(started.size(), 1, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!updated.isEmpty() && updated.last().last().toString() == "complete", 15000);
        const QString path = started.first().at(3).toString();
        QVERIFY(path.startsWith(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + '/'));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("synthetic-download-content"));
        file.close();
        QVERIFY(file.remove());
    }

    void permissionOriginAndInspector() {
        WpeProfile profile(regularParameters(m_storage.path() + "/inspector", "inspector"));
        WpeEngineView view(&profile);
        if (!(view.capabilities() & EngineView::DockedDevtools)) {
            QSKIP("Requires the pinned embedding patch");
        }
        QQuickWindow window;
        window.resize(800, 600);
        view.attach(window.contentItem());
        window.show();
        view.load(url("notify"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("Permission"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(view.faviconUrl().toString().startsWith("data:image/png;base64,"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(view.surface()->textureImportCount() > 0, 15000);
        QVERIFY(evaluate(view, "window.isSecureContext").toBool());
        QSignalSpy requests(&view, &EngineView::permissionRequested);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(70, 35));
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 1, 15000);
        const auto request = qvariant_cast<PermissionRequestInfo>(requests.first().first());
        QCOMPARE(request.origin, autofillOrigin(url("notify")));
        QCOMPARE(request.permissions, QStringList{"notifications"});
        view.resolvePermissionRequest(request.id, false);
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("denied"), 15000);
        QQuickItem inspectorViewport(window.contentItem());
        inspectorViewport.setSize(QSizeF(800, 600));
        view.attachDevTools(&inspectorViewport);
        view.openDevTools();
        QTRY_VERIFY_WITH_TIMEOUT(view.devToolsOpen(), 15000);
        QVERIFY(!inspectorViewport.childItems().isEmpty());
        auto *surface = qobject_cast<WpeSurface *>(inspectorViewport.childItems().first());
        QVERIFY(surface);
        QTRY_VERIFY(wpe_view_get_visible(surface->nativeView()));
        QTRY_VERIFY_WITH_TIMEOUT(surface->textureImportCount() > 0, 15000);
        QTRY_COMPARE(wpe_view_get_width(surface->nativeView()), 800);
        const auto inspectorHasContent = [&] {
            const auto frame = window.grabWindow();
            if (frame.isNull()) {
                return false;
            }
            QSet<QRgb> colors;
            const qreal scale = window.devicePixelRatio();
            for (int y = 8; y < 580; y += 8) {
                for (int x = 8; x < 780; x += 8) {
                    colors.insert(frame.pixel(qRound(x * scale), qRound(y * scale)));
                }
            }
            return colors.size() > 16;
        };
        QTRY_VERIFY_WITH_TIMEOUT(inspectorHasContent(), 15000);
        const QString artifacts = qEnvironmentVariable("EDEN_WPE_TEST_ARTIFACT_DIR");
        if (!artifacts.isEmpty()) {
            QVERIFY(QDir().mkpath(artifacts));
            QVERIFY(window.grabWindow().save(QDir(artifacts).filePath("inspector.png")));
        }
        view.closeDevTools();
        QVERIFY(!view.devToolsOpen());
        QVERIFY(inspectorViewport.childItems().isEmpty());
        view.openDevTools();
        QTRY_VERIFY(view.devToolsOpen());
        view.closeDevTools();
    }

    void engineExperimentRuntimeEvidence() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(800, 600);
        view.attach(window.contentItem());
        window.show();
        view.load(url("runtime-evidence"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("runtime-evidence"), 15000);
        QVERIFY(evaluate(view, R"JS(
(() => {
    const canvas=document.createElement('canvas');canvas.width=800;canvas.height=600;document.body.replaceChildren(canvas);
    const context=canvas.getContext('2d');
    const functions=Array.from({length:32},(_,index)=>new Function('value','return Math.sqrt(value+'+index+')'));
    let total=0;
    for(let iteration=0;iteration<100000;iteration++) total+=functions[iteration%32](iteration);
    for(let index=0;index<4000;index++){context.beginPath();context.arc(index%800,(index*13)%600,8,0,Math.PI*2);context.fill();}
    window.testAllocations=Array.from({length:100000},(_,index)=>({index,text:'allocation '+index}));
    return Number.isFinite(total);
})()
)JS")
                    .toBool());
        QTRY_VERIFY_WITH_TIMEOUT(view.surface()->textureImportCount() > 0, 10000);
        QTest::qWait(300);
        const QString process = QString("/proc/%1").arg(view.rendererProcessId());
        const auto tasks = QDir(process + "/task").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        int helpers = 0;
        const int requestedNice = qEnvironmentVariableIntValue("WEBKIT_EDEN_HELPER_NICE");
        for (const auto &task : tasks) {
            QFile status(process + "/task/" + task + "/stat");
            if (!status.open(QIODevice::ReadOnly)) {
                continue;
            }
            const QByteArray state = status.readAll();
            const int close = state.lastIndexOf(')');
            const QByteArray name = state.mid(state.indexOf('(') + 1, close - state.indexOf('(') - 1);
            const auto fields = state.mid(close + 2).simplified().split(' ');
            const int nice = fields.value(16).toInt();
            if (name == "JITWorker" || name == "HeapHelper" || name == "SkiaCPUWorker" || name == "CanvasDDL") {
                ++helpers;
                if (requestedNice > 0) {
                    QVERIFY2(nice >= requestedNice, name.constData());
                }
                qInfo("Experiment helper %s nice=%d", name.constData(), nice);
            } else if (task == QString::number(view.rendererProcessId())) {
                QCOMPARE(nice, 0);
            }
        }
        if (requestedNice > 0) {
            QVERIFY(helpers > 0);
        }
        QFile mappings(process + "/smaps");
        if (mappings.open(QIODevice::ReadOnly)) {
            quint64 anonymousHuge = 0, fileHuge = 0, sharedHuge = 0, advisedRegions = 0, textAdvisedRegions = 0,
                    textFileHuge = 0;
            bool text = false;
            for (const QByteArray &line : mappings.readAll().split('\n')) {
                const auto fields = line.simplified().split(' ');
                if (fields.value(0).contains('-')) {
                    text = line.contains("libWPEWebKit-2.0.so") && fields.value(1).contains('x');
                }
                if (line.startsWith("AnonHugePages:")) {
                    anonymousHuge += fields.value(1).toULongLong();
                }
                if (line.startsWith("FilePmdMapped:")) {
                    fileHuge += fields.value(1).toULongLong();
                    if (text) {
                        textFileHuge += fields.value(1).toULongLong();
                    }
                }
                if (line.startsWith("ShmemPmdMapped:")) {
                    sharedHuge += fields.value(1).toULongLong();
                }
                if (line.startsWith("VmFlags:") && fields.contains("hg")) {
                    ++advisedRegions;
                    if (text) {
                        ++textAdvisedRegions;
                    }
                }
            }
            qInfo(
                "Experiment huge pages: anonymousKiB=%llu fileKiB=%llu sharedKiB=%llu advisedRegions=%llu "
                "textAdvisedRegions=%llu",
                anonymousHuge,
                fileHuge,
                sharedHuge,
                advisedRegions,
                textAdvisedRegions
            );
            qInfo("Experiment WebKit executable file huge pages KiB=%llu", textFileHuge);
            if (qEnvironmentVariable("WEBKIT_EDEN_TEXT_HUGE_PAGES") == "1") {
                QVERIFY(textAdvisedRegions > 0);
            }
            if (qEnvironmentVariable("WEBKIT_EDEN_JIT_HUGE_PAGES") == "1") {
                QVERIFY(advisedRegions > textAdvisedRegions);
            }
        } else {
            qWarning("The kernel denied reading the renderer page-size accounting");
        }
    }

    void boundedEditorSurroundingPreservesCompositionAndSelection() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(800, 600);
        view.attach(window.contentItem());
        window.show();
        view.load(url("large-editor"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("large-editor"), 15000);
        QVERIFY(evaluate(view, R"JS(
window.originalText='a'.repeat(5000)+'😀'+'X'.repeat(5000);
document.body.innerHTML='<div id="large" contenteditable style="width:600px;height:400px;overflow:auto;white-space:pre-wrap;word-break:break-all"></div>';
var editor=document.getElementById('large');
editor.textContent=originalText;
editor.focus();
getSelection().setPosition(editor.firstChild,5002);
true
)JS")
                    .toBool());
        view.surface()->forceActiveFocus();
        QTRY_VERIFY(view.surface()->inputMethodQuery(Qt::ImEnabled).toBool());
        const auto surrounding = [&] {
            return view.surface()->inputMethodQuery(Qt::ImSurroundingText).toString();
        };
        const auto cursor = [&] {
            return view.surface()->inputMethodQuery(Qt::ImCursorPosition).toInt();
        };
        QTRY_VERIFY(surrounding().left(cursor()).endsWith(QString::fromUtf8("😀")));
        const int limit = qEnvironmentVariableIntValue("WEBKIT_EDEN_EDITOR_WINDOW");
        if (limit >= 128) {
            QVERIFY(surrounding().size() <= 3 * limit + 2);
            QVERIFY(cursor() >= 2 && cursor() <= limit + 1);
        } else {
            QCOMPARE(surrounding().size(), 10002);
        }
        QVERIFY(!surrounding().front().isLowSurrogate());
        QVERIFY(!surrounding().back().isHighSurrogate());
        QInputMethodEvent preedit(QString::fromUtf8("日本"), {});
        QCoreApplication::sendEvent(view.surface(), &preedit);
        QTRY_VERIFY(evaluate(view, "document.getElementById('large').textContent.includes('日本')").toBool());
        QVERIFY(!surrounding().contains(QString::fromUtf8("日本")));
        QInputMethodEvent commit;
        commit.setCommitString(QString::fromUtf8("日本"));
        QCoreApplication::sendEvent(view.surface(), &commit);
        QTRY_VERIFY(
            evaluate(view, "document.getElementById('large').textContent==='a'.repeat(5000)+'😀日本'+'X'.repeat(5000)")
                .toBool()
        );
        QVERIFY(evaluate(view, R"JS(
var editor=document.getElementById('large');
editor.textContent=originalText;
var range=document.createRange();range.setStart(editor.firstChild,4998);range.setEnd(editor.firstChild,5004);
getSelection().removeAllRanges();getSelection().addRange(range);true
)JS")
                    .toBool());
        QTRY_COMPARE(view.surface()->inputMethodQuery(Qt::ImCurrentSelection).toString(), QString::fromUtf8("aa😀XX"));
        QInputMethodEvent replace;
        replace.setCommitString("Z");
        QCoreApplication::sendEvent(view.surface(), &replace);
        QTRY_VERIFY(
            evaluate(view, "document.getElementById('large').textContent==='a'.repeat(4998)+'Z'+'X'.repeat(4998)")
                .toBool()
        );
    }

    void editorWindowPreservesLargeSelectionsAndDeletion() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(800, 600);
        view.attach(window.contentItem());
        window.show();
        view.load(url("editor-deletion"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("editor-deletion"), 15000);
        QVERIFY(evaluate(view, R"JS(
(() => {
    document.body.innerHTML='<textarea id="editor" style="width:600px;height:400px"></textarea>';
    const editor=document.getElementById('editor');
    editor.value='left\n'.repeat(1000)+'😀'+'right\n'.repeat(1000);
    editor.focus();editor.setSelectionRange(5002,5002);return true;
})()
)JS")
                    .toBool());
        view.surface()->forceActiveFocus();
        const auto surrounding = [&] {
            return view.surface()->inputMethodQuery(Qt::ImSurroundingText).toString();
        };
        const auto cursor = [&] {
            return view.surface()->inputMethodQuery(Qt::ImCursorPosition).toInt();
        };
        QTRY_VERIFY(view.surface()->inputMethodQuery(Qt::ImEnabled).toBool());
        QTRY_VERIFY(surrounding().left(cursor()).endsWith(QString::fromUtf8("😀")));
        QInputMethodEvent deletion;
        deletion.setCommitString(QString(), -2, 2);
        QCoreApplication::sendEvent(view.surface(), &deletion);
        QTRY_VERIFY(
            evaluate(view, "document.getElementById('editor').value==='left\\n'.repeat(1000)+'right\\n'.repeat(1000)")
                .toBool()
        );
        QVERIFY(evaluate(view, "document.getElementById('editor').select();true").toBool());
        QTRY_COMPARE(view.surface()->inputMethodQuery(Qt::ImCurrentSelection).toString().size(), 11000);
        QInputMethodEvent replacement;
        replacement.setCommitString(QString::fromUtf8("日本"));
        QCoreApplication::sendEvent(view.surface(), &replacement);
        QTRY_COMPARE(evaluate(view, "document.getElementById('editor').value").toString(), QString::fromUtf8("日本"));
    }

    void sparseRepaintPreservesUntouchedPixels() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(800, 600);
        view.attach(window.contentItem());
        window.show();
        view.load(url("sparse-paint"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("sparse-paint"), 15000);
        QVERIFY(evaluate(view, R"JS(
document.body.style.cssText='margin:0;height:600px;background:white';
document.body.innerHTML='<div style="position:absolute;width:calc(100vw - 20px);height:calc(100vh - 20px);transform:translateZ(0);background:#20c040"><div id="first" style="position:absolute;left:10px;top:10px;width:30px;height:30px;background:#e03020;box-shadow:0 0 4px black"></div><div id="last" style="position:absolute;right:10px;bottom:10px;width:30px;height:30px;background:#e03020;box-shadow:0 0 4px black"></div><span style="position:absolute;left:250px;top:250px;font:20px sans-serif">Unchanged middle text</span></div>';true
)JS")
                    .toBool());
        const auto matches = [&](const QColor &color) {
            const auto frame = window.grabWindow();
            if (frame.isNull()) {
                return false;
            }
            const qreal scale = window.devicePixelRatio();
            for (const QPoint &point : {QPoint(20, 20), QPoint(window.width() - 50, window.height() - 50)}) {
                const QPoint pixel(qRound(point.x() * scale), qRound(point.y() * scale));
                if (!frame.rect().contains(pixel) || frame.pixelColor(pixel) != color) {
                    return false;
                }
            }
            return frame.pixelColor(qRound(400 * scale), qRound(400 * scale)) == QColor("#20c040");
        };
        QTRY_VERIFY_WITH_TIMEOUT(matches(QColor("#e03020")), 10000);
        for (const QString &color : {QString("#2040e0"), QString("#e03020"), QString("#2040e0")}) {
            QVERIFY(evaluate(
                        view,
                        "document.getElementById('first').style.background='" + color +
                            "';document.getElementById('last').style.background='" + color + "';true"
            )
                        .toBool());
            QTRY_VERIFY_WITH_TIMEOUT(matches(QColor(color)), 5000);
        }
    }

    void bulkTeardownRepaintsRemovedPixels() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(320, 240);
        view.attach(window.contentItem());
        window.show();
        view.load(url("bulk-teardown-pixels"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("bulk-teardown-pixels"), 15000);
        QVERIFY(evaluate(view, R"JS(
document.body.style.cssText='margin:0;background:white';
document.body.innerHTML='<div id="bulk" style="position:absolute;left:20px;top:20px;width:100px;height:100px;background:#2040e0"></div>';
const bulk=document.getElementById('bulk');
for(let index=0;index<10;index++){const row=document.createElement('div');row.style.cssText='height:10px;background:#20c040';bulk.append(row)}
true
)JS")
                    .toBool());
        const auto pixelMatches = [&](const QPoint &point, const QColor &color) {
            const QImage frame = window.grabWindow();
            const qreal scale = window.devicePixelRatio();
            const QPoint pixel(qRound(point.x() * scale), qRound(point.y() * scale));
            return !frame.isNull() && frame.rect().contains(pixel) && frame.pixelColor(pixel) == color;
        };
        QTRY_VERIFY_WITH_TIMEOUT(pixelMatches(QPoint(50, 50), QColor("#20c040")), 10000);
        QVERIFY(evaluate(view, "document.getElementById('bulk').replaceChildren();true").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(pixelMatches(QPoint(50, 50), QColor("#2040e0")), 10000);
        QVERIFY(evaluate(view, R"JS(
const escaped=document.createElement('div');
escaped.style.cssText='width:100px;height:100px;transform:translateX(90px);background:#e03020';
document.getElementById('bulk').append(escaped);
true
)JS")
                    .toBool());
        QTRY_VERIFY_WITH_TIMEOUT(pixelMatches(QPoint(160, 60), QColor("#e03020")), 10000);
        QVERIFY(evaluate(view, "document.getElementById('bulk').replaceChildren();true").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(pixelMatches(QPoint(160, 60), QColor(Qt::white)), 10000);
    }

    void listMarkersRenderExpectedValuesAcrossLifecycleChanges() {
        const bool hadRenumberingValue = qEnvironmentVariableIsSet("WEBKIT_EDEN_LIST_RENUMBERING");
        const QByteArray previousRenumberingValue = qgetenv("WEBKIT_EDEN_LIST_RENUMBERING");
        const bool hadDiagnosticsValue = qEnvironmentVariableIsSet("WEBKIT_EDEN_LIST_RENUMBERING_DIAGNOSTICS");
        const QByteArray previousDiagnosticsValue = qgetenv("WEBKIT_EDEN_LIST_RENUMBERING_DIAGNOSTICS");
        const bool hadVerificationValue = qEnvironmentVariableIsSet("WEBKIT_EDEN_VERIFY_LIST_RENUMBERING");
        const QByteArray previousVerificationValue = qgetenv("WEBKIT_EDEN_VERIFY_LIST_RENUMBERING");
        if (!hadRenumberingValue) {
            qputenv("WEBKIT_EDEN_LIST_RENUMBERING", "1");
        }
        qputenv("WEBKIT_EDEN_LIST_RENUMBERING_DIAGNOSTICS", "1");
        qputenv("WEBKIT_EDEN_VERIFY_LIST_RENUMBERING", "1");
        const auto restoreEnvironment = qScopeGuard([&] {
            if (hadRenumberingValue) {
                qputenv("WEBKIT_EDEN_LIST_RENUMBERING", previousRenumberingValue);
            } else {
                qunsetenv("WEBKIT_EDEN_LIST_RENUMBERING");
            }
            if (hadDiagnosticsValue) {
                qputenv("WEBKIT_EDEN_LIST_RENUMBERING_DIAGNOSTICS", previousDiagnosticsValue);
            } else {
                qunsetenv("WEBKIT_EDEN_LIST_RENUMBERING_DIAGNOSTICS");
            }
            if (hadVerificationValue) {
                qputenv("WEBKIT_EDEN_VERIFY_LIST_RENUMBERING", previousVerificationValue);
            } else {
                qunsetenv("WEBKIT_EDEN_VERIFY_LIST_RENUMBERING");
            }
        });

        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(800, 600);
        view.attach(window.contentItem());
        window.show();
        view.load(url("list-renumbering"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("list-renumbering"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(view.surface()->textureImportCount() > 0, 15000);

        QFile fixture(QFINDTESTDATA("wpe/list-renumbering.js"));
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        const QString fixtureSource = QString::fromUtf8(fixture.readAll());
        const QJsonArray initialExpected = QJsonDocument::fromJson(QByteArray(R"JSON(
            [["reverse-one","3. "],["reverse-two","2. "],["reverse-three","1. "],
             ["explicit-one","7. "],["explicit-value","12. "],["explicit-three","13. "],
             ["outer-one","4. "],["inner-one","1. "],["inner-two","2. "],["outer-two","5. "],
             ["css-one","21. "],["css-set","7. "],["css-three","8. "],
             ["style-item","23. "],["adopted-item","1. "]]
        )JSON"))
                                               .array();
        const QJsonArray removedExpected =
            QJsonDocument::fromJson(QByteArray(R"JSON([["reverse-one","2. "],["reverse-two","1. "]])JSON")).array();
        const QJsonArray reinsertedExpected =
            QJsonDocument::fromJson(
                QByteArray(R"JSON([["reverse-one","3. "],["reverse-two","2. "],["reverse-three","1. "]])JSON")
            )
                .array();
        const QJsonArray explicitStartExpected =
            QJsonDocument::fromJson(
                QByteArray(R"JSON([["explicit-one","3. "],["explicit-value","12. "],["explicit-three","13. "]])JSON")
            )
                .array();
        const QJsonArray recreatedStyleExpected =
            QJsonDocument::fromJson(QByteArray(R"JSON([["style-item","23. "]])JSON")).array();
        const QJsonArray adoptedExpected =
            QJsonDocument::fromJson(QByteArray(R"JSON([["adopted-item","1. "]])JSON")).array();
        QCOMPARE(initialExpected.size(), qsizetype(15));
        QCOMPARE(removedExpected.size(), qsizetype(2));
        QCOMPARE(reinsertedExpected.size(), qsizetype(3));
        QCOMPARE(explicitStartExpected.size(), qsizetype(3));
        QCOMPARE(recreatedStyleExpected.size(), qsizetype(1));
        QCOMPARE(adoptedExpected.size(), qsizetype(1));

        const auto phaseScript = [](const QString &action, const QJsonArray &expected, const QColor &fence) {
            return action +
                   QStringLiteral("window.edenListRenumberingRegression.snapshot(%1,'%2')")
                       .arg(QString::fromUtf8(QJsonDocument(expected).toJson(QJsonDocument::Compact)), fence.name());
        };
        struct MarkerSnapshot {
            bool passed = false;
            QImage frame;
            QString error;
        };
        bool fixtureLoaded = false;
        const auto runPhase = [&](const QString &action, const QJsonArray &expected, const QColor &fence) {
            const QString source = fixtureLoaded ? QString() : fixtureSource;
            fixtureLoaded = true;
            const QString script = source + phaseScript(action, expected, fence);
            const QString result = evaluate(view, script).toString();
            const QJsonDocument geometry = QJsonDocument::fromJson(result.toUtf8());
            if (!geometry.isArray() || geometry.array().size() != expected.size()) {
                return MarkerSnapshot{false, {}, "The fixture returned invalid marker geometry"};
            }

            const qreal scale = window.devicePixelRatio();
            const qreal referenceOffset = 320 * scale;
            if (qAbs(referenceOffset - qRound(referenceOffset)) > 0.0001) {
                return MarkerSnapshot{false, {}, "The reference offset does not preserve device-pixel alignment"};
            }
            const QPoint fencePoint(qRound(772 * scale), qRound(20 * scale));
            const auto frameHasFence = [&](const QImage &frame) {
                return !frame.isNull() && frame.rect().contains(fencePoint) && frame.pixelColor(fencePoint) == fence;
            };
            QImage frame;
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < 10000) {
                frame = window.grabWindow();
                if (frameHasFence(frame)) {
                    break;
                }
                QTest::qWait(5);
            }
            if (!frameHasFence(frame)) {
                return MarkerSnapshot{false, {}, "The compositor did not present the current phase fence"};
            }

            const auto inkPixels = [](const QImage &image) {
                int count = 0;
                for (int y = 0; y < image.height(); ++y) {
                    for (int x = 0; x < image.width(); ++x) {
                        const QColor pixel = image.pixelColor(x, y);
                        if (pixel.red() < 80 && pixel.green() < 90 && pixel.blue() < 100) {
                            ++count;
                        }
                    }
                }
                return count;
            };
            const int cropWidth = qRound(96 * scale);
            const int cropHeight = qRound(28 * scale);
            for (qsizetype index = 0; index < expected.size(); ++index) {
                const QJsonObject item = geometry.array().at(index).toObject();
                const QPoint actualOrigin(
                    qRound(item.value("actualX").toDouble() * scale),
                    qRound(item.value("actualY").toDouble() * scale)
                );
                const QPoint referenceOrigin(
                    qRound(item.value("referenceX").toDouble() * scale),
                    qRound(item.value("referenceY").toDouble() * scale)
                );
                const QRect actualRect(actualOrigin, QSize(cropWidth, cropHeight));
                const QRect referenceRect(referenceOrigin, QSize(cropWidth, cropHeight));
                if (!frame.rect().contains(actualRect) || !frame.rect().contains(referenceRect)) {
                    return MarkerSnapshot{
                        false,
                        {},
                        QStringLiteral("The marker crop for %1 is outside the captured frame")
                            .arg(item.value("id").toString())
                    };
                }
                const QImage actual = frame.copy(actualRect);
                const QImage reference = frame.copy(referenceRect);
                if (!inkPixels(actual) || !inkPixels(reference)) {
                    return MarkerSnapshot{
                        false,
                        {},
                        QStringLiteral("The actual or fixed reference marker for %1 is blank")
                            .arg(item.value("id").toString())
                    };
                }
                if (actual != reference) {
                    return MarkerSnapshot{
                        false,
                        {},
                        QStringLiteral("The rendered marker for %1 differs from its fixed reference")
                            .arg(item.value("id").toString())
                    };
                }
            }
            return MarkerSnapshot{true, frame, {}};
        };
        const auto blankRegionHasNoGlyphs = [&](const QImage &frame, const QPointF &origin) {
            const qreal scale = window.devicePixelRatio();
            const QRect
                region(qRound(origin.x() * scale), qRound(origin.y() * scale), qRound(96 * scale), qRound(28 * scale));
            if (frame.isNull() || !frame.rect().contains(region)) {
                return false;
            }
            for (int y = region.top(); y < region.top() + region.height(); ++y) {
                for (int x = region.left(); x < region.left() + region.width(); ++x) {
                    const QColor pixel = frame.pixelColor(x, y);
                    if (pixel.red() < 80 && pixel.green() < 90 && pixel.blue() < 100) {
                        return false;
                    }
                }
            }
            return true;
        };

        auto result = runPhase(
            QStringLiteral("window.edenListRenumberingRegression.initialize();"),
            initialExpected,
            QColor("#b03020")
        );
        QVERIFY2(result.passed, qPrintable(result.error));

        result = runPhase(
            QStringLiteral("window.edenListRenumberingRegression.removeReversedTail();"),
            removedExpected,
            QColor("#208040")
        );
        QVERIFY2(result.passed, qPrintable(result.error));

        result = runPhase(
            QStringLiteral("window.edenListRenumberingRegression.reinsertReversedTail();"),
            reinsertedExpected,
            QColor("#2040b0")
        );
        QVERIFY2(result.passed, qPrintable(result.error));

        result = runPhase(
            QStringLiteral("window.edenListRenumberingRegression.changeExplicitStart();"),
            explicitStartExpected,
            QColor("#a040b0")
        );
        QVERIFY2(result.passed, qPrintable(result.error));

        const QString hiddenStyleGeometry =
            evaluate(view, "window.edenListRenumberingRegression.hideStyleItem()").toString();
        const QJsonObject hiddenStylePosition = QJsonDocument::fromJson(hiddenStyleGeometry.toUtf8()).object();
        QVERIFY(!hiddenStylePosition.isEmpty());
        result = runPhase(QStringLiteral(""), QJsonArray(), QColor("#c08020"));
        QVERIFY2(result.passed, qPrintable(result.error));
        QVERIFY2(
            blankRegionHasNoGlyphs(
                result.frame,
                QPointF(hiddenStylePosition.value("x").toDouble(), hiddenStylePosition.value("y").toDouble())
            ),
            "A marker remained painted after its renderer was destroyed"
        );

        result = runPhase(
            QStringLiteral("window.edenListRenumberingRegression.showStyleItem();"),
            recreatedStyleExpected,
            QColor("#8040c0")
        );
        QVERIFY2(result.passed, qPrintable(result.error));

        const QString sourceGeometry =
            evaluate(view, "window.edenListRenumberingRegression.adoptSourceItem()").toString();
        const QJsonObject sourcePosition = QJsonDocument::fromJson(sourceGeometry.toUtf8()).object();
        QVERIFY(!sourcePosition.isEmpty());
        result = runPhase(QStringLiteral(""), adoptedExpected, QColor("#208080"));
        QVERIFY2(result.passed, qPrintable(result.error));
        QVERIFY2(
            blankRegionHasNoGlyphs(
                result.frame,
                QPointF(sourcePosition.value("x").toDouble(), sourcePosition.value("y").toDouble())
            ),
            "The source document retained a painted marker after adoption"
        );
    }

    void opportunisticYieldPreservesIdleCallbacksAndLifecycle() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        WebKitFeatureList *features = webkit_settings_get_all_features();
        QVERIFY(features);
        const auto releaseFeatures = qScopeGuard([&] { webkit_feature_list_unref(features); });
        auto *idleCallbackFeature = webkit_feature_list_find(features, "RequestIdleCallback");
        QVERIFY(idleCallbackFeature);
        webkit_settings_set_feature_enabled(webkit_web_view_get_settings(view.nativeView()), idleCallbackFeature, TRUE);
        QQuickWindow window;
        window.resize(800, 600);
        view.attach(window.contentItem());
        window.show();
        view.load(url("opportunistic-yield"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("opportunistic-yield"), 15000);
        QFile fixture(QFINDTESTDATA("wpe-opportunistic-yield.js"));
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        QVERIFY(evaluate(view, QString::fromUtf8(fixture.readAll()) + R"JS(
;window.edenYieldResult={done:false};
edenDOMProbe().then(result=>window.edenYieldResult={done:true,result},
    error=>window.edenYieldResult={done:true,error:String(error)+'\n'+String(error.stack||'')});
true
)JS")
                    .toBool());
        QTRY_VERIFY_WITH_TIMEOUT(evaluate(view, "edenYieldResult.done").toBool(), 120000);
        const auto completion = evaluate(view, "edenYieldResult").toMap();
        QVERIFY2(!completion.contains("error"), qPrintable(completion.value("error").toString()));
        const auto result = completion.value("result").toMap();
        QCOMPARE(result.value("probe").toString(), QStringLiteral("opportunistic-yield"));
        QCOMPARE(result.value("visibility").toString(), QStringLiteral("visible"));
        const auto measurements = result.value("measurements").toMap();
        const auto assertions = measurements.value("assertions").toList();
        QVERIFY(!assertions.isEmpty());
        for (const auto &assertion : assertions) {
            const auto record = assertion.toMap();
            QVERIFY2(record.value("passed").toBool(), qPrintable(record.value("name").toString()));
        }
        qInfo().noquote() << "EDEN_OPPORTUNISTIC_YIELD_RESULT|"
                          << QJsonDocument::fromVariant(result).toJson(QJsonDocument::Compact);
    }

    void layoutPhase1PatchesPreserveTextAndLayoutBehavior() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(800, 600);
        view.attach(window.contentItem());
        window.show();
        view.load(url("layout-phase1"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("layout-phase1"), 15000);
        QFile fixture(QFINDTESTDATA("wpe/layout-phase1.js"));
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        QVERIFY(evaluate(view, QString::fromUtf8(fixture.readAll()) + ";true").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(evaluate(view, "layoutPhase1RegressionResult.done").toBool(), 30000);
        const auto result = evaluate(view, "layoutPhase1RegressionResult").toMap();
        QVERIFY2(result.value("error").toString().isEmpty(), qPrintable(result.value("error").toString()));
        QVERIFY(result.value("assertions").toInt() >= 25);
    }

    void fontReusePreservesShapingAcrossFontChanges() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(800, 600);
        view.attach(window.contentItem());
        window.show();
        view.load(url("font-state"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("font-state"), 15000);
        QCOMPARE(
            evaluate(view, R"JS(
(() => {
    const canvas=document.createElement('canvas');canvas.width=800;canvas.height=150;
    const context=canvas.getContext('2d',{willReadFrequently:true});
    const strings=['office AVATAR ffi','العربية نص تجريبي','עברית בדיקה','日本語 😀 café','a\u0301 e\u0308'];
    const fonts=['12px serif','bold 28px serif','italic 19px sans-serif','40px monospace','11px sans-serif'];
    const cases=fonts.flatMap(font=>strings.map(text=>({font,text})));
    function render(item) {
        context.clearRect(0,0,800,150);context.font=item.font;context.direction='ltr';
        const metric=context.measureText(item.text);
        context.fillText(item.text,30,80);
        const pixels=context.getImageData(0,0,800,150).data;
        let hash=2166136261;
        for (const value of pixels) hash=Math.imul(hash^value,16777619);
        return JSON.stringify([metric.width,metric.actualBoundingBoxLeft,metric.actualBoundingBoxRight,metric.actualBoundingBoxAscent,metric.actualBoundingBoxDescent,hash]);
    }
    const expected=cases.map(render);
    for (let pass=0;pass<3;pass++) {
        for (let index=cases.length-1;index>=0;index--) {
            if(render(cases[index])!==expected[index]) return 'changed font state at '+index;
            if(render(cases[index])!==expected[index]) return 'changed repeated font state at '+index;
        }
    }
    return 'ok';
})()
)JS")
                .toString(),
            QString("ok")
        );
    }

    void fontRenderingFollowsTheSharedPolicy() {
        WPESettings *settings = wpe_display_get_settings(WpeRuntime::display());
        QVERIFY(settings);
        GError *error = nullptr;
        QCOMPARE(
            bool(wpe_settings_get_boolean(settings, WPE_SETTING_FONT_ANTIALIAS, &error)),
            FontPolicy::antialiasing()
        );
        QVERIFY(!error);
        QCOMPARE(
            wpe_settings_get_byte(settings, WPE_SETTING_FONT_SUBPIXEL_LAYOUT, &error),
            guint8(WPE_SETTINGS_SUBPIXEL_LAYOUT_NONE)
        );
        QVERIFY(!error);
        QCOMPARE(
            wpe_settings_get_byte(settings, WPE_SETTING_FONT_HINTING_STYLE, &error),
            guint8(WPE_SETTINGS_HINTING_STYLE_SLIGHT)
        );
        QVERIFY(!error);
        QCOMPARE(
            QString::fromUtf8(wpe_settings_get_string(settings, WPE_SETTING_FONT_NAME, &error)),
            FontPolicy::systemDescription()
        );
        QVERIFY(!error);

        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(800, 600);
        view.attach(window.contentItem());
        window.show();
        view.load(url("font-policy"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("font-policy"), 15000);
        const auto quoted = [](const QString &family) {
            return QLatin1Char('\'') + QString(family).replace(QLatin1Char('\''), QLatin1String("\\'")) +
                   QLatin1Char('\'');
        };
        const QString script = QStringLiteral(R"JS((() => {
    const measure = family => {
        const context = document.createElement('canvas').getContext('2d');
        context.font = '16px ' + family;
        return context.measureText('Handgloves 0123456789 WMil').width;
    };
    const generics = [['sans-serif', %1], ['serif', %2], ['monospace', %3]];
    const mismatched = generics.filter(pair => measure(pair[0]) !== measure(pair[1])).map(pair => pair[0]);
    const styleOf = family => {
        const probe = document.createElement('span');
        probe.style.fontFamily = family;
        document.body.appendChild(probe);
        const resolved = getComputedStyle(probe).fontFamily;
        probe.remove();
        return resolved;
    };
    return mismatched.length ? 'mismatched ' + mismatched.join(',') + ' ' + styleOf('sans-serif') : 'ok';
})())JS")
                                   .arg(
                                       quoted(FontPolicy::sansSerifFamily()),
                                       quoted(FontPolicy::serifFamily()),
                                       quoted(FontPolicy::monospaceFamily())
                                   );
        QCOMPARE(evaluate(view, script).toString(), QString("ok"));
    }

    void displaySyncTracksScreenAndStopsWhenHidden() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(800, 600);
        view.attach(window.contentItem());
        window.show();
        view.load(url("display-sync"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("display-sync"), 15000);
        if (qEnvironmentVariable("EDEN_WPE_DISPLAY_SYNC") != "0") {
            QCOMPARE(wpe_display_get_n_screens(WpeRuntime::display()), guint(QGuiApplication::screens().size()));
            auto *screen = wpe_toplevel_get_screen(wpe_view_get_toplevel(view.surface()->nativeView()));
            QVERIFY(screen);
            QCOMPARE(wpe_screen_get_refresh_rate(screen), qMax(1000, qRound(window.screen()->refreshRate() * 1000)));
        }
        QVERIFY(evaluate(
                    view,
                    "window.testFrames=0;window.keepRunning=true;function "
                    "frame(){testFrames++;if(keepRunning)requestAnimationFrame(frame)}requestAnimationFrame(frame);true"
        )
                    .toBool());
        QTRY_VERIFY_WITH_TIMEOUT(evaluate(view, "testFrames").toInt() >= 5, 5000);
        if (qEnvironmentVariable("EDEN_WPE_DISPLAY_SYNC") != "0") {
            QTRY_VERIFY_WITH_TIMEOUT(WpeRuntime::displayPulseCount() >= 5, 5000);
        }
        window.hide();
        QTest::qWait(500);
        const auto before = evaluate(view, "testFrames").toInt();
        QTest::qWait(200);
        QCOMPARE(evaluate(view, "testFrames").toInt(), before);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(evaluate(view, "testFrames").toInt() > before, 5000);
        QVERIFY(evaluate(view, "keepRunning=false;true").toBool());
        QTest::qWait(1500);
        QSignalSpy swaps(&window, &QQuickWindow::frameSwapped);
        const auto pulses = WpeRuntime::displayPulseCount();
        QTest::qWait(250);
        QCOMPARE(swaps.size(), 0);
        QCOMPARE(WpeRuntime::displayPulseCount(), pulses);
    }

    void displaySyncMovesBetweenWindows() {
        WpeProfile profile(privateParameters());
        WpeEngineView first(&profile);
        WpeEngineView second(&profile);
        QQuickWindow firstWindow;
        QQuickWindow secondWindow;
        firstWindow.resize(800, 600);
        secondWindow.resize(800, 600);
        first.attach(firstWindow.contentItem());
        second.attach(secondWindow.contentItem());
        firstWindow.show();
        first.load(url("first-display"));
        second.load(url("second-display"));
        QTRY_COMPARE_WITH_TIMEOUT(first.title(), QString("first-display"), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(second.title(), QString("second-display"), 15000);
        const QString animation = "window.testFrames=0;function "
                                  "tick(){testFrames++;requestAnimationFrame(tick)}requestAnimationFrame(tick);true";
        QVERIFY(evaluate(first, animation).toBool());
        QVERIFY(evaluate(second, animation).toBool());
        QTRY_VERIFY_WITH_TIMEOUT(evaluate(first, "testFrames").toInt() >= 5, 5000);
        for (int cycle = 0; cycle < 3; cycle++) {
            const int secondBefore = evaluate(second, "testFrames").toInt();
            secondWindow.show();
            firstWindow.hide();
            QTRY_VERIFY_WITH_TIMEOUT(evaluate(second, "testFrames").toInt() >= secondBefore + 5, 5000);
            const int firstBefore = evaluate(first, "testFrames").toInt();
            firstWindow.show();
            secondWindow.hide();
            QTRY_VERIFY_WITH_TIMEOUT(evaluate(first, "testFrames").toInt() >= firstBefore + 5, 5000);
        }
        second.attach(nullptr);
        const int before = evaluate(first, "testFrames").toInt();
        QTRY_VERIFY_WITH_TIMEOUT(evaluate(first, "testFrames").toInt() >= before + 5, 5000);
    }

    void canvasThreadPixelsAndContextLifecycle() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        WebKitFeatureList *features = webkit_settings_get_all_features();
        QVERIFY(features);
        auto *testingFeature = webkit_feature_list_find(features, "DOMTestingAPIs");
        QVERIFY(testingFeature);
        webkit_settings_set_feature_enabled(webkit_web_view_get_settings(view.nativeView()), testingFeature, TRUE);
        webkit_feature_list_unref(features);
        QQuickWindow window;
        window.resize(640, 480);
        view.attach(window.contentItem());
        window.show();
        view.load(url("canvas-thread-regression"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("canvas-thread-regression"), 15000);
        const bool available =
            evaluate(
                view,
                "typeof document.createElement('canvas').getContext('2d').isCanvasThreadForTesting === 'function'"
            )
                .toBool();
        if (!available) {
            QVERIFY2(
                !qEnvironmentVariableIsSet("EDEN_WPE_REQUIRE_PHASE1"),
                "The selected runtime lacks the canvas thread patch"
            );
            QSKIP("Requires the canvas thread patch");
        }
        QFile fixture(QFINDTESTDATA("wpe/canvas-thread.js"));
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        QVERIFY(evaluate(view, QString::fromUtf8(fixture.readAll()) + ";true").toBool());
        const auto disabled = qgetenv("WEBKIT_EDEN_DISABLE_CANVAS_THREAD");
        const bool expectedThreaded = disabled.isEmpty() || disabled == "0";
        QVERIFY(evaluate(
                    view,
                    QString(
                        "window.canvasThreadPresentationReady=false;window.canvasThreadResult={done:false};"
                        "canvasThreadRegression(%1).then(result=>"
                        "canvasThreadResult={done:true,result},error=>canvasThreadResult={done:true,error:String(error)"
                        "});true"
                    )
                        .arg(expectedThreaded ? "true" : "false")
        )
                    .toBool());
        QTRY_VERIFY_WITH_TIMEOUT(
            evaluate(view, "canvasThreadPresentationReady || canvasThreadResult.done").toBool(),
            90000
        );
        QVERIFY2(
            evaluate(view, "canvasThreadPresentationReady").toBool(),
            qPrintable(evaluate(view, "canvasThreadResult.error").toString())
        );
        const int placeholderPixelX = qRound(4 * window.devicePixelRatio());
        const int placeholderPixelY = qRound(4 * window.devicePixelRatio());
        const auto hasExpectedPlaceholderPixel = [placeholderPixelX, placeholderPixelY](const QImage &image) {
            if (image.isNull()) {
                return false;
            }
            const QColor pixel = image.pixelColor(placeholderPixelX, placeholderPixelY);
            return pixel.red() == 209 && pixel.green() == 40 && pixel.blue() == 180;
        };
        QImage presented;
        QTRY_VERIFY_WITH_TIMEOUT((presented = window.grabWindow(), hasExpectedPlaceholderPixel(presented)), 15000);
        QVERIFY(evaluate(view, "canvasThreadResume();true").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(evaluate(view, "canvasThreadResult.done").toBool(), 90000);
        QImage retainedPresentation;
        QTRY_VERIFY_WITH_TIMEOUT(
            (retainedPresentation = window.grabWindow(), hasExpectedPlaceholderPixel(retainedPresentation)),
            15000
        );
        const auto completion = evaluate(view, "canvasThreadResult").toMap();
        QVERIFY2(!completion.contains("error"), qPrintable(completion.value("error").toString()));
        const auto result = completion.value("result").toMap();
        qInfo(
            "Canvas checks: document=%d worker=%d; WebGL uploads: document=%d worker=%d",
            result.value("checks").toInt(),
            result.value("workerChecks").toInt(),
            result.value("webglUploads").toInt(),
            result.value("workerWebglUploads").toInt()
        );
        QCOMPARE(result.value("webglUploads").toInt(), 2);
        const int workerWebglUploads = result.value("workerWebglUploads").toInt();
        QVERIFY(workerWebglUploads == 0 || workerWebglUploads == 2);
        if (!workerWebglUploads) {
            qInfo("Worker WebGL uploads unavailable: OffscreenCanvas.getContext(webgl) returned null");
        }
        QVERIFY(result.value("checks").toInt() >= 30);
        QVERIFY2(
            result.value("workerChecks").toInt() >= 26,
            qPrintable(QString::number(result.value("workerChecks").toInt()))
        );
        QCOMPARE(result.value("lossSkipped").toBool(), !expectedThreaded);
        if (expectedThreaded) {
            QCOMPARE(result.value("lost").toInt(), 1);
            QCOMPARE(result.value("canceled").toInt(), 1);
            const auto workerLoss = result.value("workerLoss").toMap();
            QCOMPARE(workerLoss.value("lost").toInt(), 1);
            QCOMPARE(workerLoss.value("canceled").toInt(), 1);
        }
    }

    void canvasReplayPreservesPixelsAndReadbackOrder() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(640, 480);
        view.attach(window.contentItem());
        window.show();
        view.load(url("canvas-readback"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("canvas-readback"), 15000);
        const auto result = evaluate(view, R"JS((() => {
            const failures = [];
            const canvas = document.createElement('canvas');
            document.body.replaceChildren(canvas);
            const reset = () => {
                canvas.width = 256;
                canvas.height = 256;
                return canvas.getContext('2d');
            };
            const fill = (context, color) => {
                context.fillStyle = color;
                for (let i = 0; i < 2048; ++i) {
                    context.fillRect(i % 16 * 16, Math.floor(i / 16) % 16 * 16, 16, 16);
                }
            };
            const check = (context, x, y, expected, label) => {
                const actual = Array.from(context.getImageData(x, y, 1, 1).data);
                if (actual.some((value, index) => Math.abs(value - expected[index]) > 1)) {
                    failures.push(label + ': ' + actual.join(','));
                }
            };
            let context = reset();
            fill(context, 'rgb(20,40,60)');
            context.save();
            context.beginPath();
            context.rect(16, 16, 96, 96);
            context.clip();
            context.translate(32, 32);
            context.save();
            context.beginPath();
            context.rect(0, 0, 64, 64);
            context.clip();
            for (let i = 0; i < 1024; ++i) {
                context.save();
                context.translate(i % 8 * 8, Math.floor(i / 8) % 8 * 8);
                context.fillStyle = 'rgb(200,30,40)';
                context.fillRect(0, 0, 8, 8);
                context.restore();
            }
            context.restore();
            context.restore();
            context.fillStyle = 'rgb(30,210,50)';
            context.fillRect(4, 4, 4, 4);
            check(context, 40, 40, [200,30,40,255], 'nested clips and transforms');
            check(context, 120, 120, [20,40,60,255], 'clip outside');
            check(context, 5, 5, [30,210,50,255], 'restore after batches');
            context = reset();
            fill(context, 'rgb(20,40,60)');
            context.globalAlpha = 0.5;
            context.fillStyle = 'rgb(200,0,0)';
            context.fillRect(0, 0, 256, 256);
            check(context, 100, 100, [110,20,30,255], 'alpha order');
            context = reset();
            fill(context, 'rgb(40,80,120)');
            const pixels = new ImageData(new Uint8ClampedArray([3,5,7,255]), 1, 1);
            context.putImageData(pixels, 10, 10);
            check(context, 10, 10, [3,5,7,255], 'write after queued draws');
            check(context, 11, 10, [40,80,120,255], 'write preserves neighbours');
            context = reset();
            fill(context, 'rgb(20,40,60)');
            context.fillStyle = 'rgb(30,210,50)';
            context.fillRect(0, 0, 64, 64);
            context.drawImage(canvas, 0, 0, 64, 64, 128, 128, 64, 64);
            check(context, 140, 140, [30,210,50,255], 'self image snapshot ordering');
            const source = document.createElement('canvas');
            source.width = 256;
            source.height = 256;
            const sourceContext = source.getContext('2d');
            fill(sourceContext, 'rgb(170,60,190)');
            context = reset();
            fill(context, 'rgb(20,40,60)');
            context.drawImage(source, 0, 0);
            check(context, 140, 140, [170,60,190,255], 'two canvas ordering');
            context = reset();
            fill(context, 'rgb(20,40,60)');
            context.fillStyle = context.createPattern(source, 'repeat');
            context.fillRect(0, 0, 256, 256);
            check(context, 140, 140, [170,60,190,255], 'image pattern fallback');
            context = reset();
            fill(context, 'rgb(20,40,60)');
            context.filter = 'blur(1px)';
            context.fillStyle = 'rgb(30,210,50)';
            context.fillRect(32, 32, 128, 128);
            context.filter = 'none';
            check(context, 80, 80, [30,210,50,255], 'filter layer fallback');
            context = reset();
            fill(context, 'rgb(200,30,40)');
            canvas.width = 64;
            canvas.height = 64;
            check(canvas.getContext('2d'), 10, 10, [0,0,0,0], 'resize discards queued content');
            return {valid: true, failures};
        })())JS")
                                .toMap();
        QVERIFY(result.value("valid").toBool());
        QVERIFY2(
            result.value("failures").toStringList().isEmpty(),
            qPrintable(result.value("failures").toStringList().join("; "))
        );
    }

    void canvasReplayMatchesImmediateGpuPainting() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(640, 480);
        view.attach(window.contentItem());
        window.show();
        view.load(url("canvas-equivalence"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("canvas-equivalence"), 15000);
        const auto result = evaluate(view, R"JS((() => {
            const pictures = [];
            for (const immediate of [false, true]) {
                const canvas = document.createElement('canvas');
                canvas.width = 512;
                canvas.height = 256;
                const context = canvas.getContext('2d');
                if (immediate) {
                    context.fillRect(0, 0, 1, 1);
                    context.getImageData(0, 0, 1, 1);
                }
                context.fillStyle = 'rgb(20,40,60)';
                context.fillRect(0, 0, 512, 256);
                context.save();
                context.beginPath();
                context.rect(8, 8, 496, 240);
                context.clip();
                for (let index = 0; index < 768; ++index) {
                    context.save();
                    context.translate(index % 32 * 16 + 0.25, Math.floor(index / 32) % 16 * 16 + 0.5);
                    context.globalAlpha = 0.6;
                    context.fillStyle = index % 2 ? 'rgb(230,90,40)' : 'rgb(50,170,220)';
                    context.beginPath();
                    context.arc(8, 8, 7, 0, Math.PI * 2);
                    context.fill();
                    context.restore();
                }
                context.font = '18px sans-serif';
                context.fillStyle = 'white';
                for (let index = 0; index < 512; ++index) {
                    context.fillText('office ffi AV', index % 4 * 128, Math.floor(index / 4) % 12 * 20 + 18);
                }
                context.restore();
                pictures.push(context.getImageData(0, 0, 512, 256).data);
            }
            let differences = 0;
            let maximum = 0;
            for (let index = 0; index < pictures[0].length; ++index) {
                const difference = Math.abs(pictures[0][index] - pictures[1][index]);
                maximum = Math.max(maximum, difference);
                if (difference > 1) {
                    ++differences;
                }
            }
            return {valid: true, differences, maximum};
        })())JS")
                                .toMap();
        QVERIFY(result.value("valid").toBool());
        QVERIFY2(
            result.value("differences").toInt() == 0,
            qPrintable(QString("%1 channel differences, maximum %2")
                           .arg(result.value("differences").toInt())
                           .arg(result.value("maximum").toInt()))
        );
    }

    void canvasReplayPreservesSnapshotsAcrossFrames() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(640, 480);
        view.attach(window.contentItem());
        window.show();
        view.load(url("canvas-snapshots"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("canvas-snapshots"), 15000);
        QVERIFY(evaluate(view, R"JS((() => {
            window.snapshotResult = {done: false};
            (async () => {
                const source = document.createElement('canvas');
                const target = document.createElement('canvas');
                for (const canvas of [source, target]) {
                    canvas.width = 256;
                    canvas.height = 256;
                }
                document.body.replaceChildren(source, target);
                const context = source.getContext('2d');
                const draw = color => {
                    context.fillStyle = color;
                    for (let index = 0; index < 2048; ++index) {
                        context.fillRect(index % 16 * 16, Math.floor(index / 16) % 16 * 16, 16, 16);
                    }
                };
                draw('rgb(30,80,210)');
                const snapshot = await createImageBitmap(source);
                await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
                draw('rgb(200,30,40)');
                const targetContext = target.getContext('2d');
                targetContext.drawImage(snapshot, 0, 0);
                const saved = Array.from(targetContext.getImageData(40, 40, 1, 1).data);
                const current = Array.from(context.getImageData(40, 40, 1, 1).data);
                snapshot.close();
                window.snapshotResult = {done: true, saved, current};
            })().catch(error => { window.snapshotResult = {done: true, error: String(error)}; });
            return true;
        })())JS")
                    .toBool());
        QTRY_VERIFY_WITH_TIMEOUT(evaluate(view, "window.snapshotResult.done").toBool(), 15000);
        const auto result = evaluate(view, "window.snapshotResult").toMap();
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        QCOMPARE(result.value("saved").toList(), QVariantList({30, 80, 210, 255}));
        QCOMPARE(result.value("current").toList(), QVariantList({200, 30, 40, 255}));
    }

    void canvasReplayPresentsAndSurvivesPendingDestruction() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(640, 480);
        view.attach(window.contentItem());
        window.show();
        view.load(url("canvas-presentation"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("canvas-presentation"), 15000);
        QVERIFY(evaluate(view, R"JS((() => {
            document.body.style.cssText = 'margin:0;background:white';
            window.testCanvas = document.createElement('canvas');
            testCanvas.width = 640;
            testCanvas.height = 480;
            document.body.replaceChildren(testCanvas);
            window.drawCanvas = color => {
                const context = testCanvas.getContext('2d');
                context.fillStyle = color;
                for (let i = 0; i < 4096; ++i) {
                    context.fillRect(i % 32 * 20, Math.floor(i / 32) % 24 * 20, 20, 20);
                }
                context.font = '24px sans-serif';
                context.fillStyle = 'white';
                for (let i = 0; i < 512; ++i) {
                    context.fillText('Canvas', 300, 350);
                }
            };
            drawCanvas('rgb(30,210,50)');
            return true;
        })())JS")
                    .toBool());
        const auto visibleColor = [&] {
            const auto frame = window.grabWindow();
            if (frame.isNull()) {
                return QColor();
            }
            return frame.pixelColor(qRound(80 * window.devicePixelRatio()), qRound(80 * window.devicePixelRatio()));
        };
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(visibleColor().green() - 210) <= 2, 15000);
        QVERIFY(evaluate(view, "drawCanvas('rgb(200,30,40)');true").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(
            qAbs(visibleColor().red() - 200) <= 2 && qAbs(visibleColor().green() - 30) <= 2,
            15000
        );
        QVERIFY(evaluate(view, R"JS((() => {
            for (let round = 0; round < 12; ++round) {
                const canvas = document.createElement('canvas');
                canvas.width = 640;
                canvas.height = 480;
                const context = canvas.getContext('2d');
                for (let i = 0; i < 2048; ++i) {
                    context.beginPath();
                    context.arc(i % 640, i % 480, 12, 0, Math.PI * 2);
                    context.fill();
                }
                canvas.width = 0;
            }
            return true;
        })())JS")
                    .toBool());
        view.load(url("canvas-after-destruction"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("canvas-after-destruction"), 15000);
        QCOMPARE(evaluate(view, "6 * 7").toInt(), 42);
    }

    void texturePoolReusesImportsAndPresentsLatestFrame() {
        if (!qEnvironmentVariableIsSet("EDEN_WPE_REQUIRE_GPU")) {
            QSKIP("Requires GPU transport validation");
        }
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(640, 480);
        view.attach(window.contentItem());
        window.show();
        view.load(url("pool"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("pool"), 15000);
        QImage captured;
        QTRY_VERIFY_WITH_TIMEOUT(
            (captured = window.grabWindow(),
             !captured.isNull() &&
                 captured.pixelColor(qRound(50 * window.devicePixelRatio()), qRound(50 * window.devicePixelRatio()))
                         .green() > 150),
            15000
        );
        const auto imports = view.surface()->textureImportCount();
        for (int index = 0; index < 24; ++index) {
            const int red = index % 2 ? 220 : 40;
            evaluate(view, QString("document.querySelector('#box').style.background='rgb(%1,30,120)'").arg(red));
            QTRY_VERIFY_WITH_TIMEOUT(
                (captured = window.grabWindow(),
                 qAbs(
                     captured.pixelColor(qRound(50 * window.devicePixelRatio()), qRound(50 * window.devicePixelRatio()))
                         .red() -
                     red
                 ) < 5),
                15000
            );
            qInfo() << "pool step" << index << "imports" << view.surface()->textureImportCount() - imports << "frames" << view.surface()->frameImportCount() << "held peak" << WpeFrame::heldPeak();
        }
        QVERIFY(view.surface()->accelerated());
        QVERIFY2(
            view.surface()->textureImportCount() - imports <= 4,
            "GPU texture imports must remain bounded after the buffer pool warms up"
        );
    }

    void textureCacheFollowsTheLiveBufferSet() {
        if (!qEnvironmentVariableIsSet("EDEN_WPE_REQUIRE_GPU")) {
            QSKIP("Requires GPU transport validation");
        }
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(640, 480);
        view.attach(window.contentItem());
        window.show();
        view.load(url("pool"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("pool"), 15000);
        const auto paint = [&](int index) {
            const int red = index % 2 ? 220 : 40;
            evaluate(view, QString("document.querySelector('#box').style.background='rgb(%1,30,120)'").arg(red));
            QImage captured;
            QTRY_VERIFY_WITH_TIMEOUT(
                (captured = window.grabWindow(),
                 !captured.isNull() &&
                     qAbs(
                         captured.pixelColor(qRound(50 * window.devicePixelRatio()), qRound(50 * window.devicePixelRatio()))
                             .red() -
                         red
                     ) < 5),
                15000
            );
        };
        for (int index = 0; index < 12; ++index) {
            paint(index);
        }
        const auto warmed = view.surface()->textureImportCount();
        QVERIFY(view.surface()->accelerated());
        wpe_view_buffers_changed(view.surface()->nativeView(), nullptr, 0);
        for (int index = 0; index < 12; ++index) {
            paint(index);
        }
        QVERIFY2(
            view.surface()->textureImportCount() > warmed,
            "Textures of buffers WebKit no longer lists must be dropped and imported again when reused"
        );
    }

    void portableCookieAttributeMatrix() {
        WpeProfile profile(regularParameters(m_storage.path() + "/matrix", "matrix"));
        QList<PortableCookie> source;
        for (int index = 0; index < 12; ++index) {
            PortableCookie cookie;
            cookie.name = QByteArray("matrix-") + QByteArray::number(index);
            cookie.value = index == 0 ? QByteArray() : QByteArray("synthetic");
            cookie.domain = "example.com";
            cookie.path = index % 2 ? "/deep" : "/";
            cookie.hostOnly = index % 2;
            cookie.httpOnly = index % 3;
            cookie.secure = index % 4 != 0;
            cookie.sameSite = CookieSameSite(index % 4);
            if (index % 2) {
                cookie.expires = QDateTime::currentDateTimeUtc().addDays(2);
            }
            source.append(cookie);
        }
        PortableCookie invalid = source.first();
        invalid.name = "invalid;name";
        source.append(invalid);
        source.append(source.first());
        bool complete = false;
        CookieReplaceResult replaced;
        profile.replacePortableCookies(source, [&](CookieReplaceResult result) {
            replaced = result;
            complete = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(complete, 15000);
        QVERIFY2(replaced.errorCode.isEmpty(), qPrintable(replaced.errorCode));
        QCOMPARE(replaced.importedCookies, 12);
        QCOMPARE(replaced.skippedCookies, 2);
        CookieSnapshotResult snapshot;
        complete = false;
        profile.exportPortableCookies([&](CookieSnapshotResult result) {
            snapshot = std::move(result);
            complete = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(complete, 15000);
        QCOMPARE(snapshot.cookies.size(), 12);
        for (const auto &stored : snapshot.cookies) {
            const auto found = std::find_if(source.begin(), source.end(), [&](const PortableCookie &cookie) {
                return cookie.name == stored.name;
            });
            QVERIFY(found != source.end());
            QCOMPARE(stored.value, found->value);
            QCOMPARE(stored.hostOnly, found->hostOnly);
            QCOMPARE(stored.httpOnly, found->httpOnly);
            QCOMPARE(stored.secure, found->secure);
            QCOMPARE(stored.path, found->path);
            QCOMPARE(
                stored.sameSite,
                found->sameSite == CookieSameSite::Unspecified ? CookieSameSite::Lax : found->sameSite
            );
            QCOMPARE(stored.expires.has_value(), found->expires.has_value());
            if (stored.expires) {
                QCOMPARE(stored.expires->toSecsSinceEpoch(), found->expires->toSecsSinceEpoch());
            }
        }
    }

    void mixedBlinkWebKitCookieHandoff() {
#if EDEN_ENGINE_CEF
        QLibrary module(QCoreApplication::applicationDirPath() + "/eden-engine-cef");
        module.setLoadHints(QLibrary::ResolveAllSymbolsHint | QLibrary::PreventUnloadHint);
        QVERIFY2(module.load(), qPrintable(module.errorString()));
        const auto resolve = reinterpret_cast<ResolveEnginePlugin>(module.resolve("eden_engine_plugin"));
        QVERIFY(resolve);
        m_blinkApi = resolve();
        QByteArray executable = QCoreApplication::applicationFilePath().toUtf8();
        char *arguments[] = {executable.data()};
        const QByteArray root = m_storage.path().toUtf8();
        const EngineIdentity identity{"Eden", "0.4.0", root.constData()};
        QVERIFY(m_blinkApi->initialize(1, arguments, &identity));
        auto parameters = regularParameters(m_storage.path() + "/profiles/mixed-blink", "mixed");
        parameters.backend = Backend::Cef;
        parameters.dataPath = m_storage.path() + "/profiles/mixed-blink";
        QVERIFY(QDir().mkpath(parameters.dataPath));
        QVERIFY(QDir().mkpath(parameters.cachePath));
        std::unique_ptr<EngineProfile> blink(resolve()->createProfile(&parameters, nullptr, nullptr));
        QVERIFY(blink);
        WpeProfile webkit(regularParameters(m_storage.path() + "/mixed-webkit", "mixed"));
        std::unique_ptr<EngineView> blinkView(m_blinkApi->createView(blink.get()));
        std::unique_ptr<EngineView> webkitView = std::make_unique<WpeEngineView>(&webkit);
        QVERIFY(blinkView);
        QVERIFY(webkitView);
        QQuickWindow window;
        window.resize(900, 500);
        QQuickItem left(window.contentItem());
        left.setSize(QSizeF(450, 500));
        QQuickItem right(window.contentItem());
        right.setX(450);
        right.setSize(QSizeF(450, 500));
        blinkView->attach(&left);
        webkitView->attach(&right);
        window.show();
        blinkView->load(url("blink"));
        webkitView->load(url("webkit"));
        QTRY_COMPARE_WITH_TIMEOUT(blinkView->title(), QString("blink"), 20000);
        QTRY_COMPARE_WITH_TIMEOUT(webkitView->title(), QString("webkit"), 20000);
        PortableCookie cookie;
        cookie.name = "synthetic-session";
        cookie.value = "test-only";
        cookie.domain = "127.0.0.1";
        cookie.path = "/";
        cookie.hostOnly = true;
        cookie.httpOnly = true;
        cookie.sameSite = CookieSameSite::Lax;
        bool complete = false;
        CookieReplaceResult replaced;
        blink->replacePortableCookies({cookie}, [&](CookieReplaceResult result) {
            replaced = result;
            complete = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(complete, 15000);
        QVERIFY2(replaced.errorCode.isEmpty(), qPrintable(replaced.errorCode));
        CookieSnapshotResult exported;
        complete = false;
        blink->exportPortableCookies([&](CookieSnapshotResult result) {
            exported = std::move(result);
            complete = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(complete, 15000);
        QVERIFY(exported.errorCode.isEmpty());
        QCOMPARE(exported.cookies.size(), 1);
        complete = false;
        webkit.replacePortableCookies(exported.cookies, [&](CookieReplaceResult result) {
            replaced = result;
            complete = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(complete, 15000);
        QVERIFY2(replaced.errorCode.isEmpty(), qPrintable(replaced.errorCode));
        webkitView->load(url("cookie-check"));
        QTRY_COMPARE_WITH_TIMEOUT(webkitView->title(), QString("Authenticated"), 15000);
        blinkView->attach(nullptr);
        webkitView->attach(nullptr);
#endif
    }

    void persistentStorageFlushAndProfileIsolation() {
        QTemporaryDir directory;
        const auto parameters = regularParameters(directory.path(), "persistent");
        {
            WpeProfile profile(parameters);
            WpeEngineView view(&profile);
            view.load(url("storage"));
            QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("storage"), 15000);
            QVERIFY(evaluate(
                        view,
                        "localStorage.setItem('synthetic','persistent-value');document.cookie='persistent=test-only; "
                        "Max-Age=3600; SameSite=Lax';true"
            )
                        .toBool());
            bool flushed = false;
            profile.flushStorage([&] { flushed = true; });
            QTRY_VERIFY_WITH_TIMEOUT(flushed, 15000);
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        WpeProfile reopened(parameters);
        WpeEngineView restored(&reopened);
        restored.load(url("storage"));
        QTRY_COMPARE_WITH_TIMEOUT(restored.title(), QString("storage"), 15000);
        QCOMPARE(evaluate(restored, "localStorage.getItem('synthetic')").toString(), QString("persistent-value"));
        QVERIFY(evaluate(restored, "document.cookie").toString().contains("persistent=test-only"));
        WpeProfile isolated(regularParameters(directory.path() + "/isolated", "isolated"));
        WpeEngineView isolatedView(&isolated);
        isolatedView.load(url("storage"));
        QTRY_COMPARE_WITH_TIMEOUT(isolatedView.title(), QString("storage"), 15000);
        QVERIFY(evaluate(isolatedView, "localStorage.getItem('synthetic')").toString().isEmpty());
        QVERIFY(evaluate(isolatedView, "document.cookie").toString().isEmpty());
        WpeProfile ephemeral(privateParameters());
        WpeEngineView privateView(&ephemeral);
        privateView.load(url("storage"));
        QTRY_COMPARE_WITH_TIMEOUT(privateView.title(), QString("storage"), 15000);
        QVERIFY(evaluate(privateView, "localStorage.getItem('synthetic')").toString().isEmpty());
        QVERIFY(evaluate(privateView, "document.cookie").toString().isEmpty());
    }

    void crossOriginFrameDialogIdentity() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        if (!(view.capabilities() & EngineView::DockedDevtools)) {
            QSKIP("Requires the pinned embedding patch");
        }
        QSignalSpy dialogs(&view, &EngineView::javaScriptDialogRequested);
        view.load(url("parent"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("parent"), 15000);
        auto child = url("frame-dialog");
        child.setHost("localhost");
        evaluate(
            view,
            QString("const frame=document.createElement('iframe');frame.src='%1';document.body.append(frame);true")
                .arg(child.toString())
        );
        QTRY_COMPARE_WITH_TIMEOUT(dialogs.size(), 1, 15000);
        const auto dialog = qvariant_cast<JavaScriptDialogInfo>(dialogs.first().first());
        QCOMPARE(dialog.origin, autofillOrigin(child));
        view.resolveJavaScriptDialog(dialog.id, true, {});
    }

    void rendererTerminationAndReload() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        view.load(url("before-crash"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("before-crash"), 15000);
        QTRY_VERIFY(!view.isLoading());
        webkit_web_view_terminate_web_process(view.nativeView());
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("Page unavailable"), 15000);
        QTRY_VERIFY(!view.isLoading());
        view.reload();
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("before-crash"), 15000);
        QTRY_VERIFY(!view.isLoading());
        QCOMPARE(evaluate(view, "2 + 2").toInt(), 4);
    }

    void profileDestructionCompletesCookieCallbackOnce() {
        QTemporaryDir directory;
        auto profile = std::make_unique<WpeProfile>(regularParameters(directory.path(), "destruction"));
        int callbacks = 0;
        QString error;
        profile->exportPortableCookies([&](CookieSnapshotResult result) {
            ++callbacks;
            error = result.errorCode;
        });
        profile.reset();
        QCOMPARE(callbacks, 1);
        QCOMPARE(error, QString("profile-destroyed"));
        QTest::qWait(100);
        QCOMPARE(callbacks, 1);
    }

    void browsingSoak() {
        const auto pagesPath = qEnvironmentVariable("EDEN_WPE_BROWSING_PAGES");
        if (pagesPath.isEmpty()) {
            QSKIP("Browsing validation requires an explicit page set");
        }
        QFile pagesFile(pagesPath);
        QVERIFY(pagesFile.open(QIODevice::ReadOnly));
        const auto pageDocument = QJsonDocument::fromJson(pagesFile.readAll());
        QVERIFY(pageDocument.isArray());
        const auto pages = pageDocument.array();
        QVERIFY(pages.size() >= 5);
        for (const auto &value : pages) {
            const auto page = value.toObject();
            const QUrl address(page.value("url").toString());
            QVERIFY(address.isValid() && (address.scheme() == "https" || address.scheme() == "http"));
            QVERIFY(!address.host().isEmpty() && address.userInfo().isEmpty());
            QVERIFY(!page.value("selector").toString().isEmpty());
            QVERIFY(!page.value("expectedText").toString().isEmpty());
        }
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        int terminations = 0;
        int finishedLoads = 0;
        const auto terminationHandler = g_signal_connect(
            view.nativeView(),
            "web-process-terminated",
            G_CALLBACK(+[](WebKitWebView *, WebKitWebProcessTerminationReason, gpointer data) {
                ++*static_cast<int *>(data);
            }),
            &terminations
        );
        const auto loadHandler = g_signal_connect(
            view.nativeView(),
            "load-changed",
            G_CALLBACK(+[](WebKitWebView *, WebKitLoadEvent event, gpointer data) {
                if (event == WEBKIT_LOAD_FINISHED) {
                    ++*static_cast<int *>(data);
                }
            }),
            &finishedLoads
        );
        const auto disconnectHandlers = qScopeGuard([&] {
            g_signal_handler_disconnect(view.nativeView(), terminationHandler);
            g_signal_handler_disconnect(view.nativeView(), loadHandler);
        });
        QQuickWindow window;
        window.resize(1280, 900);
        view.attach(window.contentItem());
        window.show();
        QElapsedTimer duration;
        duration.start();
        int visits = 0;
        int assertions = 0;
        while (duration.elapsed() < 3600000 || visits < 120) {
            QVERIFY2(duration.elapsed() < 4100000, "Browsing did not exercise enough pages in the allowed time");
            const auto page = pages.at(visits % pages.size()).toObject();
            const int previousLoads = finishedLoads;
            view.load(QUrl(page.value("url").toString()));
            QTRY_VERIFY_WITH_TIMEOUT(terminations || finishedLoads > previousLoads, 30000);
            QCOMPARE(terminations, 0);
            QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 10000);
            const auto configuration = QString::fromUtf8(QJsonDocument(page).toJson(QJsonDocument::Compact));
            const auto check = QString(
                                   "(()=>{const p=%1;const node=document.querySelector(p.selector);"
                                   "return document.readyState==='complete' && document.visibilityState==='visible'"
                                   " && !!node && node.textContent.includes(p.expectedText)})()"
            )
                                   .arg(configuration);
            QTRY_VERIFY_WITH_TIMEOUT(evaluate(view, check).toBool(), 15000);
            ++assertions;
            for (int step = 0; step < 8; ++step) {
                QVERIFY(evaluate(
                            view,
                            QString(
                                "(()=>{scrollTo(0,(document.documentElement.scrollHeight-innerHeight)*%1/7);"
                                "const node=document.elementFromPoint(innerWidth/2,innerHeight/2);"
                                "return !!node && document.visibilityState==='visible'})()"
                            )
                                .arg(step)
                )
                            .toBool());
                ++assertions;
                QTest::qWait(500);
                QCOMPARE(terminations, 0);
            }
            QVERIFY(view.surface()->accelerated());
            QVERIFY(view.surface()->textureImportCount() > 0);
            ++visits;
        }
        QSaveFile output(qEnvironmentVariable("EDEN_WPE_BROWSING_REPORT"));
        QVERIFY(output.open(QIODevice::WriteOnly));
        const auto report = QJsonDocument(
                                QJsonObject{
                                    {"seconds", duration.elapsed() / 1000.0},
                                    {"visits", visits},
                                    {"pages", int(pages.size())},
                                    {"assertions", assertions},
                                    {"rendererTerminations", terminations},
                                    {"passed", true}
                                }
        ).toJson();
        QCOMPARE(output.write(report), qint64(report.size()));
        QVERIFY(output.commit());
    }

    void cleanupTestCase() {
        m_server.close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (m_blinkApi) {
            m_blinkApi->shutdown();
        }
        WpeRuntime::shutdown();
    }

  private:
    void runFramePaths(const QString &constraints, bool mockDevices) {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(900, 600);
        view.attach(window.contentItem());
        window.show();
        auto *settings = webkit_web_view_get_settings(view.nativeView());
        webkit_settings_set_enable_mock_capture_devices(settings, mockDevices);
        webkit_settings_set_media_playback_requires_user_gesture(settings, FALSE);
        QObject::connect(
            &view,
            &EngineView::permissionRequested,
            &view,
            [&view](const PermissionRequestInfo &info) { view.resolvePermissionRequest(info.id, true); }
        );
        view.load(url("frame-paths"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("frame-paths"), 15000);
        evaluate(view, QString("window.framePathsConstraints = %1;").arg(constraints) + framePathsScript());
        QElapsedTimer waited;
        waited.start();
        while (!evaluate(view, "!!window.framePaths").toBool() && waited.elapsed() < 60000) {
            QTest::qWait(200);
        }
        const QJsonObject result = QJsonDocument::fromJson(evaluate(view, "JSON.stringify(window.framePaths || {stage: window.framePathsStage})").toString().toUtf8()).object();
        const QString label = QString("%1 camera %2").arg(mockDevices ? "Mock" : "Real", constraints);
        qInfo().noquote() << "Frame paths" << label << QJsonDocument(result).toJson(QJsonDocument::Compact);
        QVERIFY2(!result.contains("stage") && !result.contains("fatal"), qPrintable(QString("%1: the fixture stopped at %2 %3").arg(label, result.value("stage").toString(), result.value("fatal").toString())));
        const QJsonObject worker = result.value("worker").toObject();
        for (const char *failure : {"error", "writeError", "workerError", "timeout"}) {
            QVERIFY2(!worker.contains(failure), qPrintable(QString("%1: worker %2: %3").arg(label, failure, worker.value(failure).toVariant().toString())));
        }
        const QStringList formats = {"I420", "I420A", "I422", "I444", "NV12", "RGBA", "RGBX", "BGRA", "BGRX"};
        QVERIFY2(formats.contains(worker.value("format").toString()), qPrintable(QString("%1: camera frames have format %2").arg(label, worker.value("format").toVariant().toString())));
        const QJsonObject reference = worker.value("native").toObject();
        QVERIFY2(reference.contains("mean"), qPrintable(QString("%1: copyTo() failed: %2").arg(label, QString::fromUtf8(QJsonDocument(reference).toJson(QJsonDocument::Compact)))));
        const double referenceMean = reference.value("mean").toDouble();
        const double referenceDeviation = reference.value("deviation").toDouble();
        const auto correlation = [](const QJsonArray &first, const QJsonArray &second, bool flipSecond) {
            if (first.size() != 48 || second.size() != 48) {
                return -2.0;
            }
            double sumFirst = 0;
            double sumSecond = 0;
            for (int index = 0; index < 48; ++index) {
                sumFirst += first[index].toDouble();
                sumSecond += second[index].toDouble();
            }
            const double meanFirst = sumFirst / 48;
            const double meanSecond = sumSecond / 48;
            double covariance = 0;
            double varianceFirst = 0;
            double varianceSecond = 0;
            for (int index = 0; index < 48; ++index) {
                const int secondIndex = flipSecond ? (5 - index / 8) * 8 + index % 8 : index;
                const double a = first[index].toDouble() - meanFirst;
                const double b = second[secondIndex].toDouble() - meanSecond;
                covariance += a * b;
                varianceFirst += a * a;
                varianceSecond += b * b;
            }
            return varianceFirst > 0 && varianceSecond > 0 ? covariance / std::sqrt(varianceFirst * varianceSecond) : 0.0;
        };
        const auto expectSameImage = [&](const QString &name, const QJsonObject &path) {
            QVERIFY2(path.contains("mean"), qPrintable(QString("%1: %2 failed: %3").arg(label, name, QString::fromUtf8(QJsonDocument(path).toJson(QJsonDocument::Compact)))));
            if (referenceDeviation > 8) {
                const double upright = correlation(path.value("grid").toArray(), reference.value("grid").toArray(), false);
                const double flipped = correlation(path.value("grid").toArray(), reference.value("grid").toArray(), true);
                QVERIFY2(upright >= 0.8 && upright > flipped,
                    qPrintable(QString("%1: %2 does not match the frame's layout (correlation %3 upright, %4 flipped)").arg(label, name).arg(upright, 0, 'f', 2).arg(flipped, 0, 'f', 2)));
            }
            QVERIFY2(std::abs(path.value("mean").toDouble() - referenceMean) <= 20,
                qPrintable(QString("%1: %2 has mean luma %3, the frame data has %4").arg(label, name).arg(path.value("mean").toDouble()).arg(referenceMean)));
            if (referenceDeviation > 8) {
                QVERIFY2(path.value("deviation").toDouble() >= referenceDeviation * 0.4,
                    qPrintable(QString("%1: %2 is flat (deviation %3, the frame data has %4)").arg(label, name).arg(path.value("deviation").toDouble()).arg(referenceDeviation)));
            }
        };
        for (const char *name : {"bitmap", "canvas2d", "webgl", "output", "rgba", "bgrx"}) {
            expectSameImage(QString("worker %1").arg(name), worker.value(name).toObject());
        }
        const QJsonObject rgba = worker.value("rgba").toObject();
        QCOMPARE(rgba.value("alphaMin").toInt(), 255);
        const QJsonArray frameSize = worker.value("size").toArray();
        QCOMPARE(rgba.value("allocation").toInt(), frameSize.at(0).toInt() * frameSize.at(1).toInt() * 4);
        QCOMPARE(worker.value("rejectsYuvTarget").toObject().value("name").toString(), QString("NotSupportedError"));
        const QJsonObject trackSettings = result.value("settings").toObject();
        const QJsonArray settingsSize({trackSettings.value("width").toInt(), trackSettings.value("height").toInt()});
        QVERIFY2(frameSize == settingsSize, qPrintable(QString("%1: frames are %2x%3 but the track reports %4x%5").arg(label).arg(frameSize.at(0).toInt()).arg(frameSize.at(1).toInt()).arg(settingsSize.at(0).toInt()).arg(settingsSize.at(1).toInt())));
        const QJsonObject page = result.value("page").toObject();
        QVERIFY2(page.value("size").toArray() == settingsSize, qPrintable(QString("%1: the camera video element is %2x%3 but the track reports %4x%5").arg(label).arg(page.value("size").toArray().at(0).toInt()).arg(page.value("size").toArray().at(1).toInt()).arg(settingsSize.at(0).toInt()).arg(settingsSize.at(1).toInt())));
        for (const char *name : {"canvas", "readbackCanvas", "webgl", "bitmap", "generated", "remote"}) {
            expectSameImage(QString("page %1").arg(name), page.value(name).toObject());
        }
        expectSameImage("page remote readback", page.value("remote").toObject().value("readback").toObject());
        const QJsonObject resized = page.value("resized").toObject();
        QVERIFY2(resized.value("settings").toArray() == QJsonArray({320, 240}) && resized.value("element").toArray() == QJsonArray({320, 240}),
            qPrintable(QString("%1: after applyConstraints the camera reported %2").arg(label, QString::fromUtf8(QJsonDocument(resized).toJson(QJsonDocument::Compact)))));
        QVERIFY2(worker.value("frames").toInt() >= 30, qPrintable(QString("%1: the worker read %2 frames in 4 seconds").arg(label).arg(worker.value("frames").toInt())));
        QVERIFY2(page.value("generatedPresented").toInt() >= 20, qPrintable(QString("%1: the page presented %2 generated frames").arg(label).arg(page.value("generatedPresented").toInt())));
        QVERIFY2(worker.value("processingMs").toDouble() <= 16, qPrintable(QString("%1: a WebGL effects step took %2 ms per frame").arg(label).arg(worker.value("processingMs").toDouble())));
    }

    static QString framePathsScript() {
        return QString(R"JS(
window.framePaths = null;
window.framePathsStage = 'opening the camera';
(async () => {
    const statsSource = `
const rgbLuma = (r, g, b) => 0.299 * r + 0.587 * g + 0.114 * b;
const imageStats = (sampleAt, width, height) => {
    let sum = 0;
    let sumSquares = 0;
    let count = 0;
    const cells = new Array(48).fill(0);
    const cellCounts = new Array(48).fill(0);
    const stepX = Math.max(1, Math.floor(width / 64));
    const stepY = Math.max(1, Math.floor(height / 48));
    for (let y = Math.floor(stepY / 2); y < height; y += stepY) {
        for (let x = Math.floor(stepX / 2); x < width; x += stepX) {
            const value = sampleAt(x, y);
            sum += value;
            sumSquares += value * value;
            count++;
            const cell = Math.min(5, Math.floor(y * 6 / height)) * 8 + Math.min(7, Math.floor(x * 8 / width));
            cells[cell] += value;
            cellCounts[cell]++;
        }
    }
    const mean = sum / count;
    return {mean: Math.round(mean * 10) / 10, deviation: Math.round(Math.sqrt(Math.max(0, sumSquares / count - mean * mean)) * 10) / 10, width, height,
        grid: cells.map((total, index) => Math.round(total / Math.max(1, cellCounts[index])))};
};
const rgbaStats = (data, width, height, bottomUp) => imageStats((x, y) => {
    const offset = ((bottomUp ? height - 1 - y : y) * width + x) * 4;
    return rgbLuma(data[offset], data[offset + 1], data[offset + 2]);
}, width, height);
const frameStats = async (frame, format) => {
    const width = frame.visibleRect ? frame.visibleRect.width : frame.displayWidth;
    const height = frame.visibleRect ? frame.visibleRect.height : frame.displayHeight;
    const options = format ? {format} : {};
    const pixels = new Uint8Array(frame.allocationSize(options));
    const layout = await frame.copyTo(pixels, options);
    const fullRange = frame.colorSpace && frame.colorSpace.fullRange;
    const toFull = value => fullRange ? value : Math.max(0, Math.min(255, (value - 16) * 255 / 219));
    const withAlpha = stats => {
        let alphaMin = 255;
        for (let y = 0; y < height; y += 7) {
            for (let x = 0; x < width; x += 7)
                alphaMin = Math.min(alphaMin, pixels[layout[0].offset + y * layout[0].stride + x * 4 + 3]);
        }
        return {...stats, alphaMin, allocation: pixels.length};
    };
    switch (format || frame.format) {
    case 'I420':
    case 'I420A':
    case 'I422':
    case 'I444':
    case 'NV12':
        return imageStats((x, y) => toFull(pixels[layout[0].offset + y * layout[0].stride + x]), width, height);
    case 'RGBA':
    case 'RGBX':
        return withAlpha(imageStats((x, y) => {
            const offset = layout[0].offset + y * layout[0].stride + x * 4;
            return rgbLuma(pixels[offset], pixels[offset + 1], pixels[offset + 2]);
        }, width, height));
    case 'BGRA':
    case 'BGRX':
        return withAlpha(imageStats((x, y) => {
            const offset = layout[0].offset + y * layout[0].stride + x * 4;
            return rgbLuma(pixels[offset + 2], pixels[offset + 1], pixels[offset]);
        }, width, height));
    }
    return {unsupportedFormat: frame.format};
};
const makeTextureRenderer = (gl) => {
    const compile = (type, source) => {
        const shader = gl.createShader(type);
        gl.shaderSource(shader, source);
        gl.compileShader(shader);
        return shader;
    };
    const program = gl.createProgram();
    gl.attachShader(program, compile(gl.VERTEX_SHADER, '#version 300 es\\nin vec2 position;\\nout vec2 coordinate;\\nvoid main() {\\n    coordinate = vec2((position.x + 1.0) * 0.5, (1.0 - position.y) * 0.5);\\n    gl_Position = vec4(position, 0.0, 1.0);\\n}\\n'));
    gl.attachShader(program, compile(gl.FRAGMENT_SHADER, '#version 300 es\\nprecision mediump float;\\nin vec2 coordinate;\\nuniform sampler2D source;\\nout vec4 color;\\nvoid main() {\\n    color = vec4(texture(source, coordinate).rgb, 1.0);\\n}\\n'));
    gl.linkProgram(program);
    gl.useProgram(program);
    const buffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const location = gl.getAttribLocation(program, 'position');
    gl.enableVertexAttribArray(location);
    gl.vertexAttribPointer(location, 2, gl.FLOAT, false, 0, 0);
    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    return source => {
        gl.bindTexture(gl.TEXTURE_2D, texture);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, source);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    };
};
const webglStats = gl => {
    const width = gl.drawingBufferWidth;
    const height = gl.drawingBufferHeight;
    const pixels = new Uint8Array(width * height * 4);
    gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    const error = gl.getError();
    return error ? {glError: error} : rgbaStats(pixels, width, height, true);
};
const canvasStats = (canvas, source, options) => {
    const context = canvas.getContext('2d', options);
    context.drawImage(source, 0, 0, canvas.width, canvas.height);
    return rgbaStats(context.getImageData(0, 0, canvas.width, canvas.height).data, canvas.width, canvas.height);
};
const attempt = async work => {
    try {
        return await work();
    } catch (error) {
        return {error: String(error && error.message || error)};
    }
};
`;
    const {frameStats, makeTextureRenderer, webglStats, canvasStats, attempt} = new Function(statsSource + 'return {frameStats, makeTextureRenderer, webglStats, canvasStats, attempt};')();
    const workerSource = statsSource + `
self.onmessage = async ({data}) => {
    const report = {};
    try {
        const reader = new MediaStreamTrackProcessor({track: data.track}).readable.getReader();
        const generator = new VideoTrackGenerator();
        self.postMessage({generatorTrack: generator.track}, [generator.track]);
        const writer = generator.writable.getWriter();
        let canvas = null;
        let gl = null;
        let render = null;
        let frames = 0;
        let processed = 0;
        let processingMs = 0;
        let renderMs = 0;
        let wrapMs = 0;
        let writeMs = 0;
        const deadline = performance.now() + 4000;
        while (performance.now() < deadline) {
            const {value: frame, done} = await reader.read();
            if (done)
                break;
            frames++;
            if (!canvas) {
                canvas = new OffscreenCanvas(frame.displayWidth, frame.displayHeight);
                gl = canvas.getContext('webgl2');
                render = makeTextureRenderer(gl);
            }
            const start = performance.now();
            render(frame);
            gl.finish();
            const rendered = performance.now();
            const output = new VideoFrame(canvas, {timestamp: frame.timestamp});
            const elapsed = performance.now() - start;
            if (frames !== 20) {
                renderMs += rendered - start;
                wrapMs += performance.now() - rendered;
            }
            if (frames === 20) {
                report.format = frame.format;
                report.size = [frame.displayWidth, frame.displayHeight];
                report.colorSpace = frame.colorSpace && frame.colorSpace.toJSON ? frame.colorSpace.toJSON() : null;
                report.native = await attempt(() => frameStats(frame));
                report.rgba = await attempt(() => frameStats(frame, 'RGBA'));
                report.bgrx = await attempt(() => frameStats(frame, 'BGRX'));
                report.rejectsYuvTarget = await attempt(async () => {
                    try {
                        frame.allocationSize({format: 'I420'});
                        return {accepted: true};
                    } catch (error) {
                        return {name: error.name};
                    }
                });
                report.webgl = await attempt(() => webglStats(gl));
                report.output = await attempt(() => frameStats(output));
                report.outputFormat = output.format;
                report.canvas2d = await attempt(() => canvasStats(new OffscreenCanvas(frame.displayWidth, frame.displayHeight), frame, {willReadFrequently: true}));
                report.bitmap = await attempt(async () => {
                    const bitmap = await createImageBitmap(frame);
                    const stats = canvasStats(new OffscreenCanvas(bitmap.width, bitmap.height), bitmap, {willReadFrequently: true});
                    bitmap.close();
                    return stats;
                });
            }
            const writeStart = performance.now();
            try {
                await writer.write(output);
            } catch (error) {
                report.writeError = String(error);
                frame.close();
                break;
            }
            if (frames !== 20) {
                writeMs += performance.now() - writeStart;
                processingMs += elapsed + performance.now() - writeStart;
                processed++;
            }
            frame.close();
        }
        report.frames = frames;
        report.processingMs = Math.round(processingMs / Math.max(1, processed) * 10) / 10;
        report.stepMs = {render: Math.round(renderMs / Math.max(1, processed) * 10) / 10, wrap: Math.round(wrapMs / Math.max(1, processed) * 10) / 10, write: Math.round(writeMs / Math.max(1, processed) * 10) / 10};
        reader.cancel();
    } catch (error) {
        report.error = String(error && error.message || error);
    }
    self.postMessage({report});
};
`;
    const stream = await navigator.mediaDevices.getUserMedia({video: window.framePathsConstraints});
    const track = stream.getVideoTracks()[0];
    const presented = (video, count) => new Promise(resolve => {
        let frames = 0;
        const next = () => { if (++frames >= count) resolve(); else video.requestVideoFrameCallback(next); };
        video.requestVideoFrameCallback(next);
    });
    const makeVideo = source => {
        const video = document.createElement('video');
        video.muted = true;
        video.playsInline = true;
        video.srcObject = source;
        video.style.cssText = 'width:160px;height:120px';
        document.body.appendChild(video);
        return video;
    };
    window.framePathsStage = 'waiting for camera frames';
    const video = makeVideo(stream);
    await video.play();
    await presented(video, 20);
    window.framePathsStage = 'running the worker pipeline';
    const worker = new Worker(URL.createObjectURL(new Blob([workerSource], {type: 'text/javascript'})));
    let generatedVideo = null;
    let generatedPresented = 0;
    const workerDone = new Promise(resolve => {
        const timer = setTimeout(() => resolve({timeout: true}), 20000);
        worker.onerror = event => { clearTimeout(timer); resolve({workerError: event.message}); };
        worker.onmessage = event => {
            if (event.data.generatorTrack) {
                generatedVideo = makeVideo(new MediaStream([event.data.generatorTrack]));
                generatedVideo.play().catch(() => {});
                const count = () => { generatedPresented++; generatedVideo.requestVideoFrameCallback(count); };
                generatedVideo.requestVideoFrameCallback(count);
                return;
            }
            clearTimeout(timer);
            resolve(event.data.report);
        };
    });
    const clone = track.clone();
    worker.postMessage({track: clone}, [clone]);
    await new Promise(resolve => setTimeout(resolve, 1000));
    window.framePathsStage = 'drawing the camera video on the page';
    const width = video.videoWidth;
    const height = video.videoHeight;
    const makeCanvas = () => {
        const canvas = document.createElement('canvas');
        canvas.width = width;
        canvas.height = height;
        return canvas;
    };
    const page = {size: [width, height]};
    page.canvas = await attempt(() => canvasStats(makeCanvas(), video));
    page.readbackCanvas = await attempt(() => canvasStats(makeCanvas(), video, {willReadFrequently: true}));
    page.webgl = await attempt(() => {
        const gl = makeCanvas().getContext('webgl2');
        makeTextureRenderer(gl)(video);
        return webglStats(gl);
    });
    page.bitmap = await attempt(async () => {
        const bitmap = await createImageBitmap(video);
        const stats = canvasStats(makeCanvas(), bitmap, {willReadFrequently: true});
        bitmap.close();
        return stats;
    });
    window.framePathsStage = 'drawing the remote video';
    page.remote = await attempt(async () => {
        const sender = new RTCPeerConnection();
        const receiver = new RTCPeerConnection();
        sender.onicecandidate = event => event.candidate && receiver.addIceCandidate(event.candidate);
        receiver.onicecandidate = event => event.candidate && sender.addIceCandidate(event.candidate);
        const remoteTrack = new Promise(resolve => { receiver.ontrack = event => resolve(event.track); });
        sender.addTrack(track.clone());
        await sender.setLocalDescription();
        await receiver.setRemoteDescription(sender.localDescription);
        await receiver.setLocalDescription();
        await sender.setRemoteDescription(receiver.localDescription);
        const remoteVideo = makeVideo(new MediaStream([await remoteTrack]));
        await remoteVideo.play();
        await Promise.race([presented(remoteVideo, 30), new Promise((resolve, reject) => setTimeout(() => reject(new Error('the remote video presented no frames')), 10000))]);
        const canvas = document.createElement('canvas');
        canvas.width = remoteVideo.videoWidth;
        canvas.height = remoteVideo.videoHeight;
        const stats = canvasStats(canvas, remoteVideo);
        const readback = document.createElement('canvas');
        readback.width = remoteVideo.videoWidth;
        readback.height = remoteVideo.videoHeight;
        const readbackStats = canvasStats(readback, remoteVideo, {willReadFrequently: true});
        sender.close();
        receiver.close();
        return {...stats, readback: readbackStats};
    });
    window.framePathsStage = 'waiting for the worker';
    const workerReport = await workerDone;
    const initialSettings = track.getSettings();
    window.framePathsStage = 'changing the camera resolution';
    page.resized = await attempt(async () => {
        await track.applyConstraints({width: {ideal: 320}, height: {ideal: 240}});
        const deadline = performance.now() + 8000;
        while (performance.now() < deadline && (video.videoWidth !== 320 || video.videoHeight !== 240))
            await presented(video, 1);
        await presented(video, 10);
        const settings = track.getSettings();
        return {settings: [settings.width, settings.height], element: [video.videoWidth, video.videoHeight]};
    });
    page.generatedPresented = generatedPresented;
    page.generated = generatedVideo && generatedVideo.videoWidth ? await attempt(() => canvasStats(makeCanvas(), generatedVideo, {willReadFrequently: true})) : {error: 'no generated video'};
    worker.terminate();
    stream.getTracks().forEach(item => item.stop());
    window.framePaths = {worker: workerReport, page, settings: initialSettings};
})().catch(error => window.framePaths = {fatal: String(error && error.message || error), stage: window.framePathsStage});
)JS");
    }

    static QString cameraCallScript() {
        return QString(R"JS(window.cameraCall = null;
(async () => {
    const stream = await navigator.mediaDevices.getUserMedia({audio: true, video: {width: {ideal: 1280}, height: {ideal: 720}, frameRate: {ideal: 30}}});
    window.cameraTimerLag = 0;
    let lastTick = performance.now();
    setInterval(() => {
        const now = performance.now();
        window.cameraTimerLag = Math.max(window.cameraTimerLag, now - lastTick - 10);
        lastTick = now;
    }, 10);
    const makeVideo = source => {
        const video = document.createElement('video');
        video.muted = true;
        video.autoplay = true;
        video.playsInline = true;
        video.style.cssText = 'width:320px;height:180px';
        video.srcObject = source;
        document.body.append(video);
        return video;
    };
    const self = makeVideo(stream);
    await self.play();
    window.cameraPresented = 0;
    if (self.requestVideoFrameCallback) {
        const countPresented = () => {
            window.cameraPresented++;
            self.requestVideoFrameCallback(countPresented);
        };
        self.requestVideoFrameCallback(countPresented);
    }
    const connect = async (sender, receiver, rewriteAnswer) => {
        sender.onicecandidate = event => event.candidate && receiver.addIceCandidate(event.candidate);
        receiver.onicecandidate = event => event.candidate && sender.addIceCandidate(event.candidate);
        const offer = await sender.createOffer();
        await sender.setLocalDescription(offer);
        await receiver.setRemoteDescription(offer);
        const answer = await receiver.createAnswer();
        await receiver.setLocalDescription(answer);
        await sender.setRemoteDescription({type: 'answer', sdp: rewriteAnswer ? rewriteAnswer(offer.sdp, answer.sdp) : answer.sdp});
    };
    const acceptSimulcast = (offerSdp, answerSdp) => {
        const simulcast = (offerSdp.match(/a=simulcast:send (\S+)/) || [])[1];
        const rids = (offerSdp.match(/a=rid:\S+ send/g) || []).map(line => line.replace(' send', ' recv'));
        const lines = answerSdp.split('\r\n');
        const start = lines.findIndex(line => line.startsWith('m=video'));
        let end = lines.findIndex((line, index) => index > start && line.startsWith('m='));
        if (end < 0)
            end = lines[lines.length - 1] === '' ? lines.length - 1 : lines.length;
        lines.splice(end, 0, ...rids, 'a=simulcast:recv ' + simulcast);
        return lines.join('\r\n');
    };
    const sender = new RTCPeerConnection();
    const receiver = new RTCPeerConnection();
    const video = sender.addTransceiver(stream.getVideoTracks()[0], {direction: 'sendonly', sendEncodings: [{rid: 'q', scaleResolutionDownBy: 4}, {rid: 'h', scaleResolutionDownBy: 2}, {rid: 'f'}]});
    sender.addTransceiver(stream.getAudioTracks()[0], {direction: 'sendonly'});
    await connect(sender, receiver, acceptSimulcast);
    const directSender = new RTCPeerConnection();
    const directReceiver = new RTCPeerConnection();
    let remote = null;
    directReceiver.ontrack = event => {
        if (event.track.kind === 'video' && !remote) {
            remote = makeVideo(new MediaStream([event.track]));
            remote.play().catch(() => {});
        }
    };
    const direct = directSender.addTransceiver(stream.getVideoTracks()[0], {direction: 'sendonly'});
    await connect(directSender, directReceiver);
    const sample = async () => {
        const outbound = await sender.getStats();
        const inbound = await directReceiver.getStats();
        let audioBytes = 0;
        let layers = 0;
        outbound.forEach(item => {
            if (item.type === 'outbound-rtp' && item.kind === 'audio')
                audioBytes += item.bytesSent || 0;
            if (item.type === 'outbound-rtp' && item.kind === 'video' && item.framesEncoded)
                layers++;
        });
        let remoteFrames = 0;
        inbound.forEach(item => {
            if (item.type === 'inbound-rtp' && item.kind === 'video')
                remoteFrames = Math.max(remoteFrames, item.framesDecoded || 0);
        });
        let encodedFrames = 0;
        let limitation = '';
        let encodedWidth = 0;
        let targetBitrate = 0;
        let encodeTime = 0;
        let keyFrames = 0;
        let pliCount = 0;
        let firCount = 0;
        let bytesSent = 0;
        const layerFrames = {};
        let sourceFrames = 0;
        let availableBitrate = 0;
        const remoteLost = {};
        const remoteFractionLost = {};
        let nackCount = 0;
        let roundTrip = 0;
        outbound.forEach(item => {
            if (item.type === 'outbound-rtp' && item.kind === 'video') {
                layerFrames[item.rid] = item.framesEncoded || 0;
                if (item.rid === 'f') {
                    encodedFrames = item.framesEncoded || 0;
                    limitation = item.qualityLimitationReason || '';
                    encodedWidth = item.frameWidth || 0;
                    targetBitrate = item.targetBitrate || 0;
                    encodeTime = item.totalEncodeTime || 0;
                    keyFrames = item.keyFramesEncoded || 0;
                    pliCount = item.pliCount || 0;
                    firCount = item.firCount || 0;
                    bytesSent = item.bytesSent || 0;
                }
            }
            if (item.type === 'media-source' && item.kind === 'video')
                sourceFrames = item.frames || 0;
            if (item.type === 'remote-inbound-rtp' && item.kind === 'video') {
                remoteLost[item.localId] = item.packetsLost || 0;
                remoteFractionLost[item.localId] = item.fractionLost || 0;
            }
            if (item.type === 'outbound-rtp' && item.kind === 'video')
                nackCount += item.nackCount || 0;
            if (item.type === 'candidate-pair' && item.nominated && item.state === 'succeeded') {
                availableBitrate = item.availableOutgoingBitrate || 0;
                roundTrip = item.currentRoundTripTime || 0;
            }
        });
        let directEncoded = 0;
        let directKeyFrames = 0;
        let directLost = 0;
        (await directSender.getStats()).forEach(item => {
            if (item.type === 'outbound-rtp' && item.kind === 'video') {
                directEncoded = item.framesEncoded || 0;
                directKeyFrames = item.keyFramesEncoded || 0;
            }
            if (item.type === 'remote-inbound-rtp' && item.kind === 'video')
                directLost = item.packetsLost || 0;
        });
        const layerLost = {};
        outbound.forEach(item => {
            if (item.type === 'outbound-rtp' && item.kind === 'video' && item.id in remoteLost)
                layerLost[item.rid] = [remoteLost[item.id], remoteFractionLost[item.id], item.packetsSent || 0];
        });
        const timerLag = window.cameraTimerLag;
        window.cameraTimerLag = 0;
        const quality = self.getVideoPlaybackQuality();
        return {selfFrames: quality.totalVideoFrames, selfDropped: quality.droppedVideoFrames, selfPresented: window.cameraPresented, selfTime: self.currentTime, remoteFrames, audioBytes, layers, encodedFrames, limitation, encodedWidth, targetBitrate, encodeTime, keyFrames, pliCount, firCount, bytesSent, layerFrames, sourceFrames, availableBitrate, roundTrip, layerLost, nackCount, directEncoded, directKeyFrames, directLost, timerLag, connection: sender.connectionState};
    };
    window.cameraCall = {sample, video, direct, stream, ready: true};
})().catch(error => window.cameraCall = {error: String(error)});
)JS");
    }

    QVariant evaluate(WpeEngineView &view, const QString &script) {
        QVariant value;
        bool finished = false;
        view.evaluate(
            script,
            [&](QVariant result) {
                value = result;
                finished = true;
            },
            nullptr
        );
        QElapsedTimer deadline;
        deadline.start();
        while (!finished && deadline.elapsed() < 15000) {
            QTest::qWait(1);
        }
        if (!finished) {
            qFatal("The native JavaScript fixture timed out");
        }
        return value;
    }
    const EnginePluginApi *m_blinkApi = nullptr;
    QTemporaryDir m_storage;
    EngineProfileParameters privateParameters() const {
        return {"synthetic", Backend::Wpe, true, {}, {}};
    }
    EngineProfileParameters regularParameters(const QString &path, const QString &id) const {
        return {id, Backend::Wpe, false, path + "/data", path + "/cache"};
    }
    QUrl url(const QString &path) const {
        return QUrl(QString("http://127.0.0.1:%1/%2").arg(m_server.serverPort()).arg(path));
    }
    QTcpServer m_server;
};

static int runUserInterfaceHangChild() {
    if (!WpeRuntime::initialize("Eden", "test")) {
        return 2;
    }
    {
        WpeProfile profile({"ui-hang", Backend::Wpe, true, {}, {}});
        const auto run = [](int milliseconds) {
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < milliseconds) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            }
        };
        run(300);
        QElapsedTimer busy;
        busy.start();
        while (busy.elapsed() < 1000) {
        }
        run(500);
    }
    WpeRuntime::shutdown();
    return 0;
}

int main(int argc, char *argv[]) {
    qputenv("QT_NO_GLIB", "1");
    qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QTemporaryDir fixtureDirectories;
    qputenv("XDG_CONFIG_HOME", fixtureDirectories.path().toUtf8());
    QDir().mkpath(fixtureDirectories.filePath("enchant/hunspell"));
    QFile affix(fixtureDirectories.filePath("enchant/hunspell/en_US.aff"));
    QFile dictionary(fixtureDirectories.filePath("enchant/hunspell/en_US.dic"));
    if (!affix.open(QIODevice::WriteOnly) || !dictionary.open(QIODevice::WriteOnly)) {
        return 2;
    }
    affix.write("SET UTF-8\n");
    dictionary.write("3\nhello\nworld\nbrowser\n");
    affix.close();
    dictionary.close();
    QDir().mkpath(fixtureDirectories.filePath("downloads"));
    QFile directories(fixtureDirectories.filePath("user-dirs.dirs"));
    if (!directories.open(QIODevice::WriteOnly)) {
        return 2;
    }
    directories.write("XDG_DOWNLOAD_DIR=\"" + fixtureDirectories.filePath("downloads").toUtf8() + "\"\n");
    directories.close();
    QCoreApplication::setApplicationName("EdenWebKitNativeTest");
    QCoreApplication::setApplicationVersion("0.4.0");
    QGuiApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    eden::platform::WaylandGlibEventDispatcher dispatcher;
    if (argc > 1 && QByteArray(argv[1]) == "--ui-hang-child") {
        return runUserInterfaceHangChild();
    }
    WpeBackendTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "wpebackend_test.moc"

#include "engine/engineplugin.h"
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
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLibrary>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

using namespace eden::engine;

class WpeBackendTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() {
        QVERIFY(WpeRuntime::initialize("Eden", "test"));
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
            QTest::keyClick(&window, Qt::Key_V, Qt::ControlModifier);
            QTest::keyClick(&window, Qt::Key_Home);
            const QString expected = QString("Before after").insert(offset, "https://example.test/link");
            QTRY_COMPARE_WITH_TIMEOUT(
                evaluate(view, "document.querySelector('#editor').textContent").toString(),
                expected,
                10000
            );
        }
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
        view.fillForm(target, {{"email", "person@example.test"}});
        QTRY_COMPARE_WITH_TIMEOUT(
            evaluate(view, "document.querySelector('#contact').value").toString(),
            QString("person@example.test"),
            5000
        );
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
        }
        QVERIFY(view.surface()->accelerated());
        QVERIFY2(
            view.surface()->textureImportCount() - imports <= 4,
            "GPU texture imports must remain bounded after the buffer pool warms up"
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

    void cleanupTestCase() {
        m_server.close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (m_blinkApi) {
            m_blinkApi->shutdown();
        }
        WpeRuntime::shutdown();
    }

  private:
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
    WpeBackendTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "wpebackend_test.moc"

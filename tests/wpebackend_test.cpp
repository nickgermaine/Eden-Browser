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
#include <QMimeData>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QScreen>
#include <QSemaphore>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

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
        QTest::newRow("swap-control") << QByteArray("swap") << false;
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
        int acknowledgements = 0;
        int releases = 0;
        g_signal_connect(
            native,
            "buffer-rendered",
            G_CALLBACK(+[](WPEView *, WPEBuffer *, int *count) { ++*count; }),
            &acknowledgements
        );
        g_signal_connect(
            native,
            "buffer-released",
            G_CALLBACK(+[](WPEView *, WPEBuffer *, int *count) { ++*count; }),
            &releases
        );
        {
            WpeFrame frame(native, buffer);
            frame.rendered();
            frame.rendered();
            QCOMPARE(acknowledgements, 1);
            QCOMPARE(releases, 0);
        }
        QCOMPARE(acknowledgements, 1);
        QCOMPARE(releases, 1);
        {
            WpeFrame dropped(native, buffer);
        }
        QCOMPARE(acknowledgements, 2);
        QCOMPARE(releases, 2);
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
        QTest::qWait(100);
        int acknowledged = 0;
        WPEView *native = view.surface()->nativeView();
        const gulong handler = g_signal_connect(
            native,
            "buffer-rendered",
            G_CALLBACK(+[](WPEView *, WPEBuffer *, int *count) { ++*count; }),
            &acknowledged
        );
        const auto disconnectNative = qScopeGuard([&] { g_signal_handler_disconnect(native, handler); });
        QSemaphore resumeRendering;
        std::atomic<bool> armed = true;
        std::atomic<bool> blocked = false;
        std::atomic<bool> singleThreaded = false;
        std::atomic<int> swaps = 0;
        const auto swapConnection =
            connect(&window, &QQuickWindow::frameSwapped, &window, [&] { ++swaps; }, Qt::DirectConnection);
        const auto blockConnection = connect(
            &window,
            &QQuickWindow::afterRendering,
            &window,
            [&] {
                if (!armed.exchange(false)) {
                    return;
                }
                if (QThread::isMainThread()) {
                    singleThreaded = true;
                    return;
                }
                blocked = true;
                resumeRendering.tryAcquire(1, 5000);
            },
            Qt::DirectConnection
        );
        const auto resume = qScopeGuard([&] {
            disconnect(blockConnection);
            disconnect(swapConnection);
            resumeRendering.release();
            window.hide();
        });
        evaluate(view, "document.querySelector('#box').style.background='blue';true");
        QTRY_VERIFY_WITH_TIMEOUT(blocked || singleThreaded, 10000);
        if (singleThreaded) {
            QSKIP("This regression requires the threaded scene graph");
        }
        QCOMPARE(swaps.load(), 0);
        if (acknowledgesBeforeSwap) {
            QTRY_VERIFY_WITH_TIMEOUT(acknowledged > 0, 2000);
        } else {
            QTest::qWait(100);
            QCOMPARE(acknowledged, 0);
        }
        QCOMPARE(swaps.load(), 0);
        resumeRendering.release();
        QTRY_VERIFY_WITH_TIMEOUT(swaps.load() > 0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(acknowledged > 0, 5000);
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

    void displaySyncTracksScreenAndStopsWhenHidden() {
        WpeProfile profile(privateParameters());
        WpeEngineView view(&profile);
        QQuickWindow window;
        window.resize(800, 600);
        view.attach(window.contentItem());
        window.show();
        view.load(url("display-sync"));
        QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("display-sync"), 15000);
        if (qEnvironmentVariable("EDEN_WPE_DISPLAY_SYNC") == "1") {
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
        if (qEnvironmentVariable("EDEN_WPE_DISPLAY_SYNC") == "1") {
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

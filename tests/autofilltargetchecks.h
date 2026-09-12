#pragma once

#include "engine/engineprofile.h"
#include "engine/engineview.h"

#include <QHash>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

#include <memory>
#include <optional>

class AutofillPageServer final : public QTcpServer {
  public:
    AutofillPageServer() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    QByteArray &request = m_requests[socket];
                    request += socket->readAll();
                    if (!request.contains("\r\n\r\n")) {
                        return;
                    }
                    const QByteArray path = request.split(' ').value(1);
                    m_requests.remove(socket);
                    QByteArray page = R"HTML(<!doctype html><input autocomplete="username"><input type="password">
<script>
const fields = Array.from(document.querySelectorAll('input'));
let events = 0;
for (const type of ['input', 'change']) {
    document.addEventListener(type, () => {
        document.title = fields.map(field => field.value).join('|') + ':' + ++events;
        if (parent !== window) { parent.postMessage(document.title, '*'); }
    });
}
if (location.pathname === '/child') { fields[0].focus(); }
if (location.pathname === '/forged') {
    Object.defineProperty(window, 'origin', {value:'https://forged.test'});
    window.__edenAutofillDocument = 'forged';
}
document.title = 'ready:' + location.pathname;
</script>)HTML";
                    if (path == "/capture-guard") {
                        page =
                            R"HTML(<!doctype html><button id="start" style="position:absolute;left:10px;top:10px;width:180px;height:48px">Start call</button>
<button id="stop" style="position:absolute;left:10px;top:80px;width:180px;height:48px">Stop call</button><script>
let stream;
navigator.mediaDevices.getUserMedia({video:{mandatory:{chromeMediaSource:'desktop',chromeMediaSourceId:'screen:0:0'}}})
.then(value=>{value.getTracks().forEach(track=>track.stop());document.title='legacy:allowed';},error=>document.title='legacy:'+error.name);
document.getElementById('start').onclick=()=>navigator.mediaDevices.getUserMedia({video:true,audio:true}).then(value=>{stream=value;document.title='call-active';},error=>document.title=error.name);
document.getElementById('stop').onclick=()=>{stream.getTracks().forEach(track=>track.stop());document.title='call-stopped';};
</script>)HTML";
                    } else if (path.startsWith("/personal")) {
                        page =
                            R"HTML(<!doctype html><input autocomplete="email"><textarea autocomplete="street-address"></textarea>
<script>
let events=0;
for(const type of ['input','change'])document.addEventListener(type,()=>{
    document.title=Array.from(document.querySelectorAll('input,textarea')).map(field=>field.value).join('|')+':'+ ++events;
});
document.title='personal-ready';
</script>)HTML";
                    } else if (path == "/cookie-source" || path == "/cookie-check") {
                        page = "<!doctype html><script>";
                        if (path == "/cookie-source") {
                            page +=
                                "document.cookie='ordinary=fixture; Path=/';"
                                "document.cookie='partitioned=fixture; Secure; SameSite=None; Partitioned; Path=/';";
                        }
                        page += "document.title=document.cookie;</script>";
                    } else if (path == "/iframe") {
                        page = "<!doctype html><title>ready:/iframe</title><iframe src=/child></iframe>"
                               "<script>onmessage=event=>document.title=event.data;</script>";
                    } else if (path == "/permission-dismissal") {
                        page = R"HTML(<!doctype html><title>permission-ready</title>
<button style="position:absolute;left:20px;top:20px;width:180px;height:48px">Location</button>
<script>
let attempt=0;
document.querySelector('button').onclick=()=>{
    const current=++attempt;
    const finish=()=>navigator.permissions.query({name:'geolocation'}).then(permission=>{
        document.title=permission.state+':'+current;
    });
    navigator.geolocation.getCurrentPosition(finish,finish);
};
</script>)HTML";
                    } else if (path == "/report-forgery") {
                        page = R"HTML(<!doctype html><title>forgery-loading</title>
<script>
console.info('EDEN_CREDENTIAL:["forged-main","forged-password"]');
console.info('EDEN_FIELD:["password","forged","current-password","forged-secret",1,2,3,4]');
let completed=0;
onmessage=event=>{if(event.data==='forgery-done'&&++completed===2)document.title='forgery-ready';};
</script><iframe src="/report-forgery-child"></iframe><iframe id="remote"></iframe>
<script>const remoteUrl=new URL('/report-forgery-child',location.href);remoteUrl.hostname='localhost';remote.src=remoteUrl.href;</script>)HTML";
                    } else if (path == "/report-forgery-child") {
                        page = R"HTML(<!doctype html><script>
console.info('EDEN_CREDENTIAL:["forged-child","forged-password"]');
console.info('EDEN_FIELD:["password","forged","current-password","forged-secret",1,2,3,4]');
parent.postMessage('forgery-done','*');
</script>)HTML";
                    } else if (path == "/form-events" || path.startsWith("/report-")) {
                        page = R"HTML(<!doctype html><title>form-events</title>
<style>input,button{position:absolute;left:10px;width:200px;height:30px;box-sizing:border-box}</style>
<input id="ordinary" name="todo" style="top:10px">
<form onsubmit="event.preventDefault()">
<input id="username" autocomplete="username" style="top:60px">
<input id="password" type="password" style="top:110px">
<button type="submit" style="top:160px">Sign in</button>
</form>
<button type="button" style="top:210px" onclick="
for(let index=0;index<1000;index++){
ordinary.value=String(index);
ordinary.dispatchEvent(new Event('input',{bubbles:true}));
}
document.title='burst-complete';">Input burst</button>)HTML";
                        if (path == "/report-world") {
                            page += R"HTML(<script>
window.__edenFormHooksInstalled=true;
window.__edenAutofillDocument='forged';
Object.defineProperty(window,'origin',{value:'https://forged.test'});
document.title='world:'+(typeof qt==='undefined'?'isolated':'exposed');
</script>)HTML";
                        } else if (path == "/report-cache") {
                            page += R"HTML(<script>
addEventListener('pageshow',event=>document.title='cache:'+(event.persisted?'restored':'fresh'));
</script>)HTML";
                        }
                    }
                    QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n";
                    if (path == "/sandbox") {
                        response += "Content-Security-Policy: sandbox allow-scripts\r\n";
                    } else if (path == "/report-sandbox") {
                        response += "Content-Security-Policy: sandbox allow-scripts allow-forms\r\n";
                    }
                    response += "Content-Length: " + QByteArray::number(page.size()) + "\r\n\r\n" + page;
                    socket->write(response);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                    m_requests.remove(socket);
                    socket->deleteLater();
                });
            }
        });
    }

  private:
    QHash<QTcpSocket *, QByteArray> m_requests;
};

inline void verifyAutofillTargets(
    eden::engine::EngineView &view,
    quint16 port,
    const std::function<void()> &createIsolatedWorld = {}
) {
    using eden::engine::AutofillTarget;
    const QUrl base(QString("http://127.0.0.1:%1/").arg(port));
    const auto request = [&view] {
        auto result = std::make_shared<std::optional<AutofillTarget>>();
        view.requestAutofillTarget([result](AutofillTarget target) { *result = std::move(target); });
        return result;
    };
    view.load(base.resolved(QUrl("first")));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("ready:/first"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    const auto first = request();
    QTRY_VERIFY_WITH_TIMEOUT(first->has_value(), 5000);
    QVERIFY(first->value().isValid());
    QCOMPARE(first->value().origin, eden::engine::autofillOrigin(base));
    if (createIsolatedWorld) {
        createIsolatedWorld();
        if (QTest::currentTestFailed()) {
            return;
        }
    }

    view.load(base.resolved(QUrl("first#state")));
    QTRY_COMPARE_WITH_TIMEOUT(view.url().fragment(), QString("state"), 5000);
    const auto sameDocument = request();
    QTRY_VERIFY_WITH_TIMEOUT(sameDocument->has_value(), 5000);
    QCOMPARE(sameDocument->value().documentId, first->value().documentId);
    view.fillCredential(first->value(), "saved-user", "saved-password");
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("saved-user|saved-password:4"), 5000);

    view.reload();
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("ready:/first"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    const auto reloaded = request();
    QTRY_VERIFY_WITH_TIMEOUT(reloaded->has_value(), 5000);
    QVERIFY(reloaded->value().isValid());
    QVERIFY(reloaded->value().documentId != first->value().documentId);
    view.fillCredential(first->value(), "stale-user", "stale-password");
    QTest::qWait(200);
    QCOMPARE(view.title(), QString("ready:/first"));

    QUrl crossOrigin = base.resolved(QUrl("second"));
    crossOrigin.setHost("localhost");
    view.load(crossOrigin);
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("ready:/second"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    view.fillCredential(reloaded->value(), "stale-user", "stale-password");
    QTest::qWait(200);
    QCOMPARE(view.title(), QString("ready:/second"));
    const auto second = request();
    QTRY_VERIFY_WITH_TIMEOUT(second->has_value(), 5000);
    QVERIFY(second->value().isValid());
    QVERIFY(second->value().origin != first->value().origin);
    view.fillCredential(second->value(), "second-user", "second-password");
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("second-user|second-password:4"), 5000);

    view.load(base.resolved(QUrl("forged")));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("ready:/forged"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    const auto forged = request();
    QTRY_VERIFY_WITH_TIMEOUT(forged->has_value(), 5000);
    QVERIFY(forged->value().isValid());
    QCOMPARE(forged->value().origin, eden::engine::autofillOrigin(base));
    QVERIFY(forged->value().documentId != "forged");
    AutofillTarget wrongOrigin = forged->value();
    wrongOrigin.origin = QUrl("https://forged.test");
    wrongOrigin.url = QUrl("https://forged.test/form");
    view.fillCredential(wrongOrigin, "wrong-user", "wrong-password");
    QTest::qWait(200);
    QCOMPARE(view.title(), QString("ready:/forged"));
    view.fillCredential(forged->value(), "real-user", "real-password");
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("real-user|real-password:4"), 5000);

    view.load(base.resolved(QUrl("sandbox")));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("ready:/sandbox"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    const auto sandbox = request();
    QTRY_VERIFY_WITH_TIMEOUT(sandbox->has_value(), 5000);
    QVERIFY(!sandbox->value().isValid());

    view.load(base.resolved(QUrl("iframe")));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("ready:/iframe"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    const auto parent = request();
    QTRY_VERIFY_WITH_TIMEOUT(parent->has_value(), 5000);
    QVERIFY(parent->value().isValid());
    view.fillCredential(parent->value(), "parent-user", "parent-password");
    QTest::qWait(200);
    QCOMPARE(view.title(), QString("ready:/iframe"));

    view.load(base.resolved(QUrl("pending-navigation")));
    view.fillCredential(parent->value(), "stale-user", "stale-password");
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("ready:/pending-navigation"), 15000);
    QTest::qWait(200);
    QCOMPARE(view.title(), QString("ready:/pending-navigation"));
    view.requestAutofillTarget([](AutofillTarget) {});
}

inline void verifyPersonalDataTargets(eden::engine::EngineView &view, quint16 port) {
    using eden::engine::AutofillTarget;
    const QUrl url(QString("http://127.0.0.1:%1/personal").arg(port));
    const QVariantMap fields{{"email", "person@example.test"}, {"addressLine1", "12 Test Street"}};
    const auto request = [&view] {
        auto result = std::make_shared<std::optional<AutofillTarget>>();
        view.requestAutofillTarget([result](AutofillTarget target) { *result = std::move(target); });
        return result;
    };
    view.load(url);
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("personal-ready"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    const auto first = request();
    QTRY_VERIFY_WITH_TIMEOUT(first->has_value(), 5000);
    QVERIFY(first->value().isValid());
    QUrl sameDocument = url;
    sameDocument.setFragment("state");
    view.load(sameDocument);
    QTRY_COMPARE(view.url().fragment(), QString("state"));
    view.fillForm(first->value(), fields);
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("person@example.test|12 Test Street:4"), 5000);
    view.reload();
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("personal-ready"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    view.fillForm(first->value(), fields);
    QTest::qWait(200);
    QCOMPARE(view.title(), QString("personal-ready"));
    const auto second = request();
    QTRY_VERIFY_WITH_TIMEOUT(second->has_value(), 5000);
    AutofillTarget wrongOrigin = second->value();
    wrongOrigin.origin = QUrl("https://forged.test");
    wrongOrigin.url = wrongOrigin.origin;
    view.fillForm(wrongOrigin, fields);
    QTest::qWait(200);
    QCOMPARE(view.title(), QString("personal-ready"));
    QUrl remote = url;
    remote.setHost("localhost");
    view.load(remote);
    QTRY_COMPARE_WITH_TIMEOUT(view.url(), remote, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    view.fillForm(second->value(), fields);
    QTest::qWait(200);
    QCOMPARE(view.title(), QString("personal-ready"));
    const auto third = request();
    QTRY_VERIFY_WITH_TIMEOUT(third->has_value(), 5000);
    view.fillForm(third->value(), fields);
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("person@example.test|12 Test Street:4"), 5000);
    view.load(url);
    view.fillForm(third->value(), fields);
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("personal-ready"), 15000);
    QTest::qWait(200);
    QCOMPARE(view.title(), QString("personal-ready"));
}

inline void verifyCookieIsolation(eden::engine::EngineView &view, eden::engine::EngineProfile &profile, quint16 port) {
    const QUrl base(QString("http://127.0.0.1:%1/").arg(port));
    view.load(base.resolved(QUrl("cookie-source")));
    QTRY_VERIFY_WITH_TIMEOUT(view.title().contains("ordinary=fixture"), 15000);
    QVERIFY(view.title().contains("partitioned=fixture"));
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    QVERIFY(!profile.supportsPortableCookies());
    auto snapshot = std::make_shared<std::optional<eden::engine::CookieSnapshotResult>>();
    profile.exportPortableCookies([snapshot](auto result) { *snapshot = std::move(result); });
    QTRY_VERIFY_WITH_TIMEOUT(snapshot->has_value(), 5000);
    QCOMPARE(snapshot->value().errorCode, QString("unsupported"));
    QVERIFY(snapshot->value().cookies.isEmpty());
    auto replacement = std::make_shared<std::optional<eden::engine::CookieReplaceResult>>();
    profile.replacePortableCookies({}, [replacement](auto result) { *replacement = std::move(result); });
    QTRY_VERIFY_WITH_TIMEOUT(replacement->has_value(), 5000);
    QCOMPARE(replacement->value().errorCode, QString("unsupported"));
    view.load(base.resolved(QUrl("cookie-check")));
    QTRY_COMPARE_WITH_TIMEOUT(view.url().path(), QString("/cookie-check"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    QVERIFY(view.title().contains("ordinary=fixture"));
    QVERIFY(view.title().contains("partitioned=fixture"));
}

inline void
verifyOrdinaryCookieHandoff(eden::engine::EngineView &view, eden::engine::EngineProfile &profile, quint16 port) {
    const QUrl base(QString("http://127.0.0.1:%1/").arg(port));
    view.load(base.resolved(QUrl("cookie-source")));
    QTRY_VERIFY_WITH_TIMEOUT(view.title().contains("ordinary=fixture"), 15000);
    QVERIFY(view.title().contains("partitioned=fixture"));
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    QVERIFY(profile.supportsPortableCookies());
    auto snapshot = std::make_shared<std::optional<eden::engine::CookieSnapshotResult>>();
    profile.exportPortableCookies([snapshot](auto result) { *snapshot = std::move(result); });
    QTRY_VERIFY_WITH_TIMEOUT(snapshot->has_value(), 10000);
    QVERIFY2(snapshot->value().errorCode.isEmpty(), qPrintable(snapshot->value().errorCode));
    QCOMPARE(snapshot->value().cookies.size(), 1);
    QCOMPARE(snapshot->value().cookies.first().name, QByteArray("ordinary"));
    auto replacement = std::make_shared<std::optional<eden::engine::CookieReplaceResult>>();
    profile.replacePortableCookies({}, [replacement](auto result) { *replacement = std::move(result); });
    QTRY_VERIFY_WITH_TIMEOUT(replacement->has_value(), 10000);
    QVERIFY2(replacement->value().errorCode.isEmpty(), qPrintable(replacement->value().errorCode));
    view.load(base.resolved(QUrl("cookie-check")));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("partitioned=fixture"), 15000);
    replacement->reset();
    profile.replacePortableCookies(snapshot->value().cookies, [replacement](auto result) {
        *replacement = std::move(result);
    });
    QTRY_VERIFY_WITH_TIMEOUT(replacement->has_value(), 10000);
    QVERIFY2(replacement->value().errorCode.isEmpty(), qPrintable(replacement->value().errorCode));
    view.reload();
    QTRY_VERIFY_WITH_TIMEOUT(view.title().contains("ordinary=fixture"), 15000);
    QVERIFY(view.title().contains("partitioned=fixture"));
    eden::engine::wipePortableCookies(snapshot->value().cookies);
}

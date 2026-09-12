#include "autofilltargetchecks.h"
#include "downloadchecks.h"
#include "engine/engineplugin.h"
#include "engine/engineprofile.h"
#include "engine/qtwebengine/qtwebengineview.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWebEngineProfile>
#include <QQuickWindow>
#include <QSequentialIterable>
#include <QtTest>

#include <memory>

extern "C" const eden::engine::EnginePluginApi *eden_engine_plugin();

class QtWebEngineUserAgentTest final : public QObject {
    Q_OBJECT

  private slots:
    void identityIsAppliedToEveryProfile();
    void credentialTargets();
    void ordinaryCookieHandoff();
    void mediaActivity();
    void concurrentDownloadsKeepDistinctDestinations();
    void formReports();
    void forgedConsoleReports();
    void isolatedFormReports_data();
    void isolatedFormReports();
    void formReportNavigation();
    void formReportAuthentication();
    void permissionDismissal();
};

void QtWebEngineUserAgentTest::forgedConsoleReports() {
    AutofillPageServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QQmlEngine engine;
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::QtWebEngine;
    parameters.privateProfile = true;
    std::unique_ptr<eden::engine::EngineProfile> profile(
        eden_engine_plugin()->createProfile(&parameters, &engine, nullptr)
    );
    QVERIFY(profile);
    QQuickWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQmlComponent component(&engine);
    component.setData("import QtQuick; Item { width:1000; height:700 }", QUrl());
    std::unique_ptr<QObject> viewport(component.create());
    auto *item = qobject_cast<QQuickItem *>(viewport.get());
    QVERIFY(item);
    item->setParentItem(window.contentItem());
    eden::engine::QtWebEngineView view(profile.get());
    view.attach(item);
    QSignalSpy reports(&view, &eden::engine::EngineView::formFieldFocused);
    QSignalSpy credentials(&view, &eden::engine::EngineView::credentialSubmitted);
    view.load(QUrl(QString("http://127.0.0.1:%1/report-forgery").arg(server.serverPort())));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("forgery-ready"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    QTest::qWait(100);
    QCOMPARE(credentials.size(), 0);
    QCOMPARE(reports.size(), 0);
}

void QtWebEngineUserAgentTest::isolatedFormReports_data() {
    QTest::addColumn<bool>("sandbox");
    QTest::newRow("main-world-overrides") << false;
    QTest::newRow("opaque-origin") << true;
}

void QtWebEngineUserAgentTest::isolatedFormReports() {
    QFETCH(bool, sandbox);
    AutofillPageServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QQmlEngine engine;
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::QtWebEngine;
    parameters.privateProfile = true;
    std::unique_ptr<eden::engine::EngineProfile> profile(
        eden_engine_plugin()->createProfile(&parameters, &engine, nullptr)
    );
    QVERIFY(profile);
    QQuickWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQmlComponent component(&engine);
    component.setData("import QtQuick; Item { width:1000; height:700 }", QUrl());
    std::unique_ptr<QObject> viewport(component.create());
    auto *item = qobject_cast<QQuickItem *>(viewport.get());
    QVERIFY(item);
    item->setParentItem(window.contentItem());
    eden::engine::QtWebEngineView view(profile.get());
    view.attach(item);
    QSignalSpy reports(&view, &eden::engine::EngineView::formFieldFocused);
    QSignalSpy credentials(&view, &eden::engine::EngineView::credentialSubmitted);
    const QUrl url(
        QString("http://127.0.0.1:%1/report-%2").arg(server.serverPort()).arg(sandbox ? "sandbox" : "world")
    );
    view.load(url);
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), sandbox ? QString("form-events") : QString("world:isolated"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(50, 75));
    QTest::keyClick(&window, Qt::Key_A);
    if (!sandbox) {
        QTRY_VERIFY_WITH_TIMEOUT(!reports.isEmpty(), 5000);
        QTRY_COMPARE(reports.constLast().at(0).toMap().value("value").toString(), QString("a"));
    }
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(50, 125));
    QTest::keyClick(&window, Qt::Key_X);
    if (!sandbox) {
        QTRY_COMPARE(reports.constLast().at(0).toMap().value("type").toString(), QString("password"));
        QCOMPARE(reports.constLast().at(0).toMap().value("value").toString(), QString());
    }
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(50, 175));
    if (sandbox) {
        QTest::qWait(150);
        QCOMPARE(credentials.size(), 0);
        QCOMPARE(reports.size(), 0);
    } else {
        QTRY_VERIFY_WITH_TIMEOUT(!credentials.isEmpty(), 5000);
        QTRY_COMPARE(
            credentials.constLast().at(0).value<eden::engine::CredentialSubmissionInfo>().password,
            QString("x")
        );
        const auto submission = credentials.constLast().at(0).value<eden::engine::CredentialSubmissionInfo>();
        QCOMPARE(submission.origin, eden::engine::autofillOrigin(url));
        QCOMPARE(submission.username, QString("a"));
        QCOMPARE(submission.password, QString("x"));
        QVERIFY(!reports.isEmpty());
    }
}

void QtWebEngineUserAgentTest::formReportNavigation() {
    AutofillPageServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QQmlEngine engine;
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::QtWebEngine;
    parameters.privateProfile = true;
    std::unique_ptr<eden::engine::EngineProfile> profile(
        eden_engine_plugin()->createProfile(&parameters, &engine, nullptr)
    );
    QVERIFY(profile);
    QQuickWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQmlComponent component(&engine);
    component.setData("import QtQuick; Item { width:1000; height:700 }", QUrl());
    std::unique_ptr<QObject> viewport(component.create());
    auto *item = qobject_cast<QQuickItem *>(viewport.get());
    QVERIFY(item);
    item->setParentItem(window.contentItem());
    eden::engine::QtWebEngineView view(profile.get());
    view.attach(item);
    QSignalSpy reports(&view, &eden::engine::EngineView::formFieldFocused);
    QSignalSpy credentials(&view, &eden::engine::EngineView::credentialSubmitted);
    QVERIFY(!item->childItems().isEmpty());
    QQuickItem *webItem = item->childItems().constFirst();
    QObject *settings = webItem->property("settings").value<QObject *>();
    QVERIFY(settings);
    const bool supportsBackForwardCache = settings->metaObject()->indexOfProperty("backForwardCacheEnabled") >= 0;
    if (supportsBackForwardCache) {
        QVERIFY(settings->setProperty("backForwardCacheEnabled", true));
    }
    const QUrl url(QString("http://127.0.0.1:%1/report-cache").arg(server.serverPort()));
    view.load(url);
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("cache:fresh"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(50, 75));
    QTest::keyClick(&window, Qt::Key_A);
    QTRY_VERIFY_WITH_TIMEOUT(!reports.isEmpty(), 5000);
    QTRY_COMPARE(reports.constLast().at(0).toMap().value("value").toString(), QString("a"));
    const auto requestTarget = [&view] {
        auto result = std::make_shared<std::optional<eden::engine::AutofillTarget>>();
        view.requestAutofillTarget([result](eden::engine::AutofillTarget target) { *result = std::move(target); });
        return result;
    };
    const auto first = requestTarget();
    QTRY_VERIFY(first->has_value());
    QVERIFY(first->value().isValid());
    QUrl fragment = url;
    fragment.setFragment("section");
    view.load(fragment);
    QTRY_COMPARE(view.url(), fragment);
    QVERIFY(
        QMetaObject::invokeMethod(
            webItem,
            "edenRunJavaScript",
            Q_ARG(QVariant, QString("history.pushState({}, '', '#state')"))
        )
    );
    QTRY_COMPARE(view.url().fragment(), QString("state"));
    const auto sameDocument = requestTarget();
    QTRY_VERIFY(sameDocument->has_value());
    QCOMPARE(sameDocument->value().documentId, first->value().documentId);
    QTest::keyClick(&window, Qt::Key_B);
    QTRY_COMPARE(reports.constLast().at(0).toMap().value("value").toString(), QString("ab"));
    if (!supportsBackForwardCache) {
        QSKIP("This Qt version does not expose back-forward cache support");
    }
    view.load(url.resolved(QUrl("different")));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("ready:/different"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    view.back();
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("cache:restored"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    const auto restored = requestTarget();
    QTRY_VERIFY(restored->has_value());
    QCOMPARE(restored->value().documentId, first->value().documentId);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(50, 75));
    QTest::keyClick(&window, Qt::Key_End);
    QTest::keyClick(&window, Qt::Key_C);
    QTRY_COMPARE_WITH_TIMEOUT(reports.constLast().at(0).toMap().value("value").toString(), QString("abc"), 5000);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(50, 125));
    QTest::keyClick(&window, Qt::Key_X);
    QTRY_COMPARE(reports.constLast().at(0).toMap().value("type").toString(), QString("password"));
    QCOMPARE(reports.constLast().at(0).toMap().value("value").toString(), QString());
    credentials.clear();
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(50, 175));
    QTRY_VERIFY(!credentials.isEmpty());
    const auto submission = credentials.constLast().at(0).value<eden::engine::CredentialSubmissionInfo>();
    QCOMPARE(submission.username, QString("abc"));
    QCOMPARE(submission.password, QString("x"));
    QCOMPARE(submission.origin, eden::engine::autofillOrigin(url));
}

void QtWebEngineUserAgentTest::formReportAuthentication() {
    AutofillPageServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QQmlEngine engine;
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::QtWebEngine;
    parameters.privateProfile = true;
    std::unique_ptr<eden::engine::EngineProfile> profile(
        eden_engine_plugin()->createProfile(&parameters, &engine, nullptr)
    );
    QVERIFY(profile);
    QQuickWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQmlComponent component(&engine);
    component.setData("import QtQuick; Item { width:1000; height:700 }", QUrl());
    std::unique_ptr<QObject> viewport(component.create());
    auto *item = qobject_cast<QQuickItem *>(viewport.get());
    QVERIFY(item);
    item->setParentItem(window.contentItem());
    eden::engine::QtWebEngineView view(profile.get());
    view.attach(item);
    QSignalSpy reports(&view, &eden::engine::EngineView::formFieldFocused);
    QSignalSpy credentials(&view, &eden::engine::EngineView::credentialSubmitted);
    const QUrl url(QString("http://127.0.0.1:%1/form-events").arg(server.serverPort()));
    view.load(url);
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("form-events"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(50, 75));
    QTest::keyClick(&window, Qt::Key_A);
    QTRY_VERIFY_WITH_TIMEOUT(!reports.isEmpty(), 5000);
    QObject *bridge = nullptr;
    for (QObject *child : view.children()) {
        if (child->metaObject()->indexOfMethod("registerDocument(QString,QString)") >= 0) {
            bridge = child;
            break;
        }
    }
    QVERIFY(bridge);
    const QString origin = eden::engine::autofillOrigin(url).toString(QUrl::FullyEncoded);
    QVariantMap registration;
    QVERIFY(
        QMetaObject::invokeMethod(
            bridge,
            "registerDocument",
            Qt::DirectConnection,
            Q_RETURN_ARG(QVariantMap, registration),
            Q_ARG(QString, origin),
            Q_ARG(QString, QString())
        )
    );
    QVERIFY(!registration.value("token").toString().isEmpty());
    const QString token = registration.value("token").toString();
    const QString document = registration.value("documentId").toString();
    const auto submit = [&](const QString &reportToken,
                            const QString &reportDocument,
                            const QString &reportOrigin,
                            const QString &kind,
                            const QVariantList &values) {
        return QMetaObject::invokeMethod(
            bridge,
            "report",
            Qt::DirectConnection,
            Q_ARG(QString, reportToken),
            Q_ARG(QString, reportDocument),
            Q_ARG(QString, reportOrigin),
            Q_ARG(QString, kind),
            Q_ARG(QVariantList, values)
        );
    };
    credentials.clear();
    reports.clear();
    const QVariantList valid{QString("fixture-user"), QString("fixture-password")};
    QVERIFY(submit("incorrect", document, origin, "credential", valid));
    QVERIFY(submit(token, "incorrect", origin, "credential", valid));
    QVERIFY(submit(token, document, "https://forged.test", "credential", valid));
    QVERIFY(submit(token, document, origin, "credential", {5, 6}));
    QVERIFY(credentials.isEmpty());
    QVERIFY(submit(token, document, origin, "credential", valid));
    QCOMPARE(credentials.size(), 1);
    QVERIFY(
        submit(token, document, origin, "field", {"password", "otp", "one-time-code", "must-stay-hidden", 1, 2, 3, 4})
    );
    QCOMPARE(reports.size(), 1);
    QCOMPARE(reports.constLast().at(0).toMap().value("value").toString(), QString());
    QVERIFY(!item->childItems().isEmpty());
    QQuickItem *webItem = item->childItems().constFirst();
    const QString createFrames = QStringLiteral(R"JS((function(){
        var loaded=0, attempted=0;
        addEventListener('message',function(event){
            if(event.data==='channel-report-attempted')document.title='channel-attempts:'+(++attempted);
        });
        for(var index=0;index<2;++index){
            var frame=document.createElement('iframe');
            var url=new URL('/child',location.href);
            if(index)url.hostname='localhost';
            frame.src=url.href;
            frame.style='position:absolute;left:600px;top:'+(300+index*150)+'px;width:200px;height:100px';
            frame.onload=function(){if(++loaded===2)document.title='channel-frames-ready';};
            document.body.append(frame);
        }
    })();)JS");
    QVERIFY(QMetaObject::invokeMethod(webItem, "edenRunJavaScript", Q_ARG(QVariant, createFrames)));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("channel-frames-ready"), 15000);
    const QVariant mainFrame = webItem->property("mainFrame");
    const QMetaObject *frameMetaObject = mainFrame.metaType().metaObject();
    QVERIFY(frameMetaObject);
    const int childrenIndex = frameMetaObject->indexOfProperty("children");
    QVERIFY(childrenIndex >= 0);
    const QVariant children = frameMetaObject->property(childrenIndex).readOnGadget(mainFrame.constData());
    const QSequentialIterable frames = children.value<QSequentialIterable>();
    QCOMPARE(frames.size(), 2);
    const int reportMethod =
        bridge->metaObject()->indexOfMethod("report(QString,QString,QString,QString,QVariantList)");
    QVERIFY(reportMethod >= 0);
    const QJsonObject forged{
        {"type", 6},
        {"id", 900001},
        {"object", "forms"},
        {"method", reportMethod},
        {"args", QJsonArray{token, document, origin, "credential", QJsonArray{"frame-user", "frame-password"}}}
    };
    const QString packet = QString::fromUtf8(QJsonDocument(forged).toJson(QJsonDocument::Compact));
    const auto runInFrame = [](QVariant frame, const QString &script) {
        const QMetaObject *metaObject = frame.metaType().metaObject();
        if (!metaObject) {
            return false;
        }
        for (int index = 0; index < metaObject->methodCount(); ++index) {
            const QMetaMethod method = metaObject->method(index);
            if (method.name() == "runJavaScript" && method.parameterCount() == 2 &&
                method.parameterMetaType(1).id() == QMetaType::UInt) {
                return method.invokeOnGadget(frame.data(), Q_ARG(QString, script), Q_ARG(quint32, 1));
            }
        }
        return false;
    };
    const QString mainScript = QStringLiteral(R"JS((function(){
        var transport=qt.webChannelTransport, receive=transport.onmessage;
        transport.onmessage=function(event){
            var message=typeof event.data==='string'?JSON.parse(event.data):event.data;
            if(message.type===10&&message.id===900001){transport.onmessage=receive;return;}
            receive(event);
        };
        transport.send(JSON.stringify(%1));
    })();)JS")
                                   .arg(packet);
    QVERIFY(runInFrame(mainFrame, mainScript));
    QTRY_COMPARE_WITH_TIMEOUT(credentials.size(), 2, 5000);
    QCOMPARE(
        credentials.constLast().at(0).value<eden::engine::CredentialSubmissionInfo>().username,
        QString("frame-user")
    );
    credentials.clear();
    const QString frameScript =
        QStringLiteral(
            "if(globalThis.qt&&qt.webChannelTransport){qt.webChannelTransport.send(JSON.stringify(%1));}"
            "parent.postMessage('channel-report-attempted','*');"
        )
            .arg(packet);
    for (QVariant frame : frames) {
        QVERIFY(runInFrame(frame, frameScript));
    }
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("channel-attempts:2"), 5000);
    QTest::qWait(100);
    QVERIFY(credentials.isEmpty());
    QVariantMap replacement;
    QVERIFY(
        QMetaObject::invokeMethod(
            bridge,
            "registerDocument",
            Qt::DirectConnection,
            Q_RETURN_ARG(QVariantMap, replacement),
            Q_ARG(QString, origin),
            Q_ARG(QString, document)
        )
    );
    QVERIFY(replacement.value("token").toString() != token);
    QVERIFY(submit(token, document, origin, "credential", valid));
    QVERIFY(credentials.isEmpty());
    QUrl other = url;
    other.setHost("localhost");
    other.setPath("/report-cache");
    view.load(other);
    QCOMPARE(view.url().host(), QString("localhost"));
    credentials.clear();
    QVERIFY(submit(replacement.value("token").toString(), document, origin, "credential", valid));
    QCOMPARE(credentials.size(), 1);
    QCOMPARE(credentials.first().at(0).value<eden::engine::CredentialSubmissionInfo>().origin, QUrl(origin));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("cache:fresh"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    credentials.clear();
    QVERIFY(submit(replacement.value("token").toString(), document, origin, "credential", valid));
    QCOMPARE(credentials.size(), 0);
}

void QtWebEngineUserAgentTest::permissionDismissal() {
    AutofillPageServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QQmlEngine engine;
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::QtWebEngine;
    parameters.privateProfile = true;
    std::unique_ptr<eden::engine::EngineProfile> profile(
        eden_engine_plugin()->createProfile(&parameters, &engine, nullptr)
    );
    QVERIFY(profile);
    QQuickWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQmlComponent component(&engine);
    component.setData("import QtQuick; Item { width:1000; height:700 }", QUrl());
    std::unique_ptr<QObject> viewport(component.create());
    auto *item = qobject_cast<QQuickItem *>(viewport.get());
    QVERIFY(item);
    item->setParentItem(window.contentItem());
    eden::engine::QtWebEngineView view(profile.get());
    view.attach(item);
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

void QtWebEngineUserAgentTest::formReports() {
    AutofillPageServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QQmlEngine engine;
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::QtWebEngine;
    parameters.privateProfile = true;
    std::unique_ptr<eden::engine::EngineProfile> profile(
        eden_engine_plugin()->createProfile(&parameters, &engine, nullptr)
    );
    QVERIFY(profile);
    QQuickWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQmlComponent component(&engine);
    component.setData("import QtQuick; Item { width:1000; height:700 }", QUrl());
    std::unique_ptr<QObject> viewport(component.create());
    auto *item = qobject_cast<QQuickItem *>(viewport.get());
    QVERIFY(item);
    item->setParentItem(window.contentItem());
    eden::engine::QtWebEngineView view(profile.get());
    view.attach(item);
    QSignalSpy reports(&view, &eden::engine::EngineView::formFieldFocused);
    QSignalSpy credentials(&view, &eden::engine::EngineView::credentialSubmitted);
    const QUrl url(QString("http://127.0.0.1:%1/form-events").arg(server.serverPort()));
    view.load(url);
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("form-events"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(50, 225));
    QTRY_COMPARE(view.title(), QString("burst-complete"));
    QTest::qWait(100);
    QCOMPARE(reports.size(), 0);
    QCOMPARE(credentials.size(), 0);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(50, 75));
    QTest::keyClick(&window, Qt::Key_A);
    QTRY_VERIFY(!reports.isEmpty());
    QTRY_COMPARE(reports.constLast().at(0).toMap().value("value").toString(), QString("a"));
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(50, 125));
    QTest::keyClick(&window, Qt::Key_X);
    QTRY_COMPARE(reports.constLast().at(0).toMap().value("type").toString(), QString("password"));
    QCOMPARE(reports.constLast().at(0).toMap().value("value").toString(), QString());
    credentials.clear();
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(50, 175));
    QTRY_VERIFY(!credentials.isEmpty());
    QTRY_COMPARE(credentials.constLast().at(0).value<eden::engine::CredentialSubmissionInfo>().password, QString("x"));
    const auto submission = credentials.constLast().at(0).value<eden::engine::CredentialSubmissionInfo>();
    QCOMPARE(submission.username, QString("a"));
    QCOMPARE(submission.password, QString("x"));
    QCOMPARE(submission.origin, eden::engine::autofillOrigin(url));
    QTRY_VERIFY(reports.constLast().at(0).toMap().isEmpty());
    const qsizetype reportsBeforeOrdinaryInput = reports.size();
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(50, 25));
    QTest::keyClick(&window, Qt::Key_B);
    QTest::qWait(100);
    QCOMPARE(reports.size(), reportsBeforeOrdinaryInput);
}

void QtWebEngineUserAgentTest::concurrentDownloadsKeepDistinctDestinations() {
    QQmlEngine engine;
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::QtWebEngine;
    parameters.privateProfile = true;
    std::unique_ptr<eden::engine::EngineProfile> profile(
        eden_engine_plugin()->createProfile(&parameters, &engine, nullptr)
    );
    QVERIFY(profile);
    QQuickWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQmlComponent component(&engine);
    component.setData("import QtQuick; Item { width:1000; height:700 }", QUrl());
    std::unique_ptr<QObject> viewport(component.create());
    auto *item = qobject_cast<QQuickItem *>(viewport.get());
    QVERIFY(item);
    item->setParentItem(window.contentItem());
    eden::engine::QtWebEngineView first(profile.get());
    eden::engine::QtWebEngineView second(profile.get());
    first.attach(item);
    second.attach(item);
    verifyConcurrentDownloads(*profile, first, second);
}

void QtWebEngineUserAgentTest::mediaActivity() {
    AutofillPageServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QQmlEngine engine;
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::QtWebEngine;
    parameters.privateProfile = true;
    std::unique_ptr<eden::engine::EngineProfile> profile(
        eden_engine_plugin()->createProfile(&parameters, &engine, nullptr)
    );
    QVERIFY(profile);
    QQuickWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQmlComponent component(&engine);
    component.setData("import QtQuick; Item { width:1000; height:700 }", QUrl());
    std::unique_ptr<QObject> viewport(component.create());
    auto *item = qobject_cast<QQuickItem *>(viewport.get());
    QVERIFY(item);
    item->setParentItem(window.contentItem());
    eden::engine::QtWebEngineView view(profile.get());
    view.attach(item);
    connect(&view, &eden::engine::EngineView::permissionRequested, &view, [&view](const auto &request) {
        view.resolvePermissionRequest(request.id, !request.permissions.contains("screen sharing"));
    });
    view.load(QUrl(QString("http://127.0.0.1:%1/capture-guard").arg(server.serverPort())));
    QTRY_VERIFY_WITH_TIMEOUT(view.title().startsWith("legacy:"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isLoading(), 15000);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(80, 35));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("call-active"), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(view.activityIndicators().size(), 2, 5000);
    QCOMPARE(view.activityIndicators().at(0).toMap().value("icon").toString(), QString("camera"));
    QCOMPARE(view.activityIndicators().at(1).toMap().value("icon").toString(), QString("microphone"));
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(80, 100));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("call-stopped"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.isCapturing(), 5000);
    QTest::mouseClick(&window, Qt::LeftButton, {}, QPoint(80, 35));
    QTRY_COMPARE_WITH_TIMEOUT(view.title(), QString("call-active"), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(view.isCapturing(), 5000);
    view.load(QUrl("about:blank"));
    QTRY_VERIFY_WITH_TIMEOUT(!view.isCapturing(), 5000);
}

void QtWebEngineUserAgentTest::ordinaryCookieHandoff() {
    AutofillPageServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QTemporaryDir directory;
    QQmlEngine engine;
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::QtWebEngine;
    parameters.profileId = "cookie-test";
    parameters.dataPath = directory.filePath("data");
    parameters.cachePath = directory.filePath("cache");
    std::unique_ptr<eden::engine::EngineProfile> profile(
        eden_engine_plugin()->createProfile(&parameters, &engine, nullptr)
    );
    QVERIFY(profile);
    QQuickWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQmlComponent component(&engine);
    component.setData("import QtQuick; Item { width:1000; height:700 }", QUrl());
    std::unique_ptr<QObject> viewport(component.create());
    auto *item = qobject_cast<QQuickItem *>(viewport.get());
    QVERIFY(item);
    item->setParentItem(window.contentItem());
    eden::engine::QtWebEngineView view(profile.get());
    view.attach(item);
    verifyOrdinaryCookieHandoff(view, *profile, server.serverPort());
}

void QtWebEngineUserAgentTest::credentialTargets() {
    AutofillPageServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QQmlEngine engine;
    eden::engine::EngineProfileParameters parameters;
    parameters.backend = eden::engine::Backend::QtWebEngine;
    parameters.privateProfile = true;
    std::unique_ptr<eden::engine::EngineProfile> profile(
        eden_engine_plugin()->createProfile(&parameters, &engine, nullptr)
    );
    QVERIFY(profile);
    QQuickWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQmlComponent component(&engine);
    component.setData("import QtQuick; Item { width:1000; height:700 }", QUrl());
    std::unique_ptr<QObject> viewport(component.create());
    auto *item = qobject_cast<QQuickItem *>(viewport.get());
    QVERIFY(item);
    item->setParentItem(window.contentItem());
    eden::engine::QtWebEngineView view(profile.get());
    view.attach(item);
    verifyAutofillTargets(view, server.serverPort());
    if (!QTest::currentTestFailed()) {
        verifyPersonalDataTargets(view, server.serverPort());
        verifyCookieIsolation(view, *profile, server.serverPort());
    }
}

void QtWebEngineUserAgentTest::identityIsAppliedToEveryProfile() {
    const eden::engine::EnginePluginApi *api = eden_engine_plugin();
    QVERIFY(api);

    const eden::engine::EngineIdentity identity{"Eden", "0.3.0"};
    QVERIFY(api->initialize(0, nullptr, &identity));

    QQuickWebEngineProfile *defaultProfile = QQuickWebEngineProfile::defaultProfile();
    QVERIFY(defaultProfile);
    const QString userAgent = defaultProfile->httpUserAgent();
    QVERIFY(userAgent.contains("Chrome/"));
    QVERIFY(userAgent.endsWith(" Eden/0.3.0"));
    QCOMPARE(userAgent.count("Eden/0.3.0"), 1);

    QQmlEngine qmlEngine;
    QTemporaryDir storageDirectory;
    QVERIFY(storageDirectory.isValid());
    eden::engine::EngineProfileParameters regularParameters;
    regularParameters.profileId = QStringLiteral("11111111-2222-3333-4444-555555555555");
    regularParameters.backend = eden::engine::Backend::QtWebEngine;
    regularParameters.dataPath = storageDirectory.path() + "/data";
    regularParameters.cachePath = storageDirectory.path() + "/cache";
    eden::engine::EngineProfileParameters privateParameters;
    privateParameters.backend = eden::engine::Backend::QtWebEngine;
    privateParameters.privateProfile = true;
    std::unique_ptr<eden::engine::EngineProfile> regularProfile(
        api->createProfile(&regularParameters, &qmlEngine, nullptr)
    );
    std::unique_ptr<eden::engine::EngineProfile> privateProfile(
        api->createProfile(&privateParameters, &qmlEngine, nullptr)
    );
    QVERIFY(regularProfile);
    QVERIFY(privateProfile);

    auto *regularNativeProfile = qobject_cast<QQuickWebEngineProfile *>(regularProfile->nativeProfile());
    auto *privateNativeProfile = qobject_cast<QQuickWebEngineProfile *>(privateProfile->nativeProfile());
    QVERIFY(regularNativeProfile);
    QVERIFY(privateNativeProfile);
    QCOMPARE(regularNativeProfile->httpUserAgent(), userAgent);
    QCOMPARE(privateNativeProfile->httpUserAgent(), userAgent);
}

int main(int argc, char *argv[]) {
    QTemporaryDir environmentDirectory;
    if (!environmentDirectory.isValid()) {
        return 1;
    }
    const QString downloadDirectory = environmentDirectory.filePath("Downloads");
    if (!QDir().mkpath(downloadDirectory)) {
        return 1;
    }
    QFile userDirectories(environmentDirectory.filePath("user-dirs.dirs"));
    if (!userDirectories.open(QIODevice::WriteOnly)) {
        return 1;
    }
    const QByteArray downloadSetting = QString("XDG_DOWNLOAD_DIR=\"%1\"\n").arg(downloadDirectory).toUtf8();
    if (userDirectories.write(downloadSetting) != downloadSetting.size()) {
        return 1;
    }
    userDirectories.close();
    qputenv("XDG_CONFIG_HOME", environmentDirectory.path().toUtf8());
    const eden::engine::EnginePluginApi *api = eden_engine_plugin();
    if (!api || !api->prepareApplication || !api->prepareApplication(argc, argv)) {
        return 1;
    }
    QGuiApplication application(argc, argv);
    QtWebEngineUserAgentTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "qtwebengineuseragent_test.moc"

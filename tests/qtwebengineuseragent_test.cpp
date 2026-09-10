#include "autofilltargetchecks.h"
#include "engine/engineplugin.h"
#include "engine/engineprofile.h"
#include "engine/qtwebengine/qtwebengineview.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWebEngineProfile>
#include <QQuickWindow>
#include <QtTest>

#include <memory>

extern "C" const eden::engine::EnginePluginApi *eden_engine_plugin();

class QtWebEngineUserAgentTest final : public QObject {
    Q_OBJECT

  private slots:
    void identityIsAppliedToEveryProfile();
    void credentialTargets();
    void formReports();
};

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
    const eden::engine::EnginePluginApi *api = eden_engine_plugin();
    if (!api || !api->prepareApplication || !api->prepareApplication(argc, argv)) {
        return 1;
    }
    QGuiApplication application(argc, argv);
    QtWebEngineUserAgentTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "qtwebengineuseragent_test.moc"

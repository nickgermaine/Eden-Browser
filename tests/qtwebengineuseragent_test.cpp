#include "engine/engineplugin.h"
#include "engine/engineprofile.h"

#include <QQmlEngine>
#include <QQuickWebEngineProfile>
#include <QtTest>

#include <memory>

extern "C" const eden::engine::EnginePluginApi *eden_engine_plugin();

class QtWebEngineUserAgentTest final : public QObject {
    Q_OBJECT

  private slots:
    void identityIsAppliedToEveryProfile();
};

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
    std::unique_ptr<eden::engine::EngineProfile> regularProfile(api->createProfile(false, &qmlEngine, nullptr));
    std::unique_ptr<eden::engine::EngineProfile> privateProfile(api->createProfile(true, &qmlEngine, nullptr));
    QVERIFY(regularProfile);
    QVERIFY(privateProfile);

    auto *regularNativeProfile = qobject_cast<QQuickWebEngineProfile *>(regularProfile->nativeProfile());
    auto *privateNativeProfile = qobject_cast<QQuickWebEngineProfile *>(privateProfile->nativeProfile());
    QVERIFY(regularNativeProfile);
    QVERIFY(privateNativeProfile);
    QCOMPARE(regularNativeProfile->httpUserAgent(), userAgent);
    QCOMPARE(privateNativeProfile->httpUserAgent(), userAgent);
}

QTEST_MAIN(QtWebEngineUserAgentTest)

#include "qtwebengineuseragent_test.moc"

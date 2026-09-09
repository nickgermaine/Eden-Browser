#include "engine/engineplugin.h"
#include "engine/engineprofile.h"

#include <QGuiApplication>
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

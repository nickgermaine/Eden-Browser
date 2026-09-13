#include <libsecret/secret.h>

#include "core/profiles/enginestorage.h"
#include "core/profiles/profilecontext.h"
#include "core/profiles/profileeditorcontroller.h"
#include "core/profiles/profilelistmodel.h"
#include "core/profiles/profilemanager.h"
#include "core/profiles/profileregistry.h"
#include "core/profiles/profilesettings.h"
#include "core/profiles/sessionstore.h"
#include "core/profiles/windowregistry.h"
#include "core/settings/settingsstore.h"
#include "core/settings/theme/thememanager.h"
#include "core/window/tabmodel.h"
#include "core/window/tabstripnavigator.h"
#include "core/window/windowcontroller.h"
#include "core/window/windowframe.h"
#include "engine/engineregistry.h"
#include "passwords/credentialvault.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

static bool failKeyringLookup = false;

extern "C" gchar *secret_password_lookup_sync(const SecretSchema *, GCancellable *, GError **error, ...) {
    if (failKeyringLookup) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Fixture keyring lookup failure");
        return nullptr;
    }
    return g_strdup(QByteArray(32, 's').toBase64().constData());
}

extern "C" gboolean secret_password_store_sync(
    const SecretSchema *,
    const gchar *,
    const gchar *,
    const gchar *,
    GCancellable *,
    GError **,
    ...
) {
    qFatal("The activation tests must not create keyring entries");
}

extern "C" void secret_password_free(gchar *password) {
    g_free(password);
}

static bool writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

static QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

class ProfileActivationTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanup();
    void cleanupTestCase();
    void launchRequestWaitsForStartup();
    void controllerRejectsUnavailableStorage_data();
    void controllerRejectsUnavailableStorage();
    void managerRecoversBeforeOpeningWindows_data();
    void managerRecoversBeforeOpeningWindows();
    void recoveryCanChooseAnotherProfile();
    void privateStartupRetryPreservesMode();
    void windowReportsSaveFailures();
    void queuedLaunchesOpenSeparateWindows();
    void startupWindowShowsProgress();

  private:
    std::optional<eden::core::ProfileRecord> createRecord();
    eden::core::ProfilePaths pathsFor(const eden::core::ProfileRecord &record) const;

    QTemporaryDir m_data;
    QTemporaryDir m_cache;
    std::unique_ptr<eden::core::ProfileRegistry> m_registry;
    std::unique_ptr<eden::core::WindowRegistry> m_windows;
    std::unique_ptr<eden::core::ProfileManager> m_manager;
    std::unique_ptr<QQmlEngine> m_engine;
};

void ProfileActivationTest::initTestCase() {
    QVERIFY(m_data.isValid());
    QVERIFY(m_cache.isValid());
    m_registry = std::make_unique<eden::core::ProfileRegistry>(m_data.filePath("profiles.sqlite"));
    QSignalSpy loaded(m_registry.get(), &eden::core::ProfileRegistry::loadFinished);
    m_registry->loadAsync();
    QTRY_COMPARE(loaded.size(), 1);
    m_windows = std::make_unique<eden::core::WindowRegistry>();
    m_manager = std::make_unique<eden::core::ProfileManager>(
        m_registry.get(),
        m_windows.get(),
        eden::core::ProfilePaths::Roots{m_data.path(), m_cache.path()}
    );
    qmlRegisterType<eden::core::WindowController>("Eden.Ui", 1, 0, "WindowController");
    qmlRegisterType<eden::core::WindowFrame>("Eden.Ui", 1, 0, "WindowFrame");
    qmlRegisterType<eden::core::TabStripNavigator>("Eden.Ui", 1, 0, "TabStripNavigator");
    qmlRegisterUncreatableType<eden::engine::EngineView>("Eden.Ui", 1, 0, "EngineView", "Owned by the controller");
    qmlRegisterUncreatableType<eden::core::ProfileListModel>(
        "Eden.Ui",
        1,
        0,
        "ProfileListModel",
        "Owned by the manager"
    );
    qmlRegisterUncreatableType<eden::core::ProfileEditorController>(
        "Eden.Ui",
        1,
        0,
        "ProfileEditorController",
        "Owned by the manager"
    );
    qmlRegisterUncreatableType<eden::core::ProfileSettings>("Eden.Ui", 1, 0, "ProfileSettings", "Owned by the profile");
    qmlRegisterSingletonInstance("Eden.Ui", 1, 0, "Engines", eden::engine::EngineRegistry::instance());
    qmlRegisterSingletonInstance("Eden.Ui", 1, 0, "Settings", eden::core::SettingsStore::instance());
    qmlRegisterSingletonInstance("Eden.Ui", 1, 0, "Themes", eden::core::ThemeManager::instance());
    qmlRegisterSingletonInstance("Eden.Ui", 1, 0, "Profiles", m_manager.get());
    m_engine = std::make_unique<QQmlEngine>();
    m_engine->addImportPath(QCoreApplication::applicationDirPath());
    m_manager->setQmlEngine(m_engine.get());
}

void ProfileActivationTest::cleanup() {
    failKeyringLookup = false;
    for (QWindow *window : QGuiApplication::topLevelWindows()) {
        delete window;
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(m_windows->allControllers().size(), 0);
}

void ProfileActivationTest::cleanupTestCase() {
    m_manager->setQmlEngine(nullptr);
    m_engine.reset();
    m_manager.reset();
    m_windows.reset();
    m_registry.reset();
    eden::core::EngineStorage::instance()->shutdown();
    QFile::setPermissions(m_data.filePath("engine-data"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QFile::setPermissions(m_cache.filePath("profiles"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

std::optional<eden::core::ProfileRecord> ProfileActivationTest::createRecord() {
    eden::core::ProfileRecord record;
    record.id = eden::core::ProfileId::generate();
    record.displayName = "Activation fixture";
    bool finished = false;
    eden::core::ProfileError result = eden::core::ProfileError::None;
    const auto completion = [&](eden::core::ProfileError error) {
        result = error;
        finished = true;
    };
    m_registry->createProfile(record, completion);
    if (!QTest::qWaitFor([&] { return finished; }, 5000) || result != eden::core::ProfileError::None) {
        return {};
    }
    finished = false;
    m_registry->commitProfileReady(record.id, {}, completion);
    if (!QTest::qWaitFor([&] { return finished; }, 5000) || result != eden::core::ProfileError::None) {
        return {};
    }
    record.lifecycle = eden::core::ProfileLifecycle::Ready;
    return record;
}

eden::core::ProfilePaths ProfileActivationTest::pathsFor(const eden::core::ProfileRecord &record) const {
    return {{m_data.path(), m_cache.path()}, record.id};
}

void ProfileActivationTest::controllerRejectsUnavailableStorage_data() {
    QTest::addColumn<QString>("failure");
    QTest::newRow("keyring") << QString("keyring");
    QTest::newRow("database") << QString("database");
    QTest::newRow("settings") << QString("settings");
}

void ProfileActivationTest::launchRequestWaitsForStartup() {
    QCOMPARE(m_manager->startupState(), QString("loading"));
    QVERIFY(m_manager->requestWindow({true, {}, {QUrl("eden://settings/privacy")}}));
    QTest::qWait(20);
    QCOMPARE(m_manager->browserWindowCount(), 0);
    const auto record = createRecord();
    QVERIFY(record);
    m_manager->activateProfile(record->id.toString());
    QTRY_COMPARE(m_manager->browserWindowCount(), 2);
    const auto context = m_manager->contextFor(record->id.toString());
    QVERIFY(context);
    QCOMPARE(context->windows().size(), 2);
    int privateWindows = 0;
    for (auto *controller : context->windows()) {
        if (controller->isPrivateWindow()) {
            ++privateWindows;
            QCOMPARE(controller->tabs()->rowCount(), 1);
            QCOMPARE(
                controller->tabs()->data(controller->tabs()->index(0), eden::core::TabModel::UrlRole).toUrl(),
                QUrl("eden://settings/privacy")
            );
        }
    }
    QCOMPARE(privateWindows, 1);
}

void ProfileActivationTest::controllerRejectsUnavailableStorage() {
    QFETCH(QString, failure);
    const auto record = createRecord();
    QVERIFY(record);
    const auto paths = pathsFor(*record);
    const auto context = eden::core::ProfileContext::create(*record, paths, m_engine.get(), nullptr);
    QVERIFY(context);
    QString damagedPath;
    if (failure == "keyring") {
        failKeyringLookup = true;
    } else {
        damagedPath = failure == "database" ? paths.databasePath() : paths.settingsPath();
        QVERIFY(writeFile(damagedPath, "damaged fixture"));
    }
    eden::core::WindowController controller;
    controller.initialize(context, false, {}, false);
    QVERIFY(!context->isActivated());
    QVERIFY(!controller.isInitialized());
    QVERIFY(!controller.tabs());
    QVERIFY(!controller.profileContext());
    QVERIFY(context->windows().isEmpty());
    QVERIFY(m_windows->allControllers().isEmpty());
    failKeyringLookup = false;
    if (!damagedPath.isEmpty()) {
        QCOMPARE(readFile(damagedPath), QByteArray("damaged fixture"));
        QVERIFY(QFile::remove(damagedPath));
    }
    controller.initialize(context, false, {}, false);
    QVERIFY(context->isActivated());
    QVERIFY(controller.isInitialized());
    QVERIFY(controller.tabs());
    QCOMPARE(context->windows().size(), 1);
    controller.prepareToClose();
}

void ProfileActivationTest::managerRecoversBeforeOpeningWindows_data() {
    controllerRejectsUnavailableStorage_data();
    QTest::newRow("session") << QString("session");
}

void ProfileActivationTest::managerRecoversBeforeOpeningWindows() {
    QFETCH(QString, failure);
    const auto record = createRecord();
    QVERIFY(record);
    const auto paths = pathsFor(*record);
    QVERIFY(paths.ensureBaseDirectories());
    QString damagedPath;
    if (failure == "keyring") {
        failKeyringLookup = true;
    } else {
        damagedPath = failure == "database"   ? paths.databasePath()
                      : failure == "settings" ? paths.settingsPath()
                                              : paths.sessionPath();
        QVERIFY(writeFile(damagedPath, "damaged fixture"));
    }
    QSignalSpy opened(m_manager.get(), &eden::core::ProfileManager::profileOpened);
    m_manager->activateProfile(record->id.toString());
    QCOMPARE(m_manager->startupState(), QString("recovery"));
    QVERIFY(!m_manager->recoveryMessage().isEmpty());
    QVERIFY(m_manager->property("recoveryCanRetry").toBool());
    QCOMPARE(m_manager->browserWindowCount(), 0);
    QCOMPARE(opened.size(), 0);
    QQuickItem *retryButton = nullptr;
    for (QWindow *window : QGuiApplication::topLevelWindows()) {
        for (QQuickItem *item : window->findChildren<QQuickItem *>()) {
            if (item->property("text").toString() == "Try again" && item->isVisible()) {
                retryButton = item;
                break;
            }
        }
    }
    QVERIFY(retryButton);
    QVERIFY(QMetaObject::invokeMethod(retryButton, "clicked"));
    QCOMPARE(m_manager->browserWindowCount(), 0);
    QCOMPARE(opened.size(), 0);
    if (!damagedPath.isEmpty()) {
        QCOMPARE(readFile(damagedPath), QByteArray("damaged fixture"));
    }
    failKeyringLookup = false;
    if (failure == "session") {
        const auto context = m_manager->contextFor(record->id.toString());
        QVERIFY(context);
        const QJsonObject tab{{"url", "eden://settings"}, {"pinned", true}};
        const QJsonObject window{{"tabs", QJsonArray{tab}}, {"activeIndex", 0}};
        const QJsonObject session{{"version", 4}, {"windows", QJsonArray{window}}};
        const QByteArray stored = QByteArray("EDEN-SESSION-1\n") +
                                  context->credentialVault()->sealData(QJsonDocument(session).toJson(), "session");
        QVERIFY(writeFile(damagedPath, stored));
    } else if (!damagedPath.isEmpty()) {
        QVERIFY(QFile::remove(damagedPath));
    }
    QVERIFY(QMetaObject::invokeMethod(m_manager.get(), "retryProfileOpen"));
    QCOMPARE(opened.size(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(m_manager->startupState(), QString("browsing"), 10000);
    QVERIFY(m_manager->recoveryMessage().isEmpty());
    QVERIFY(!m_manager->property("recoveryCanRetry").toBool());
    QCOMPARE(m_manager->browserWindowCount(), 1);
    const auto context = m_manager->contextFor(record->id.toString());
    QVERIFY(context->isActivated());
    const auto controllers = context->windows();
    QCOMPARE(controllers.size(), 1);
    QVERIFY(controllers.first()->isInitialized());
    QCOMPARE(controllers.first()->tabs()->rowCount(), 1);
    QCOMPARE(controllers.first()->currentUrl(), QUrl(failure == "session" ? "eden://settings" : "eden://newtab"));
}

void ProfileActivationTest::recoveryCanChooseAnotherProfile() {
    const auto broken = createRecord();
    const auto healthy = createRecord();
    QVERIFY(broken);
    QVERIFY(healthy);
    const auto paths = pathsFor(*broken);
    QVERIFY(paths.ensureBaseDirectories());
    QVERIFY(writeFile(paths.settingsPath(), "damaged fixture"));
    m_manager->activateProfile(broken->id.toString());
    QCOMPARE(m_manager->startupState(), QString("recovery"));
    QQuickItem *chooseButton = nullptr;
    for (QWindow *window : QGuiApplication::topLevelWindows()) {
        for (QQuickItem *item : window->findChildren<QQuickItem *>()) {
            if (item->property("text").toString() == "Choose another profile" && item->isVisible()) {
                chooseButton = item;
                break;
            }
        }
    }
    QVERIFY(chooseButton);
    QVERIFY(QMetaObject::invokeMethod(chooseButton, "clicked"));
    QCOMPARE(m_manager->startupState(), QString("chooser"));
    QVERIFY(!m_manager->property("recoveryCanRetry").toBool());
    m_manager->activateProfile(healthy->id.toString());
    QTRY_COMPARE_WITH_TIMEOUT(m_manager->startupState(), QString("browsing"), 10000);
    QCOMPARE(m_manager->browserWindowCount(), 1);
    QCOMPARE(m_windows->mostRecentActiveProfileId(), healthy->id.toString());
    QCOMPARE(readFile(paths.settingsPath()), QByteArray("damaged fixture"));
}

void ProfileActivationTest::privateStartupRetryPreservesMode() {
    const auto record = createRecord();
    QVERIFY(record);
    const auto paths = pathsFor(*record);
    QVERIFY(paths.ensureBaseDirectories());
    QVERIFY(writeFile(paths.sessionPath(), "damaged fixture"));
    bool stored = false;
    m_registry->setApplicationState(
        QLatin1String(eden::core::applicationstate::lastActiveProfileId),
        record->id.toString(),
        [&](eden::core::ProfileError error) { stored = error == eden::core::ProfileError::None; }
    );
    QTRY_VERIFY(stored);
    failKeyringLookup = true;
    m_manager->configureLaunch(true, {}, {});
    m_manager->beginStartup();
    QTRY_COMPARE(m_manager->startupState(), QString("recovery"));
    QCOMPARE(m_manager->browserWindowCount(), 0);
    failKeyringLookup = false;
    QVERIFY(QMetaObject::invokeMethod(m_manager.get(), "retryProfileOpen"));
    QTRY_COMPARE_WITH_TIMEOUT(m_manager->startupState(), QString("browsing"), 10000);
    QCOMPARE(m_manager->browserWindowCount(), 1);
    const auto context = m_manager->contextFor(record->id.toString());
    QVERIFY(context);
    QCOMPARE(context->windows().size(), 1);
    QVERIFY(context->windows().first()->isPrivateWindow());
    context->saveSessionNow();
    QCOMPARE(readFile(paths.sessionPath()), QByteArray("damaged fixture"));
}

void ProfileActivationTest::windowReportsSaveFailures() {
    const auto record = createRecord();
    QVERIFY(record);
    const auto paths = pathsFor(*record);
    const auto context = eden::core::ProfileContext::create(*record, paths, m_engine.get(), nullptr);
    QVERIFY(context);
    eden::core::WindowController controller;
    controller.initialize(context, false, {}, false);
    QVERIFY(controller.isInitialized());
    QVERIFY(context->sessions()->write({}));
    const QByteArray settings = readFile(paths.settingsPath());
    const QByteArray session = readFile(paths.sessionPath());
    QVERIFY(QFile::setPermissions(paths.dataDirectory(), QFile::ReadOwner | QFile::ExeOwner));
    const auto restore = qScopeGuard([&] {
        QFile::setPermissions(paths.dataDirectory(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    });
    QSignalSpy messages(&controller, &eden::core::WindowController::transientMessageRequested);
    context->settings()->setTabLayout("sidebar");
    QCOMPARE(messages.size(), 1);
    QVERIFY(messages.constLast().constFirst().toString().contains("settings"));
    QVERIFY(!context->sessions()->write({}));
    QCOMPARE(messages.size(), 2);
    QVERIFY(messages.constLast().constFirst().toString().contains("tabs"));
    QCOMPARE(readFile(paths.settingsPath()), settings);
    QCOMPARE(readFile(paths.sessionPath()), session);
    controller.prepareToClose();
}

void ProfileActivationTest::queuedLaunchesOpenSeparateWindows() {
    const auto record = createRecord();
    QVERIFY(record);
    QVERIFY(m_manager->requestWindow({false, {}, {QUrl("eden://settings/search"), QUrl("eden://newtab")}}));
    QVERIFY(m_manager->requestWindow({true, {}, {QUrl("eden://settings/privacy")}}));
    QCOMPARE(m_manager->browserWindowCount(), 0);
    m_manager->activateProfile(record->id.toString());
    QTRY_COMPARE(m_manager->browserWindowCount(), 3);
    const auto context = m_manager->contextFor(record->id.toString());
    QVERIFY(context);
    QCOMPARE(context->windows().size(), 3);
    int normalWindows = 0;
    int privateWindows = 0;
    int requestedNormalWindows = 0;
    for (auto *controller : context->windows()) {
        if (controller->isPrivateWindow()) {
            ++privateWindows;
            QCOMPARE(controller->tabs()->rowCount(), 1);
            QCOMPARE(
                controller->tabs()->data(controller->tabs()->index(0), eden::core::TabModel::UrlRole).toUrl(),
                QUrl("eden://settings/privacy")
            );
        } else {
            ++normalWindows;
            if (controller->tabs()->rowCount() == 2) {
                ++requestedNormalWindows;
                QCOMPARE(
                    controller->tabs()->data(controller->tabs()->index(0), eden::core::TabModel::UrlRole).toUrl(),
                    QUrl("eden://settings/search")
                );
                QCOMPARE(
                    controller->tabs()->data(controller->tabs()->index(1), eden::core::TabModel::UrlRole).toUrl(),
                    QUrl("eden://newtab")
                );
            }
        }
    }
    QCOMPARE(normalWindows, 2);
    QCOMPARE(privateWindows, 1);
    QCOMPARE(requestedNormalWindows, 1);
    QVERIFY(m_manager->requestWindow({}));
    QTRY_COMPARE(m_manager->browserWindowCount(), 4);
    QCOMPARE(context->windows().size(), 4);
    for (auto *controller : context->windows()) {
        if (!controller->isPrivateWindow()) {
            delete controller->window();
        }
    }
    QTRY_COMPARE(m_manager->browserWindowCount(), 1);
    QVERIFY(m_manager->requestWindow({true, {}, {}}));
    QTRY_COMPARE(m_manager->browserWindowCount(), 2);
    for (auto *controller : context->windows()) {
        QVERIFY(controller->isPrivateWindow());
    }
}

void ProfileActivationTest::startupWindowShowsProgress() {
    QQmlComponent component(m_engine.get());
    component.loadFromModule("Eden.Ui", "LaunchWindow");
    QVERIFY2(!component.isError(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window, qPrintable(component.errorString()));
    auto *message = window->findChild<QQuickItem *>("startupMessage");
    auto *details = window->findChild<QQuickItem *>("startupDetails");
    QVERIFY(message);
    QVERIFY(details);
    QVERIFY(!message->property("text").toString().isEmpty());
    QCOMPARE(message->property("text").toString(), m_manager->startupMessage());
    QCOMPARE(details->property("text").toString(), m_manager->startupDetails());
}

QTEST_MAIN(ProfileActivationTest)

#include "profileactivation_test.moc"

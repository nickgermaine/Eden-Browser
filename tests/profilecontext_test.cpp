#include <libsecret/secret.h>

#include "core/bookmarks/bookmarkstore.h"
#include "core/downloads/downloadmanager.h"
#include "core/history/historystore.h"
#include "core/profiles/profilecontext.h"
#include "core/profiles/profiledatabase.h"
#include "core/profiles/profilemigration.h"
#include "core/profiles/profilesettings.h"
#include "core/profiles/sessionstore.h"
#include "core/window/windowcontroller.h"
#include "profiletesthelpers.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QtTest>

extern "C" gchar *secret_password_lookup_sync(const SecretSchema *, GCancellable *, GError **, ...) {
    return g_strdup(QByteArray(32, 'h').toBase64().constData());
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
    qFatal("The profile context tests must not create keyring entries");
}

extern "C" void secret_password_free(gchar *password) {
    g_free(password);
}

class ProfileContextTest final : public QObject {
    Q_OBJECT

  private slots:
    void twoContextsWriteToSeparateDatabases();
    void flushWaitsForHistoryWrites();
    void settingsAreScopedPerProfile();
    void sessionsAreScopedPerProfile();
    void creationEnforcesOwnerOnlyPermissions();
    void rejectsMismatchedIdentity();
    void identityChangesNotifyOnlyRegisteredWindows();
    void settingsRoutesUseUrlPaths();
    void downloadHistoryPersistsEncryptedPerProfile();
    void legacyMigrationMovesDataIntoProfile();
    void migrationIsIdempotent();
};

void ProfileContextTest::twoContextsWriteToSeparateDatabases() {
    eden::test::ProfileHarness first;
    eden::test::ProfileHarness second;
    QVERIFY(first.create());
    QVERIFY(second.create());
    first.context->activate();
    second.context->activate();
    first.context->history()->recordVisit(QUrl("https://first.example"), "First Site");
    second.context->history()->recordVisit(QUrl("https://second.example"), "Second Site");
    first.context->bookmarks()->add(QUrl("https://bookmark.first"), "First Bookmark");
    QTRY_COMPARE(first.context->history()->rowCount(), 1);
    QTRY_COMPARE(second.context->history()->rowCount(), 1);
    QCOMPARE(
        first.context->history()
            ->data(first.context->history()->index(0), eden::core::HistoryStore::TitleRole)
            .toString(),
        QString("First Site")
    );
    QCOMPARE(
        second.context->history()
            ->data(second.context->history()->index(0), eden::core::HistoryStore::TitleRole)
            .toString(),
        QString("Second Site")
    );
    QCOMPARE(first.context->bookmarks()->rowCount(), 1);
    QCOMPARE(second.context->bookmarks()->rowCount(), 0);
    QVERIFY(first.context->paths().databasePath() != second.context->paths().databasePath());
    QVERIFY(QFileInfo::exists(first.context->paths().databasePath()));
    QVERIFY(QFileInfo::exists(second.context->paths().databasePath()));
}

void ProfileContextTest::flushWaitsForHistoryWrites() {
    eden::test::ProfileHarness profile;
    QVERIFY(profile.create());
    QCOMPARE(profile.context->activate(), eden::core::ProfileError::None);
    profile.context->history()->recordVisit(QUrl("https://signout.invalid/"), "Saved before sign-out");
    bool finished = false;
    profile.context->flushEngines(0, [&profile, &finished] {
        QVERIFY(!profile.context->history()->hasPendingWrites());
        QCOMPARE(profile.context->history()->rowCount(), 1);
        finished = true;
        profile.context->shutdownStores();
    });
    QVERIFY(!finished);
    QTRY_VERIFY(finished);
    QVERIFY(!profile.context->history());
}

void ProfileContextTest::settingsAreScopedPerProfile() {
    eden::test::ProfileHarness first;
    eden::test::ProfileHarness second;
    QVERIFY(first.create());
    QVERIFY(second.create());
    first.context->activate();
    second.context->activate();
    first.context->settings()->setTabLayout("sidebar");
    QCOMPARE(first.context->settings()->tabLayout(), QString("sidebar"));
    QCOMPARE(second.context->settings()->tabLayout(), QString("horizontal"));
    first.context->settings()->selectSearchEngine("Google");
    QCOMPARE(second.context->settings()->searchEngine(), QString("DuckDuckGo"));
}

void ProfileContextTest::sessionsAreScopedPerProfile() {
    eden::test::ProfileHarness first;
    eden::test::ProfileHarness second;
    QVERIFY(first.create());
    QVERIFY(second.create());
    first.context->activate();
    second.context->activate();
    QJsonObject session;
    session.insert("version", 3);
    QJsonArray windows;
    QJsonObject window;
    window.insert("activeIndex", 0);
    windows.append(window);
    session.insert("windows", windows);
    QVERIFY(first.context->sessions()->write(session));
    QVERIFY(QFileInfo::exists(first.context->paths().sessionPath()));
    QVERIFY(!QFileInfo::exists(second.context->paths().sessionPath()));
    QCOMPARE(first.context->sessions()->load().value("windows").toArray().size(), 1);
    QVERIFY(second.context->sessions()->load().isEmpty());
}

void ProfileContextTest::creationEnforcesOwnerOnlyPermissions() {
    eden::test::ProfileHarness harness;
    QVERIFY(harness.create());
    const QFileInfo dataInfo(harness.context->paths().dataDirectory());
    QVERIFY(dataInfo.exists());
    const QFile::Permissions permissions = QFile::permissions(harness.context->paths().dataDirectory());
    QVERIFY(permissions.testFlag(QFileDevice::ReadOwner));
    QVERIFY(!permissions.testFlag(QFileDevice::ReadGroup));
    QVERIFY(!permissions.testFlag(QFileDevice::ReadOther));
}

void ProfileContextTest::rejectsMismatchedIdentity() {
    eden::test::ProfileHarness harness;
    QVERIFY(harness.dataRoot.isValid());
    eden::core::ProfileRecord record;
    record.id = eden::core::ProfileId::generate();
    record.displayName = "Mismatch";
    const eden::core::ProfilePaths paths(harness.roots(), eden::core::ProfileId::generate());
    eden::core::ProfileError error = eden::core::ProfileError::None;
    QVERIFY(!eden::core::ProfileContext::create(record, paths, nullptr, &error));
    QCOMPARE(error, eden::core::ProfileError::InvalidProfileId);
}

void ProfileContextTest::identityChangesNotifyOnlyRegisteredWindows() {
    eden::test::ProfileHarness harness;
    QVERIFY(harness.create());
    eden::core::WindowController controller;
    harness.context->registerWindow(&controller);
    QSignalSpy identityChanged(&controller, &eden::core::WindowController::profileIdentityChanged);

    eden::core::ProfileSummary summary;
    summary.profileId = harness.context->idString();
    summary.displayName = "Renamed";
    summary.colorSeed = 4;
    summary.avatarRevision = 2;
    summary.lifecycle = eden::core::ProfileLifecycle::Ready;
    harness.context->applySummary(summary);
    QCOMPARE(identityChanged.size(), 1);

    harness.context->unregisterWindow(&controller);
    summary.displayName = "Renamed again";
    harness.context->applySummary(summary);
    QCOMPARE(identityChanged.size(), 1);
}

void ProfileContextTest::settingsRoutesUseUrlPaths() {
    eden::core::WindowController controller;
    QCOMPARE(
        controller.settingsPath(QUrl("eden://settings/autofill/passwords?site=example.com")),
        QString("/autofill/passwords")
    );
    QCOMPARE(controller.settingsPath(QUrl("eden://settings/privacy/site-settings")), QString("/privacy/site-settings"));
}

void ProfileContextTest::downloadHistoryPersistsEncryptedPerProfile() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath("eden.sqlite");
    const QByteArray key(32, 'd');
    eden::core::ProfileDatabase database(databasePath);
    QVERIFY(database.initialize(key));
    {
        eden::core::DownloadManager downloads(databasePath);
        QVERIFY(downloads.initialize(key));
        downloads.beginDownload(
            7,
            "report.json",
            QUrl("https://example.test/report.json"),
            directory.filePath("report.json"),
            128
        );
        downloads.updateDownload(7, 128, 128, "complete");
        QCOMPARE(downloads.rowCount(), 1);
    }
    {
        eden::core::DownloadManager downloads(databasePath);
        QVERIFY(downloads.initialize(key));
        QCOMPARE(downloads.rowCount(), 1);
        QCOMPARE(
            downloads.data(downloads.index(0), eden::core::DownloadManager::StateRole).toString(),
            QString("complete")
        );
        downloads.beginDownload(
            8,
            "trace.json",
            QUrl("devtools://devtools/trace.json"),
            directory.filePath("trace.json"),
            512
        );
    }
    {
        eden::core::DownloadManager downloads(databasePath);
        QVERIFY(downloads.initialize(key));
        QCOMPARE(downloads.rowCount(), 2);
        QCOMPARE(
            downloads.data(downloads.index(0), eden::core::DownloadManager::StateRole).toString(),
            QString("interrupted")
        );
        downloads.clearFinished();
        QCOMPARE(downloads.rowCount(), 0);
    }
}

void ProfileContextTest::legacyMigrationMovesDataIntoProfile() {
    QTemporaryDir dataRoot;
    QTemporaryDir cacheRoot;
    QVERIFY(dataRoot.isValid() && cacheRoot.isValid());
    const QByteArray previousDataHome = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", QFile::encodeName(dataRoot.path()));
    const auto restoreDataHome = qScopeGuard([&previousDataHome] { qputenv("XDG_DATA_HOME", previousDataHome); });
    const eden::core::ProfilePaths::Roots roots{dataRoot.path(), cacheRoot.path()};

    {
        QSqlDatabase legacy = QSqlDatabase::addDatabase("QSQLITE", "legacy-db");
        legacy.setDatabaseName(roots.dataRoot + "/eden.db");
        QVERIFY(legacy.open());
        QSqlQuery query(legacy);
        QVERIFY(query.exec(
            "CREATE TABLE history(url TEXT PRIMARY KEY, title TEXT NOT NULL, visited_at INTEGER NOT NULL, "
            "visit_count INTEGER NOT NULL DEFAULT 1)"
        ));
        QVERIFY(query.exec("INSERT INTO history VALUES('https://legacy.example', 'Legacy Site', 42, 3)"));
        legacy.close();
    }
    QSqlDatabase::removeDatabase("legacy-db");
    QFile session(roots.dataRoot + "/session.json");
    QVERIFY(session.open(QIODevice::WriteOnly));
    session.write("{\"version\":3,\"windows\":[{\"activeIndex\":0,\"tabs\":[{\"url\":\"https://legacy.example\"}]}]}");
    session.close();
    QVERIFY(QDir().mkpath(roots.dataRoot + "/webengine/storage"));
    QFile engineMarker(roots.dataRoot + "/webengine/storage/Cookies");
    QVERIFY(engineMarker.open(QIODevice::WriteOnly));
    engineMarker.write("cookie-data");
    engineMarker.close();

    const eden::core::ProfileMigration::Inventory inventory = eden::core::ProfileMigration::inventoryLegacyData(roots);
    QVERIFY(inventory.any());
    QCOMPARE(inventory.legacyDatabasePath, roots.dataRoot + "/eden.db");

    const eden::core::ProfileId profileId = eden::core::ProfileId::generate();
    const eden::core::ProfilePaths paths(roots, profileId);
    QCOMPARE(eden::core::ProfileMigration::migrate(inventory, paths), eden::core::ProfileError::None);

    QVERIFY(!QFileInfo::exists(roots.dataRoot + "/eden.db"));
    QVERIFY(!QFileInfo::exists(roots.dataRoot + "/session.json"));
    QVERIFY(QFileInfo::exists(paths.databasePath()));
    QVERIFY(QFileInfo::exists(paths.sessionPath()));
    QVERIFY(QFileInfo::exists(paths.engineDataDirectory("qtwebengine") + "/storage/Cookies"));

    const QByteArray databaseKey(32, 'k');
    eden::core::ProfileDatabase database(paths.databasePath());
    QVERIFY(database.initialize(databaseKey));
    eden::core::HistoryStore history(paths.databasePath());
    history.initialize(databaseKey);
    QCOMPARE(history.rowCount(), 1);
    QCOMPARE(history.data(history.index(0), eden::core::HistoryStore::TitleRole).toString(), QString("Legacy Site"));
}

void ProfileContextTest::migrationIsIdempotent() {
    QTemporaryDir dataRoot;
    QTemporaryDir cacheRoot;
    QVERIFY(dataRoot.isValid() && cacheRoot.isValid());
    const QByteArray previousDataHome = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", QFile::encodeName(dataRoot.path()));
    const auto restoreDataHome = qScopeGuard([&previousDataHome] { qputenv("XDG_DATA_HOME", previousDataHome); });
    const eden::core::ProfilePaths::Roots roots{dataRoot.path(), cacheRoot.path()};
    QFile session(roots.dataRoot + "/session.json");
    QVERIFY(session.open(QIODevice::WriteOnly));
    session.write("{\"version\":3}");
    session.close();
    const eden::core::ProfileId profileId = eden::core::ProfileId::generate();
    const eden::core::ProfilePaths paths(roots, profileId);
    const eden::core::ProfileMigration::Inventory inventory = eden::core::ProfileMigration::inventoryLegacyData(roots);
    QCOMPARE(eden::core::ProfileMigration::migrate(inventory, paths), eden::core::ProfileError::None);
    QCOMPARE(eden::core::ProfileMigration::migrate(inventory, paths), eden::core::ProfileError::None);
    const eden::core::ProfileMigration::Inventory after = eden::core::ProfileMigration::inventoryLegacyData(roots);
    QVERIFY(!after.any());
    QVERIFY(QFileInfo::exists(paths.sessionPath()));
}

QTEST_GUILESS_MAIN(ProfileContextTest)
#include "profilecontext_test.moc"

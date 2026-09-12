#include "core/permissions/permissionstore.h"
#include "core/profiles/sqlcipherdatabase.h"

#include <QTemporaryDir>
#include <QtTest>
#include <sqlcipher/sqlite3.h>

class PermissionStoreTest final : public QObject {
    Q_OBJECT

  private slots:
    void persistsOriginScopedVerdicts();
    void listsRequestedSitesAndAllPermissionTypes();
    void screenCaptureAlwaysRequiresFreshConsent();
};

void PermissionStoreTest::persistsOriginScopedVerdicts() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath("profile.sqlite");
    const QByteArray key(32, 'k');
    QVERIFY(eden::core::SqlCipherDatabase::initialize(path, key));
    {
        eden::core::PermissionStore store(path);
        QVERIFY(store.initialize(key));
        QVERIFY(store.noteRequested(QUrl("https://Example.test/login"), "camera"));
        QCOMPARE(store.state(QUrl("https://example.test/elsewhere"), "camera"), QString("ask"));
        QVERIFY(store.setPermission(QUrl("https://example.test"), "camera", true));
        QCOMPARE(store.state(QUrl("https://example.test/path"), "camera"), QString("allowed"));
        QCOMPARE(store.state(QUrl("http://example.test"), "camera"), QString("ask"));
    }
    {
        eden::core::PermissionStore store(path);
        QVERIFY(store.initialize(key));
        QCOMPARE(store.state(QUrl("https://example.test"), "camera"), QString("allowed"));
        QVERIFY(store.setPermission(QUrl("https://example.test"), "camera", false));
        QCOMPARE(store.state(QUrl("https://example.test"), "camera"), QString("blocked"));
    }
}

void PermissionStoreTest::listsRequestedSitesAndAllPermissionTypes() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath("profile.sqlite");
    const QByteArray key(32, 'p');
    QVERIFY(eden::core::SqlCipherDatabase::initialize(path, key));
    eden::core::PermissionStore store(path);
    QVERIFY(store.initialize(key));
    QVERIFY(store.noteRequested(QUrl("https://maps.example"), "geolocation"));
    QVERIFY(store.setPermission(QUrl("https://maps.example"), "notifications", false));
    const QVariantList sites = store.sites("maps");
    QCOMPARE(sites.size(), 1);
    QCOMPARE(sites.constFirst().toMap().value("origin").toString(), QString("https://maps.example"));
    const QVariantList requested = store.permissionsForOrigin(QUrl("https://maps.example"), false);
    QCOMPARE(requested.size(), 2);
    const QVariantList all = store.permissionsForOrigin(QUrl("https://maps.example"), true);
    QVERIFY(all.size() >= 9);
}

void PermissionStoreTest::screenCaptureAlwaysRequiresFreshConsent() {
    QTemporaryDir directory;
    const QString path = directory.filePath("profile.sqlite");
    const QByteArray key(32, 'p');
    QVERIFY(eden::core::SqlCipherDatabase::initialize(path, key));
    {
        eden::core::PermissionStore store(path);
        QVERIFY(store.initialize(key));
        QVERIFY(store.setPermission(QUrl("https://meet.example"), "screen sharing", true));
        QCOMPARE(store.state(QUrl("https://meet.example"), "screen sharing"), QString("ask"));
        for (const QString &alias : {QString("screen audio"), QString("desktop video"), QString("screen capture")}) {
            QVERIFY(store.setPermission(QUrl("https://meet.example"), alias, true));
            QCOMPARE(store.state(QUrl("https://meet.example"), alias), QString("ask"));
        }
        QVERIFY(store.setPermission(QUrl("https://meet.example"), "screen sharing", false));
        QCOMPARE(store.state(QUrl("https://meet.example"), "screen sharing"), QString("blocked"));
    }
    sqlite3 *database = eden::core::SqlCipherDatabase::open(path, key, false);
    QVERIFY(database);
    QVERIFY(eden::core::SqlCipherDatabase::execute(database, "UPDATE site_permissions SET verdict=1"));
    sqlite3_close_v2(database);
    eden::core::PermissionStore reloaded(path);
    QVERIFY(reloaded.initialize(key));
    QCOMPARE(reloaded.state(QUrl("https://meet.example"), "screen sharing"), QString("ask"));
    QCOMPARE(reloaded.sites().first().toMap().value("allowedCount").toInt(), 0);
}

QTEST_MAIN(PermissionStoreTest)
#include "permissionstore_test.moc"

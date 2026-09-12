#include <libsecret/secret.h>

#include "core/profiles/profilesettings.h"
#include "passwords/credentialvault.h"

#include <QFile>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

extern "C" gchar *secret_password_lookup_sync(const SecretSchema *, GCancellable *, GError **, ...) {
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
    qFatal("The settings tests must not create keyring entries");
}

extern "C" void secret_password_free(gchar *password) {
    g_free(password);
}

static QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

static bool writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

class ProfileSettingsTest final : public QObject {
    Q_OBJECT

  private slots:
    void preservesInvalidSettings_data();
    void preservesInvalidSettings();
    void readsWithoutRewriting();
    void migratesValidLegacySettings();
    void rejectsUnreadableSettings();
    void reportsWriteFailures();
};

void ProfileSettingsTest::preservesInvalidSettings_data() {
    QTest::addColumn<QByteArray>("plain");
    QTest::addColumn<QByteArray>("stored");
    QTest::addColumn<bool>("tamper");
    QTest::newRow("authentication-failure")
        << QByteArray("{\"appearance/tabLayout\":\"sidebar\"}") << QByteArray() << true;
    QTest::newRow("invalid-json") << QByteArray("{broken") << QByteArray() << false;
    QTest::newRow("non-object-json") << QByteArray("[]") << QByteArray() << false;
    QTest::newRow("truncated-envelope") << QByteArray() << QByteArray("EDEN-SETTINGS-1\ntruncated") << false;
    QTest::newRow("unknown-envelope") << QByteArray() << QByteArray("EDEN-SETTINGS-2\nfixture") << false;
    QTest::newRow("invalid-legacy") << QByteArray() << QByteArray("[broken section\ninvalid") << false;
    QTest::newRow("invalid-legacy-utf8") << QByteArray() << QByteArray("[appearance]\ntabLayout=side\xc3") << false;
    QTest::newRow("empty-file") << QByteArray() << QByteArray() << false;
}

void ProfileSettingsTest::preservesInvalidSettings() {
    QFETCH(QByteArray, plain);
    QFETCH(QByteArray, stored);
    QFETCH(bool, tamper);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    eden::passwords::CredentialVault vault(directory.filePath("vault.sqlite"), "settings-test");
    QVERIFY(vault.initialize());
    const QString path = directory.filePath("settings.ini");
    if (!plain.isEmpty()) {
        stored = QByteArray("EDEN-SETTINGS-1\n") + vault.sealData(plain, "settings");
    }
    if (tamper) {
        stored[stored.size() - 1] = stored.back() ^ 1;
    }
    QVERIFY(writeFile(path, stored));
    eden::core::ProfileSettings settings(path);
    settings.setVault(&vault);
    QVERIFY(!settings.initialize());
    QCOMPARE(settings.property("errorCode").toString(), QString("settings_invalid"));
    QVERIFY(!settings.property("errorMessage").toString().isEmpty());
    settings.setTabLayout("sidebar");
    settings.setSearchSuggestions(false);
    QCOMPARE(readFile(path), stored);
    const QByteArray restored =
        QByteArray("EDEN-SETTINGS-1\n") + vault.sealData("{\"appearance/tabLayout\":\"sidebar\"}", "settings");
    QVERIFY(writeFile(path, restored));
    QVERIFY(settings.initialize());
    QCOMPARE(settings.tabLayout(), QString("sidebar"));
    QVERIFY(settings.property("errorCode").toString().isEmpty());
    QCOMPARE(readFile(path), restored);
}

void ProfileSettingsTest::readsWithoutRewriting() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    eden::passwords::CredentialVault vault(directory.filePath("vault.sqlite"), "settings-test");
    QVERIFY(vault.initialize());
    const QString path = directory.filePath("settings.ini");
    eden::core::ProfileSettings first(path);
    first.setVault(&vault);
    QVERIFY(first.initialize());
    first.setTabLayout("sidebar");
    const QByteArray original = readFile(path);
    QVERIFY(original.startsWith("EDEN-SETTINGS-1\n"));
    eden::core::ProfileSettings reopened(path);
    reopened.setVault(&vault);
    QVERIFY(reopened.initialize());
    QCOMPARE(reopened.tabLayout(), QString("sidebar"));
    QCOMPARE(readFile(path), original);
    reopened.setHomePageUrl("https://fixture.invalid/home");
    QVERIFY(readFile(path) != original);
    eden::core::ProfileSettings latest(path);
    latest.setVault(&vault);
    QVERIFY(latest.initialize());
    QCOMPARE(latest.homePageUrl(), QString("https://fixture.invalid/home"));
    QCOMPARE(latest.tabLayout(), QString("sidebar"));
}

void ProfileSettingsTest::migratesValidLegacySettings() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    eden::passwords::CredentialVault vault(directory.filePath("vault.sqlite"), "settings-test");
    QVERIFY(vault.initialize());
    const QString path = directory.filePath("settings.ini");
    QVERIFY(writeFile(path, "[appearance]\ntabLayout=sidebar\n[custom]\nfeature=retained\n"));
    eden::core::ProfileSettings settings(path);
    settings.setVault(&vault);
    QVERIFY(settings.initialize());
    QCOMPARE(settings.tabLayout(), QString("sidebar"));
    const QByteArray migrated = readFile(path);
    const QByteArray header("EDEN-SETTINGS-1\n");
    QVERIFY(migrated.startsWith(header));
    const QJsonObject values =
        QJsonDocument::fromJson(vault.openData(migrated.sliced(header.size()), "settings")).object();
    QCOMPARE(values.value("custom/feature").toString(), QString("retained"));
}

void ProfileSettingsTest::rejectsUnreadableSettings() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    eden::passwords::CredentialVault vault(directory.filePath("vault.sqlite"), "settings-test");
    QVERIFY(vault.initialize());
    const QString path = directory.filePath("settings.ini");
    const QByteArray original("[appearance]\ntabLayout=sidebar\n");
    QVERIFY(writeFile(path, original));
    QVERIFY(QFile::setPermissions(path, {}));
    const auto restore = qScopeGuard([&] { QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner); });
    eden::core::ProfileSettings settings(path);
    settings.setVault(&vault);
    QVERIFY(!settings.initialize());
    QCOMPARE(settings.property("errorCode").toString(), QString("settings_read_failed"));
    QVERIFY(QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner));
    QCOMPARE(readFile(path), original);
    QVERIFY(settings.initialize());
    QCOMPARE(settings.tabLayout(), QString("sidebar"));
}

void ProfileSettingsTest::reportsWriteFailures() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    eden::passwords::CredentialVault vault(directory.filePath("vault.sqlite"), "settings-test");
    QVERIFY(vault.initialize());
    const QString path = directory.filePath("settings.ini");
    eden::core::ProfileSettings settings(path);
    settings.setVault(&vault);
    QVERIFY(settings.initialize());
    const QByteArray original = readFile(path);
    QVERIFY(QFile::setPermissions(directory.path(), QFile::ReadOwner | QFile::ExeOwner));
    const auto restore = qScopeGuard([&] {
        QFile::setPermissions(directory.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    });
    settings.setTabLayout("sidebar");
    QCOMPARE(settings.property("errorCode").toString(), QString("settings_write_failed"));
    QVERIFY(!settings.property("errorMessage").toString().isEmpty());
    QCOMPARE(readFile(path), original);
    QVERIFY(QFile::setPermissions(directory.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    settings.setSearchSuggestions(false);
    QVERIFY(settings.property("errorCode").toString().isEmpty());
    eden::core::ProfileSettings reopened(path);
    reopened.setVault(&vault);
    QVERIFY(reopened.initialize());
    QCOMPARE(reopened.tabLayout(), QString("sidebar"));
    QVERIFY(!reopened.searchSuggestions());
}

QTEST_GUILESS_MAIN(ProfileSettingsTest)

#include "profilesettings_test.moc"

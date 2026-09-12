#include <libsecret/secret.h>

#include "passwords/credentialvault.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <optional>

namespace {

    std::optional<QByteArray> storedKey;
    bool lookupFails = false;
    bool storeFails = false;
    int storeCalls = 0;
    int freeCalls = 0;

    QByteArray readFile(const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

}

extern "C" gchar *secret_password_lookup_sync(const SecretSchema *, GCancellable *, GError **error, ...) {
    if (lookupFails) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Simulated keyring timeout");
    }
    return storedKey ? g_strdup(storedKey->constData()) : nullptr;
}

extern "C" gboolean secret_password_store_sync(
    const SecretSchema *,
    const gchar *,
    const gchar *,
    const gchar *password,
    GCancellable *,
    GError **error,
    ...
) {
    ++storeCalls;
    if (storeFails) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Simulated keyring write failure");
        return false;
    }
    storedKey = QByteArray(password);
    return true;
}

extern "C" void secret_password_free(gchar *password) {
    ++freeCalls;
    g_free(password);
}

class CredentialVaultTest final : public QObject {
    Q_OBJECT

  private slots:
    void init();
    void createsAndReusesKeys();
    void lookupFailurePreservesExistingVault();
    void lookupFailureDoesNotCreateVault();
    void malformedKeys_data();
    void malformedKeys();
    void missingKeyPreservesExistingVault();
    void failedStoreCanBeRetried();
    void createsKeyAlongsideLegacyData();
};

void CredentialVaultTest::init() {
    storedKey.reset();
    lookupFails = false;
    storeFails = false;
    storeCalls = 0;
    freeCalls = 0;
}

void CredentialVaultTest::createsAndReusesKeys() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath("vault.sqlite");
    qint64 id;
    QByteArray derived;
    {
        eden::passwords::CredentialVault vault(path, "mocked-profile");
        QVERIFY(vault.initialize());
        QCOMPARE(storeCalls, 1);
        QVERIFY(storedKey.has_value());
        QCOMPARE(QByteArray::fromBase64(*storedKey).size(), 32);
        id = vault.saveCredential(0, "https://example.test", "test-user", "test-password");
        QVERIFY(id > 0);
        derived = vault.derivedKey("profile-database");
    }
    const QByteArray key = *storedKey;
    eden::passwords::CredentialVault reopened(path, "mocked-profile");
    QVERIFY(reopened.initialize());
    QCOMPARE(storeCalls, 1);
    QCOMPARE(*storedKey, key);
    QCOMPARE(reopened.revealPassword(id), QString("test-password"));
    QCOMPARE(reopened.derivedKey("profile-database"), derived);
}

void CredentialVaultTest::lookupFailurePreservesExistingVault() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath("vault.sqlite");
    qint64 id;
    {
        eden::passwords::CredentialVault vault(path, "mocked-profile");
        QVERIFY(vault.initialize());
        id = vault.saveCredential(0, "https://example.test", "test-user", "test-password");
        QVERIFY(id > 0);
    }
    const QByteArray database = readFile(path);
    const QByteArray originalKey = *storedKey;
    storeCalls = 0;
    lookupFails = true;
    eden::passwords::CredentialVault reopened(path, "mocked-profile");
    QVERIFY(!reopened.initialize());
    QVERIFY(!reopened.available());
    QCOMPARE(storeCalls, 0);
    QCOMPARE(*storedKey, originalKey);
    QCOMPARE(readFile(path), database);
    QVERIFY(freeCalls > 0);
    lookupFails = false;
    QVERIFY(reopened.initialize());
    QVERIFY(reopened.error().isEmpty());
    QCOMPARE(reopened.revealPassword(id), QString("test-password"));
    QCOMPARE(storeCalls, 0);
}

void CredentialVaultTest::lookupFailureDoesNotCreateVault() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath("vault.sqlite");
    lookupFails = true;
    eden::passwords::CredentialVault vault(path, "mocked-profile");
    QVERIFY(!vault.initialize());
    QCOMPARE(storeCalls, 0);
    QVERIFY(!QFile::exists(path));
    QVERIFY(!storedKey.has_value());
    lookupFails = false;
    QVERIFY(vault.initialize());
    QCOMPARE(storeCalls, 1);
}

void CredentialVaultTest::malformedKeys_data() {
    QTest::addColumn<QByteArray>("encoded");
    QTest::newRow("empty") << QByteArray();
    QTest::newRow("short") << QByteArray(31, 'x').toBase64();
    QTest::newRow("long") << QByteArray(33, 'x').toBase64();
    QTest::newRow("invalid-base64") << (QByteArray(32, 'x').toBase64() + '!');
}

void CredentialVaultTest::malformedKeys() {
    QFETCH(QByteArray, encoded);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath("vault.sqlite");
    storedKey = encoded;
    eden::passwords::CredentialVault vault(path, "mocked-profile");
    QVERIFY(!vault.initialize());
    QCOMPARE(storeCalls, 0);
    QCOMPARE(*storedKey, encoded);
    QVERIFY(!QFile::exists(path));
}

void CredentialVaultTest::missingKeyPreservesExistingVault() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath("vault.sqlite");
    {
        eden::passwords::CredentialVault vault(path, "mocked-profile");
        QVERIFY(vault.initialize());
    }
    const QByteArray database = readFile(path);
    const QByteArray originalKey = *storedKey;
    storedKey.reset();
    storeCalls = 0;
    eden::passwords::CredentialVault reopened(path, "mocked-profile");
    QVERIFY(!reopened.initialize());
    QCOMPARE(storeCalls, 0);
    QVERIFY(!storedKey.has_value());
    QCOMPARE(readFile(path), database);
    storedKey = originalKey;
    QVERIFY(reopened.initialize());
}

void CredentialVaultTest::failedStoreCanBeRetried() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath("vault.sqlite");
    storeFails = true;
    eden::passwords::CredentialVault vault(path, "mocked-profile");
    QVERIFY(!vault.initialize());
    QVERIFY(!QFile::exists(path));
    QVERIFY(!storedKey.has_value());
    storeFails = false;
    QVERIFY(vault.initialize());
    QVERIFY(vault.error().isEmpty());
    QCOMPARE(storeCalls, 2);
}

void CredentialVaultTest::createsKeyAlongsideLegacyData() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile legacy(directory.filePath("settings.ini"));
    QVERIFY(legacy.open(QIODevice::WriteOnly));
    const QByteArray contents("[appearance]\ntabLayout=sidebar\n");
    QCOMPARE(legacy.write(contents), contents.size());
    legacy.close();
    eden::passwords::CredentialVault vault(directory.filePath("vault.sqlite"), "mocked-profile");
    QVERIFY(vault.initialize());
    QCOMPARE(storeCalls, 1);
    QCOMPARE(readFile(legacy.fileName()), contents);
}

QTEST_GUILESS_MAIN(CredentialVaultTest)
#include "credentialvault_test.moc"

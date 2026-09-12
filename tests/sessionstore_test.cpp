#include <libsecret/secret.h>

#include "core/profiles/sessionstore.h"
#include "passwords/credentialvault.h"

#include <QFile>
#include <QJsonArray>
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
    qFatal("The session tests must not create keyring entries");
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

static QJsonObject savedSession(const QString &url = QStringLiteral("https://fixture.invalid/restored")) {
    const QJsonObject tab{{"url", url}, {"title", "Saved tab"}, {"pinned", true}, {"backend", "cef"}};
    const QJsonObject window{{"tabs", QJsonArray{tab}}, {"activeIndex", 0}, {"layout", "sidebar"}};
    return {{"version", 4}, {"windows", QJsonArray{window}}};
}

static QByteArray sealedSession(eden::passwords::CredentialVault &vault, const QJsonObject &session) {
    return QByteArray("EDEN-SESSION-1\n") +
           vault.sealData(QJsonDocument(session).toJson(QJsonDocument::Compact), "session");
}

class SessionStoreTest final : public QObject {
    Q_OBJECT

  private slots:
    void preservesInvalidSessions_data();
    void preservesInvalidSessions();
    void validatesBeforeFirstWrite();
    void savesNewSessionsAndReadsWithoutRewriting();
    void migratesLegacySessions();
    void rejectsUnreadableSessions();
    void retriesFailedMigration();
    void reportsWriteFailures();
    void retriesUnavailableVault();
};

void SessionStoreTest::preservesInvalidSessions_data() {
    QTest::addColumn<QByteArray>("plain");
    QTest::addColumn<QByteArray>("stored");
    QTest::addColumn<bool>("tamper");
    QTest::newRow("authentication-failure") << QByteArray("{\"windows\":[]}") << QByteArray() << true;
    QTest::newRow("invalid-encrypted-json") << QByteArray("{broken") << QByteArray() << false;
    QTest::newRow("encrypted-array-root") << QByteArray("[]") << QByteArray() << false;
    QTest::newRow("truncated-envelope") << QByteArray() << QByteArray("EDEN-SESSION-1\ntruncated") << false;
    QTest::newRow("unknown-envelope") << QByteArray() << QByteArray("EDEN-SESSION-2\nfixture") << false;
    QTest::newRow("invalid-legacy") << QByteArray() << QByteArray("{broken") << false;
    QTest::newRow("legacy-array-root") << QByteArray() << QByteArray("[]") << false;
    QTest::newRow("empty-file") << QByteArray() << QByteArray() << false;
}

void SessionStoreTest::preservesInvalidSessions() {
    QFETCH(QByteArray, plain);
    QFETCH(QByteArray, stored);
    QFETCH(bool, tamper);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    eden::passwords::CredentialVault vault(directory.filePath("vault.sqlite"), "session-test");
    QVERIFY(vault.initialize());
    const QString path = directory.filePath("session.json");
    if (!plain.isEmpty()) {
        stored = QByteArray("EDEN-SESSION-1\n") + vault.sealData(plain, "session");
    }
    if (tamper) {
        stored[stored.size() - 1] = stored.back() ^ 1;
    }
    QVERIFY(writeFile(path, stored));
    eden::core::SessionStore sessions(path);
    sessions.setVault(&vault);
    sessions.requestSave();
    QVERIFY(sessions.savePending());
    QVERIFY(sessions.load().isEmpty());
    QCOMPARE(sessions.property("errorCode").toString(), QString("session_invalid"));
    QVERIFY(!sessions.property("errorMessage").toString().isEmpty());
    QVERIFY(!sessions.savePending());
    sessions.requestSave();
    QVERIFY(!sessions.savePending());
    QVERIFY(!sessions.write(savedSession()));
    QCOMPARE(readFile(path), stored);
    const QJsonObject restoredSession = savedSession();
    const QByteArray restored = sealedSession(vault, restoredSession);
    QVERIFY(writeFile(path, restored));
    QVERIFY(!sessions.write(savedSession("https://fixture.invalid/new")));
    QCOMPARE(readFile(path), restored);
    QCOMPARE(sessions.load(), restoredSession);
    QVERIFY(sessions.property("errorCode").toString().isEmpty());
    QCOMPARE(readFile(path), restored);
    sessions.requestSave();
    QVERIFY(sessions.savePending());
    sessions.cancelPendingSave();
    QVERIFY(!sessions.savePending());
    QVERIFY(sessions.write(savedSession("https://fixture.invalid/new")));
    QCOMPARE(sessions.load(), savedSession("https://fixture.invalid/new"));
}

void SessionStoreTest::validatesBeforeFirstWrite() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    eden::passwords::CredentialVault vault(directory.filePath("vault.sqlite"), "session-test");
    QVERIFY(vault.initialize());
    const QString path = directory.filePath("session.json");
    const QByteArray original("EDEN-SESSION-1\ntruncated");
    QVERIFY(writeFile(path, original));
    eden::core::SessionStore sessions(path);
    sessions.setVault(&vault);
    QVERIFY(!sessions.write(savedSession()));
    QCOMPARE(readFile(path), original);
    QCOMPARE(sessions.property("errorCode").toString(), QString("session_invalid"));
}

void SessionStoreTest::savesNewSessionsAndReadsWithoutRewriting() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    eden::passwords::CredentialVault vault(directory.filePath("vault.sqlite"), "session-test");
    QVERIFY(vault.initialize());
    const QString path = directory.filePath("session.json");
    eden::core::SessionStore sessions(path);
    sessions.setVault(&vault);
    QVERIFY(sessions.load().isEmpty());
    QVERIFY(sessions.property("errorCode").toString().isEmpty());
    QVERIFY(!QFileInfo::exists(path));
    QVERIFY(sessions.write(savedSession()));
    const QByteArray original = readFile(path);
    QVERIFY(original.startsWith("EDEN-SESSION-1\n"));
    eden::core::SessionStore reopened(path);
    reopened.setVault(&vault);
    QCOMPARE(reopened.load(), savedSession());
    QCOMPARE(readFile(path), original);
    QVERIFY(reopened.write({}));
    QVERIFY(reopened.load().isEmpty());
    QVERIFY(reopened.property("errorCode").toString().isEmpty());
}

void SessionStoreTest::migratesLegacySessions() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    eden::passwords::CredentialVault vault(directory.filePath("vault.sqlite"), "session-test");
    QVERIFY(vault.initialize());
    const QString path = directory.filePath("session.json");
    const QJsonObject legacy{
        {"tabs", QJsonArray{QJsonObject{{"url", "https://fixture.invalid/legacy"}}}},
        {"activeIndex", 0}
    };
    QVERIFY(writeFile(path, QJsonDocument(legacy).toJson()));
    eden::core::SessionStore sessions(path);
    sessions.setVault(&vault);
    QCOMPARE(sessions.load(), legacy);
    QVERIFY(sessions.property("errorCode").toString().isEmpty());
    const QByteArray migrated = readFile(path);
    QVERIFY(migrated.startsWith("EDEN-SESSION-1\n"));
    QCOMPARE(sessions.load(), legacy);
    QCOMPARE(readFile(path), migrated);
}

void SessionStoreTest::rejectsUnreadableSessions() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    eden::passwords::CredentialVault vault(directory.filePath("vault.sqlite"), "session-test");
    QVERIFY(vault.initialize());
    const QString path = directory.filePath("session.json");
    const QByteArray original = sealedSession(vault, savedSession());
    QVERIFY(writeFile(path, original));
    QVERIFY(QFile::setPermissions(path, {}));
    const auto restore = qScopeGuard([&] { QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner); });
    eden::core::SessionStore sessions(path);
    sessions.setVault(&vault);
    QVERIFY(sessions.load().isEmpty());
    QCOMPARE(sessions.property("errorCode").toString(), QString("session_read_failed"));
    QVERIFY(!sessions.write({}));
    QVERIFY(QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner));
    QCOMPARE(readFile(path), original);
    QCOMPARE(sessions.load(), savedSession());
    QVERIFY(sessions.write({}));
}

void SessionStoreTest::retriesFailedMigration() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    eden::passwords::CredentialVault vault(directory.filePath("vault.sqlite"), "session-test");
    QVERIFY(vault.initialize());
    const QString path = directory.filePath("session.json");
    const QByteArray original = QJsonDocument(savedSession()).toJson();
    QVERIFY(writeFile(path, original));
    QVERIFY(QFile::setPermissions(directory.path(), QFile::ReadOwner | QFile::ExeOwner));
    const auto restore = qScopeGuard([&] {
        QFile::setPermissions(directory.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    });
    eden::core::SessionStore sessions(path);
    sessions.setVault(&vault);
    QVERIFY(sessions.load().isEmpty());
    QCOMPARE(sessions.property("errorCode").toString(), QString("session_write_failed"));
    QCOMPARE(readFile(path), original);
    QVERIFY(QFile::setPermissions(directory.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QVERIFY(!sessions.write({}));
    QCOMPARE(readFile(path), original);
    QCOMPARE(sessions.load(), savedSession());
    QVERIFY(sessions.property("errorCode").toString().isEmpty());
}

void SessionStoreTest::reportsWriteFailures() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    eden::passwords::CredentialVault vault(directory.filePath("vault.sqlite"), "session-test");
    QVERIFY(vault.initialize());
    const QString path = directory.filePath("session.json");
    eden::core::SessionStore sessions(path);
    sessions.setVault(&vault);
    QVERIFY(sessions.write(savedSession()));
    const QByteArray original = readFile(path);
    QVERIFY(QFile::setPermissions(directory.path(), QFile::ReadOwner | QFile::ExeOwner));
    const auto restore = qScopeGuard([&] {
        QFile::setPermissions(directory.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    });
    QVERIFY(!sessions.write({}));
    QCOMPARE(sessions.property("errorCode").toString(), QString("session_write_failed"));
    QVERIFY(!sessions.property("errorMessage").toString().isEmpty());
    QCOMPARE(readFile(path), original);
    QVERIFY(QFile::setPermissions(directory.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QVERIFY(sessions.write({}));
    QVERIFY(sessions.property("errorCode").toString().isEmpty());
}

void SessionStoreTest::retriesUnavailableVault() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    eden::passwords::CredentialVault vault(directory.filePath("vault.sqlite"), "session-test");
    QVERIFY(vault.initialize());
    const QString path = directory.filePath("session.json");
    const QByteArray original = sealedSession(vault, savedSession());
    QVERIFY(writeFile(path, original));
    eden::core::SessionStore sessions(path);
    QVERIFY(sessions.load().isEmpty());
    QCOMPARE(sessions.property("errorCode").toString(), QString("session_read_failed"));
    sessions.setVault(&vault);
    QVERIFY(!sessions.write({}));
    QCOMPARE(readFile(path), original);
    QCOMPARE(sessions.load(), savedSession());
    QVERIFY(sessions.property("errorCode").toString().isEmpty());
}

QTEST_GUILESS_MAIN(SessionStoreTest)

#include "sessionstore_test.moc"

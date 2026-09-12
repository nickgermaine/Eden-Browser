#include <libsecret/secret.h>

#include "core/profiles/enginestorage.h"
#include "core/profiles/profilemigration.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <QDirIterator>
#include <QFile>
#include <QProcess>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QtTest>
#include <sodium.h>

#include <optional>

namespace {
    std::optional<QByteArray> storedKey;
    bool lookupFailure = false;
    bool storeFailure = false;
    int storeCalls = 0;

    bool writeFile(const QString &path, const QByteArray &data) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
    }

    QByteArray readFile(const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    void allowCleanup(const eden::core::ProfilePaths::Roots &roots) {
        QFile::setPermissions(roots.dataRoot + "/webengine", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QFile::setPermissions(roots.cacheRoot + "/webengine", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QFile::setPermissions(roots.dataRoot + "/engine-data", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QFile::setPermissions(roots.cacheRoot + "/profiles", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
}

extern "C" gchar *secret_password_lookup_sync(const SecretSchema *, GCancellable *, GError **error, ...) {
    if (lookupFailure) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Synthetic keyring failure");
        return nullptr;
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
    if (storeFailure) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Synthetic keyring failure");
        return false;
    }
    storedKey = QByteArray(password);
    return true;
}

class EngineStorageTest final : public QObject {
    Q_OBJECT
  private slots:
    void init();
    void migrationPersistenceAndRevocation();
    void preservesSourceAndRetriesMigration();
    void helperFailureCannotWritePlaintext();
    void resumesInterruptedLegacyCopy();
    void keyringFailuresAreRetryable();
    void unreadableMigrationPreservesSource();
    void recoversDisconnectedMount();
};

void EngineStorageTest::init() {
    storedKey.reset();
    lookupFailure = false;
    storeFailure = false;
    storeCalls = 0;
}

void EngineStorageTest::migrationPersistenceAndRevocation() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const eden::core::ProfilePaths::Roots roots{directory.filePath("data"), directory.filePath("cache")};
    const QString file = roots.dataRoot + "/engine-data/cef/profiles/fixture/Local Storage/secret-name";
    const QString cache = roots.cacheRoot + "/profiles/fixture/qtwebengine/secret-cache";
    const QByteArray marker("eden-private-engine-canary-7364eeff-2947");
    QVERIFY(writeFile(file, marker));
    QVERIFY(writeFile(cache, marker));
    QVERIFY(writeFile(roots.dataRoot + "/webengine/legacy-site-data", marker));
    QVERIFY(writeFile(roots.cacheRoot + "/webengine/legacy-site-cache", marker));
    eden::core::EngineStorage storage;
    std::optional<QString> result;
    storage.prepare(roots, [&](const QString &error) { result = error; });
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 40000);
    QVERIFY2(result->isEmpty(), qPrintable(*result));
    QVERIFY(storage.protects(file));
    QVERIFY(storage.protects(cache));
    QVERIFY(storage.protects(roots.dataRoot + "/webengine/legacy-site-data"));
    QVERIFY(storage.protects(roots.cacheRoot + "/webengine/legacy-site-cache"));
    QVERIFY(!storage.protects(directory.filePath("unprotected")));
    QCOMPARE(readFile(file), marker);
    QCOMPARE(readFile(cache), marker);
    QVERIFY(!QFileInfo::exists(roots.dataRoot + "/engine-data.plaintext-migration"));
    QVERIFY(writeFile(file, marker + "-updated"));
    storage.shutdown();
    QVERIFY(!storage.protects(file));
    QVERIFY(!writeFile(file, "plaintext-fallback"));
    for (const QString &cipher :
         {roots.dataRoot + "/engine-data.encrypted",
          roots.cacheRoot + "/profiles.encrypted",
          roots.dataRoot + "/webengine.encrypted",
          roots.cacheRoot + "/webengine.encrypted"}) {
        QDirIterator entries(cipher, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (entries.hasNext()) {
            const QString path = entries.next();
            QVERIFY(!path.contains("secret-name"));
            QVERIFY(!readFile(path).contains(marker));
        }
    }
    result.reset();
    storage.prepare(roots, [&](const QString &error) { result = error; });
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 40000);
    QVERIFY2(result->isEmpty(), qPrintable(*result));
    QCOMPARE(readFile(file), marker + "-updated");
    QCOMPARE(readFile(cache), marker);
    storage.shutdown();
    const QByteArray key = *storedKey;
    storedKey.reset();
    result.reset();
    storage.prepare(roots, [&](const QString &error) { result = error; });
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 10000);
    QVERIFY(!result->isEmpty());
    QCOMPARE(storeCalls, 1);
    QVERIFY(!storage.protects(file));
    storedKey = QByteArray(32, 'x').toBase64();
    result.reset();
    storage.prepare(roots, [&](const QString &error) { result = error; });
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 40000);
    QVERIFY(!result->isEmpty());
    storedKey = key;
    result.reset();
    storage.prepare(roots, [&](const QString &error) { result = error; });
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 40000);
    QVERIFY2(result->isEmpty(), qPrintable(*result));
    QCOMPARE(readFile(file), marker + "-updated");
    storage.shutdown();
    allowCleanup(roots);
}

void EngineStorageTest::preservesSourceAndRetriesMigration() {
    QTemporaryDir directory;
    const eden::core::ProfilePaths::Roots roots{directory.filePath("data"), directory.filePath("cache")};
    const QString source = roots.dataRoot + "/engine-data";
    QVERIFY(writeFile(source + "/good", "preserve-every-byte"));
    QVERIFY(QFile::link(directory.path(), source + "/unsafe-link"));
    eden::core::EngineStorage storage;
    std::optional<QString> result;
    storage.prepare(roots, [&](const QString &error) { result = error; });
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 40000);
    QVERIFY(!result->isEmpty());
    QCOMPARE(readFile(source + ".plaintext-migration/good"), QByteArray("preserve-every-byte"));
    QVERIFY(!storage.protects(source));
    QVERIFY(QFile::remove(source + ".plaintext-migration/unsafe-link"));
    result.reset();
    storage.prepare(roots, [&](const QString &error) { result = error; });
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 40000);
    QVERIFY2(result->isEmpty(), qPrintable(*result));
    QCOMPARE(readFile(source + "/good"), QByteArray("preserve-every-byte"));
    storage.shutdown();
    allowCleanup(roots);
}

void EngineStorageTest::resumesInterruptedLegacyCopy() {
    QTemporaryDir directory;
    const eden::core::ProfilePaths::Roots roots{directory.filePath("data"), directory.filePath("cache")};
    const QString source = roots.dataRoot + "/webengine";
    QVERIFY(writeFile(source + "/one", "first-complete-record"));
    QVERIFY(writeFile(source + "/sub/two", "second-complete-record"));
    eden::core::EngineStorage storage;
    std::optional<QString> result;
    storage.prepare(roots, [&](const QString &error) { result = error; });
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 40000);
    QVERIFY2(result->isEmpty(), qPrintable(*result));
    const eden::core::ProfilePaths paths(roots, eden::core::ProfileId::generate());
    QVERIFY(paths.ensureBaseDirectories());
    const QString target = paths.engineDataDirectory("qtwebengine");
    QVERIFY(writeFile(target + "/one", "incomplete"));
    const QJsonObject journal{
        {"version", 1},
        {"operations", QJsonArray{QJsonObject{{"source", source}, {"destination", target}, {"operation", "move"}}}}
    };
    QVERIFY(writeFile(eden::core::ProfileMigration::journalPath(roots), QJsonDocument(journal).toJson()));
    QCOMPARE(eden::core::ProfileMigration::resume(paths), eden::core::ProfileError::None);
    QCOMPARE(readFile(target + "/one"), QByteArray("first-complete-record"));
    QCOMPARE(readFile(target + "/sub/two"), QByteArray("second-complete-record"));
    QVERIFY(QDir(source).isEmpty());
    QVERIFY(storage.protects(target));
    QCOMPARE(eden::core::ProfileMigration::resume(paths), eden::core::ProfileError::None);
    QCOMPARE(readFile(target + "/one"), QByteArray("first-complete-record"));
    storage.shutdown();
    allowCleanup(roots);
}

void EngineStorageTest::helperFailureCannotWritePlaintext() {
    QTemporaryDir directory;
    const eden::core::ProfilePaths::Roots roots{directory.filePath("data"), directory.filePath("cache")};
    eden::core::EngineStorage storage;
    QSignalSpy failed(&storage, &eden::core::EngineStorage::storageFailed);
    std::optional<QString> result;
    storage.prepare(roots, [&](const QString &error) { result = error; });
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 40000);
    QVERIFY2(result->isEmpty(), qPrintable(*result));
    QProcess unmount;
    unmount.start("fusermount3", {"-u", roots.dataRoot + "/engine-data"});
    QVERIFY(unmount.waitForFinished(10000));
    QCOMPARE(unmount.exitCode(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 10000);
    QVERIFY(!storage.protects(roots.dataRoot + "/engine-data"));
    QVERIFY(!writeFile(roots.dataRoot + "/engine-data/plaintext-fallback", "secret"));
    storage.shutdown();
    allowCleanup(roots);
}

void EngineStorageTest::keyringFailuresAreRetryable() {
    QTemporaryDir directory;
    const eden::core::ProfilePaths::Roots roots{directory.filePath("data"), directory.filePath("cache")};
    eden::core::EngineStorage storage;
    for (int stage = 0; stage < 3; ++stage) {
        lookupFailure = stage == 0;
        storeFailure = stage == 1;
        std::optional<QString> result;
        storage.prepare(roots, [&](const QString &error) { result = error; });
        QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 40000);
        QCOMPARE(result->isEmpty(), stage == 2);
    }
    storage.shutdown();
    allowCleanup(roots);
}

void EngineStorageTest::unreadableMigrationPreservesSource() {
    QTemporaryDir directory;
    const QString source = directory.filePath("source");
    const QString destination = directory.filePath("destination");
    QVERIFY(writeFile(source + "/locked/record", "preserved-record"));
    QVERIFY(QFile::setPermissions(source + "/locked", {}));
    QVERIFY(!eden::core::EngineStorage::migrateLegacyPath(source, destination));
    QVERIFY(QFile::setPermissions(source + "/locked", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QCOMPARE(readFile(source + "/locked/record"), QByteArray("preserved-record"));
    QVERIFY(eden::core::EngineStorage::migrateLegacyPath(source, destination));
    QCOMPARE(readFile(destination + "/locked/record"), QByteArray("preserved-record"));
}

void EngineStorageTest::recoversDisconnectedMount() {
    QTemporaryDir directory;
    const eden::core::ProfilePaths::Roots roots{directory.filePath("data"), directory.filePath("cache")};
    eden::core::EngineStorage storage;
    std::optional<QString> result;
    storage.prepare(roots, [&](const QString &error) { result = error; });
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 40000);
    QVERIFY2(result->isEmpty(), qPrintable(*result));
    const QString view = roots.dataRoot + "/engine-data";
    QVERIFY(writeFile(view + "/record", "survives-helper-crash"));
    storage.shutdown();
    allowCleanup(roots);
    const QByteArray key = QByteArray::fromBase64(*storedKey);
    const QByteArray purpose("eden-site-data-v1");
    QByteArray password(32, Qt::Uninitialized);
    QCOMPARE(
        crypto_generichash(
            reinterpret_cast<unsigned char *>(password.data()),
            32,
            reinterpret_cast<const unsigned char *>(purpose.constData()),
            purpose.size(),
            reinterpret_cast<const unsigned char *>(key.constData()),
            key.size()
        ),
        0
    );
    QProcess helper;
    helper.setStandardOutputFile(QProcess::nullDevice());
    helper.setStandardErrorFile(QProcess::nullDevice());
    helper.start(
        QCoreApplication::applicationDirPath() + "/eden-gocryptfs",
        {"-q", "-fg", "-passfile", "/dev/stdin", "-fsname", view + ".encrypted", view + ".encrypted", view}
    );
    QVERIFY(helper.waitForStarted());
    helper.write(password.toHex() + '\n');
    helper.closeWriteChannel();
    QTRY_COMPARE_WITH_TIMEOUT(QStorageInfo(view).rootPath(), view, 10000);
    helper.kill();
    QVERIFY(helper.waitForFinished(10000));
    QVERIFY(!writeFile(view + "/plaintext-fallback", "secret"));
    result.reset();
    storage.prepare(roots, [&](const QString &error) { result = error; });
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 40000);
    QVERIFY2(result->isEmpty(), qPrintable(*result));
    QCOMPARE(readFile(view + "/record"), QByteArray("survives-helper-crash"));
    storage.shutdown();
    allowCleanup(roots);
}

QTEST_GUILESS_MAIN(EngineStorageTest)
#include "enginestorage_test.moc"

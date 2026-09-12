#include "core/profiles/profileregistry.h"

#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

using eden::core::ProfileError;
using eden::core::ProfileId;
using eden::core::ProfileLifecycle;
using eden::core::ProfileRecord;
using eden::core::ProfileRegistry;
using eden::core::ProfileSummary;

class ProfileRegistryTest final : public QObject {
    Q_OBJECT

  private slots:
    void createsFreshRegistryAndPersistsProfiles();
    void reopenReportsLoadedAndKeepsData();
    void commitReadyStoresVerifierAndProtectionFlag();
    void malformedRegistryEntersRecovery();
    void unknownLifecycleEntersRecovery();
    void removalReassignsLastActiveInOneTransaction();
    void processLockRejectsSecondWriter();
    void applicationStateRoundTrips();
    void identityFieldsUpdateAndPublish();

  private:
    ProfileRecord makeRecord(const QString &name) {
        ProfileRecord record;
        record.id = ProfileId::generate();
        record.displayName = name;
        record.colorSeed = 7;
        record.createdAt = 1000;
        record.lastUsedAt = 1000;
        return record;
    }

    bool waitForCallback(
        ProfileRegistry &registry,
        std::function<void(ProfileRegistry::VoidCallback)> operation,
        ProfileError *result = nullptr
    ) {
        bool finished = false;
        ProfileError observed = ProfileError::None;
        operation([&finished, &observed](ProfileError error) {
            observed = error;
            finished = true;
        });
        const bool completed = QTest::qWaitFor([&finished] { return finished; }, 5000);
        if (result) {
            *result = observed;
        }
        return completed;
    }
};

void ProfileRegistryTest::createsFreshRegistryAndPersistsProfiles() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath("profiles.sqlite");
    ProfileRegistry registry(databasePath);
    QSignalSpy loaded(&registry, &ProfileRegistry::loadFinished);
    registry.loadAsync();
    QTRY_COMPARE(loaded.size(), 1);
    QCOMPARE(
        loaded.constFirst().constFirst().value<ProfileRegistry::LoadStatus>(),
        ProfileRegistry::LoadStatus::Created
    );

    const ProfileRecord record = makeRecord("First");
    ProfileError error = ProfileError::None;
    QVERIFY(waitForCallback(
        registry,
        [&](ProfileRegistry::VoidCallback callback) { registry.createProfile(record, std::move(callback)); },
        &error
    ));
    QCOMPARE(error, ProfileError::None);
    QCOMPARE(registry.snapshot().profiles.size(), 1);
    QCOMPARE(registry.snapshot().profiles.constFirst().lifecycle, ProfileLifecycle::Creating);
    QCOMPARE(registry.snapshot().profiles.constFirst().displayName, QString("First"));
    QVERIFY(!registry.snapshot().profiles.constFirst().protectedProfile);

    ProfileError duplicateError = ProfileError::None;
    QVERIFY(waitForCallback(
        registry,
        [&](ProfileRegistry::VoidCallback callback) { registry.createProfile(record, std::move(callback)); },
        &duplicateError
    ));
    QCOMPARE(duplicateError, ProfileError::RegistryWriteFailed);
    QCOMPARE(registry.snapshot().profiles.size(), 1);
}

void ProfileRegistryTest::reopenReportsLoadedAndKeepsData() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath("profiles.sqlite");
    ProfileRecord record = makeRecord("Persistent");
    {
        ProfileRegistry registry(databasePath);
        QSignalSpy loaded(&registry, &ProfileRegistry::loadFinished);
        registry.loadAsync();
        QTRY_COMPARE(loaded.size(), 1);
        QVERIFY(waitForCallback(registry, [&](ProfileRegistry::VoidCallback callback) {
            registry.createProfile(record, std::move(callback));
        }));
        QVERIFY(waitForCallback(registry, [&](ProfileRegistry::VoidCallback callback) {
            registry.commitProfileReady(record.id, {}, std::move(callback));
        }));
    }
    ProfileRegistry reopened(databasePath);
    QSignalSpy loaded(&reopened, &ProfileRegistry::loadFinished);
    reopened.loadAsync();
    QTRY_COMPARE(loaded.size(), 1);
    QCOMPARE(
        loaded.constFirst().constFirst().value<ProfileRegistry::LoadStatus>(),
        ProfileRegistry::LoadStatus::Loaded
    );
    QCOMPARE(reopened.snapshot().profiles.size(), 1);
    QCOMPARE(reopened.snapshot().profiles.constFirst().displayName, QString("Persistent"));
    QCOMPARE(reopened.snapshot().profiles.constFirst().lifecycle, ProfileLifecycle::Ready);
}

void ProfileRegistryTest::commitReadyStoresVerifierAndProtectionFlag() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ProfileRegistry registry(directory.filePath("profiles.sqlite"));
    QSignalSpy loaded(&registry, &ProfileRegistry::loadFinished);
    registry.loadAsync();
    QTRY_COMPARE(loaded.size(), 1);
    const ProfileRecord record = makeRecord("Locked");
    QVERIFY(waitForCallback(registry, [&](ProfileRegistry::VoidCallback callback) {
        registry.createProfile(record, std::move(callback));
    }));
    QVERIFY(waitForCallback(registry, [&](ProfileRegistry::VoidCallback callback) {
        registry.commitProfileReady(record.id, QStringLiteral("$argon2id$fake"), std::move(callback));
    }));
    QVERIFY(registry.snapshot().profiles.constFirst().protectedProfile);

    bool fetched = false;
    QString verifier;
    registry.fetchPasswordVerifier(record.id, [&fetched, &verifier](ProfileError error, QString value) {
        QCOMPARE(error, ProfileError::None);
        verifier = value;
        fetched = true;
    });
    QTRY_VERIFY(fetched);
    QCOMPARE(verifier, QString("$argon2id$fake"));

    QVERIFY(waitForCallback(registry, [&](ProfileRegistry::VoidCallback callback) {
        registry.updatePasswordVerifier(record.id, QString(), std::move(callback));
    }));
    QVERIFY(!registry.snapshot().profiles.constFirst().protectedProfile);
}

void ProfileRegistryTest::identityFieldsUpdateAndPublish() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ProfileRegistry registry(directory.filePath("profiles.sqlite"));
    QSignalSpy loaded(&registry, &ProfileRegistry::loadFinished);
    registry.loadAsync();
    QTRY_COMPARE(loaded.size(), 1);
    const ProfileRecord record = makeRecord("Original");
    QVERIFY(waitForCallback(registry, [&](ProfileRegistry::VoidCallback callback) {
        registry.createProfile(record, std::move(callback));
    }));

    QVERIFY(waitForCallback(registry, [&](ProfileRegistry::VoidCallback callback) {
        registry.updateDisplayName(record.id, "Renamed", std::move(callback));
    }));
    QCOMPARE(registry.snapshot().profiles.constFirst().displayName, QString("Renamed"));

    QVERIFY(waitForCallback(registry, [&](ProfileRegistry::VoidCallback callback) {
        registry.updateColorSeed(record.id, 11, std::move(callback));
    }));
    QCOMPARE(registry.snapshot().profiles.constFirst().colorSeed, 11U);

    QVERIFY(waitForCallback(registry, [&](ProfileRegistry::VoidCallback callback) {
        registry.incrementAvatarRevision(record.id, std::move(callback));
    }));
    QCOMPARE(registry.snapshot().profiles.constFirst().avatarRevision, 1);
}

void ProfileRegistryTest::malformedRegistryEntersRecovery() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath("profiles.sqlite");
    QFile garbage(databasePath);
    QVERIFY(garbage.open(QIODevice::WriteOnly));
    garbage.write("this is not a sqlite database and it is long enough to have a header");
    garbage.close();
    ProfileRegistry registry(databasePath);
    QSignalSpy loaded(&registry, &ProfileRegistry::loadFinished);
    registry.loadAsync();
    QTRY_COMPARE(loaded.size(), 1);
    QCOMPARE(
        loaded.constFirst().constFirst().value<ProfileRegistry::LoadStatus>(),
        ProfileRegistry::LoadStatus::Recovery
    );
    QVERIFY(registry.snapshot().profiles.isEmpty());
}

void ProfileRegistryTest::unknownLifecycleEntersRecovery() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath("profiles.sqlite");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", "registry-tamper");
        database.setDatabaseName(databasePath);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec("CREATE TABLE schema_version(version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL)"));
        QVERIFY(query.exec("INSERT INTO schema_version VALUES(1, 0)"));
        QVERIFY(query.exec(
            "CREATE TABLE profiles(id TEXT PRIMARY KEY, display_name TEXT NOT NULL, color_seed INTEGER NOT NULL, "
            "avatar_revision INTEGER NOT NULL DEFAULT 0, password_verifier TEXT, lifecycle TEXT NOT NULL, "
            "created_at INTEGER NOT NULL, last_used_at INTEGER NOT NULL)"
        ));
        QVERIFY(query.exec(
            "INSERT INTO profiles VALUES('0c5a9b7e-1f2d-4e3a-8b6c-9d0e1f2a3b4c', 'Broken', 1, 0, NULL, "
            "'Exploded', 5, 5)"
        ));
        QVERIFY(query.exec("CREATE TABLE application_state(key TEXT PRIMARY KEY, value TEXT NOT NULL)"));
        database.close();
    }
    QSqlDatabase::removeDatabase("registry-tamper");
    ProfileRegistry registry(databasePath);
    QSignalSpy loaded(&registry, &ProfileRegistry::loadFinished);
    registry.loadAsync();
    QTRY_COMPARE(loaded.size(), 1);
    QCOMPARE(
        loaded.constFirst().constFirst().value<ProfileRegistry::LoadStatus>(),
        ProfileRegistry::LoadStatus::Recovery
    );
}

void ProfileRegistryTest::removalReassignsLastActiveInOneTransaction() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ProfileRegistry registry(directory.filePath("profiles.sqlite"));
    QSignalSpy loaded(&registry, &ProfileRegistry::loadFinished);
    registry.loadAsync();
    QTRY_COMPARE(loaded.size(), 1);
    const ProfileRecord first = makeRecord("First");
    const ProfileRecord second = makeRecord("Second");
    for (const ProfileRecord &record : {first, second}) {
        QVERIFY(waitForCallback(registry, [&](ProfileRegistry::VoidCallback callback) {
            registry.createProfile(record, std::move(callback));
        }));
        QVERIFY(waitForCallback(registry, [&](ProfileRegistry::VoidCallback callback) {
            registry.commitProfileReady(record.id, {}, std::move(callback));
        }));
    }
    registry.touchLastUsed(first.id, 5000, true);
    QTRY_COMPARE(
        registry.applicationStateValue(QLatin1String(eden::core::applicationstate::lastActiveProfileId)),
        first.id.toString()
    );
    ProfileError error = ProfileError::None;
    QVERIFY(waitForCallback(
        registry,
        [&](ProfileRegistry::VoidCallback callback) {
            registry.removeProfile(first.id, second.id.toString(), std::move(callback));
        },
        &error
    ));
    QCOMPARE(error, ProfileError::None);
    QCOMPARE(registry.snapshot().profiles.size(), 1);
    QCOMPARE(registry.snapshot().profiles.constFirst().profileId, second.id.toString());
    QCOMPARE(
        registry.applicationStateValue(QLatin1String(eden::core::applicationstate::lastActiveProfileId)),
        second.id.toString()
    );

    ProfileError missingError = ProfileError::None;
    QVERIFY(waitForCallback(
        registry,
        [&](ProfileRegistry::VoidCallback callback) {
            registry.removeProfile(first.id, second.id.toString(), std::move(callback));
        },
        &missingError
    ));
    QCOMPARE(missingError, ProfileError::RegistryWriteFailed);
}

void ProfileRegistryTest::processLockRejectsSecondWriter() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString lockPath = directory.filePath("profiles.sqlite.lock");
    ProfileRegistry firstWriter(directory.filePath("profiles.sqlite"));
    QVERIFY(firstWriter.acquireProcessLock(lockPath));
    QVERIFY(firstWriter.processLockHeld());
    ProfileRegistry secondWriter(directory.filePath("profiles.sqlite"));
    QVERIFY(!secondWriter.acquireProcessLock(lockPath));
    QVERIFY(!secondWriter.processLockHeld());
}

void ProfileRegistryTest::applicationStateRoundTrips() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ProfileRegistry registry(directory.filePath("profiles.sqlite"));
    QSignalSpy loaded(&registry, &ProfileRegistry::loadFinished);
    registry.loadAsync();
    QTRY_COMPARE(loaded.size(), 1);
    QVERIFY(waitForCallback(registry, [&](ProfileRegistry::VoidCallback callback) {
        registry.setApplicationState(
            QLatin1String(eden::core::applicationstate::startupDisposition),
            QStringLiteral("ChooseProfile"),
            std::move(callback)
        );
    }));
    QCOMPARE(
        registry.applicationStateValue(QLatin1String(eden::core::applicationstate::startupDisposition)),
        QString("ChooseProfile")
    );
    QVERIFY(waitForCallback(registry, [&](ProfileRegistry::VoidCallback callback) {
        registry.setApplicationState(
            QLatin1String(eden::core::applicationstate::startupDisposition),
            QStringLiteral("ResumeLast"),
            std::move(callback)
        );
    }));
    QCOMPARE(
        registry.applicationStateValue(QLatin1String(eden::core::applicationstate::startupDisposition)),
        QString("ResumeLast")
    );
}

QTEST_GUILESS_MAIN(ProfileRegistryTest)
#include "profileregistry_test.moc"

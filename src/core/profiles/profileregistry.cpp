#include "core/profiles/profileregistry.h"

#include "core/profiles/profilepaths.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace eden::core {

    class ProfileRegistryWorker final : public QObject {
      public:
        ProfileRegistryWorker(ProfileRegistry *facade, QString databasePath)
            : m_facade(facade),
              m_databasePath(std::move(databasePath)),
              m_connectionName(
                  QStringLiteral("eden-profile-registry-") + QUuid::createUuid().toString(QUuid::WithoutBraces)
              ) {}

        ~ProfileRegistryWorker() override {
            if (m_database.isValid()) {
                m_database.close();
                m_database = QSqlDatabase();
                QSqlDatabase::removeDatabase(m_connectionName);
            }
        }

        void load() {
            if (m_database.isValid()) {
                m_database.close();
                m_database = QSqlDatabase();
                QSqlDatabase::removeDatabase(m_connectionName);
            }
            const bool existed = QFileInfo::exists(m_databasePath);
            QDir().mkpath(QFileInfo(m_databasePath).absolutePath());
            m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
            m_database.setDatabaseName(m_databasePath);
            if (!m_database.open()) {
                finishLoad(ProfileRegistry::LoadStatus::Recovery);
                return;
            }
            applyConnectionPragmas();
            if (existed && !passesIntegrityCheck()) {
                finishLoad(ProfileRegistry::LoadStatus::Recovery);
                return;
            }
            if (!ensureSchema()) {
                finishLoad(ProfileRegistry::LoadStatus::Recovery);
                return;
            }
            ProfilePaths::restrictFile(m_databasePath);
            ProfilePaths::restrictFile(m_databasePath + QStringLiteral("-wal"));
            ProfilePaths::restrictFile(m_databasePath + QStringLiteral("-shm"));
            ProfileRegistry::Snapshot snapshot;
            if (!readSnapshot(snapshot)) {
                finishLoad(ProfileRegistry::LoadStatus::Recovery);
                return;
            }
            publish(std::move(snapshot));
            finishLoad(existed ? ProfileRegistry::LoadStatus::Loaded : ProfileRegistry::LoadStatus::Created);
        }

        void createProfile(const ProfileRecord &draft, ProfileRegistry::VoidCallback callback) {
            mutate(std::move(callback), [&draft](QSqlDatabase &database) {
                QSqlQuery query(database);
                query.prepare(QStringLiteral(
                    "INSERT INTO profiles(id, display_name, color_seed, avatar_revision, password_verifier, lifecycle, "
                    "created_at, "
                    "last_used_at) VALUES(?, ?, ?, 0, NULL, ?, ?, ?)"
                ));
                query.addBindValue(draft.id.toString());
                query.addBindValue(draft.displayName);
                query.addBindValue(draft.colorSeed);
                query.addBindValue(profileLifecycleName(ProfileLifecycle::Creating));
                query.addBindValue(draft.createdAt);
                query.addBindValue(draft.lastUsedAt);
                return query.exec();
            });
        }

        void commitProfileReady(const QString &id, const QString &verifier, ProfileRegistry::VoidCallback callback) {
            mutate(std::move(callback), [&id, &verifier](QSqlDatabase &database) {
                QSqlQuery query(database);
                query.prepare(QStringLiteral("UPDATE profiles SET lifecycle = ?, password_verifier = ? WHERE id = ?"));
                query.addBindValue(profileLifecycleName(ProfileLifecycle::Ready));
                query.addBindValue(verifier.isEmpty() ? QVariant(QMetaType::fromType<QString>()) : QVariant(verifier));
                query.addBindValue(id);
                return query.exec() && query.numRowsAffected() == 1;
            });
        }

        void updateField(
            const QString &id,
            const QString &statement,
            const QVariant &value,
            ProfileRegistry::VoidCallback callback
        ) {
            mutate(std::move(callback), [&](QSqlDatabase &database) {
                QSqlQuery query(database);
                query.prepare(statement);
                if (value.isValid()) {
                    query.addBindValue(value);
                }
                query.addBindValue(id);
                return query.exec() && query.numRowsAffected() == 1;
            });
        }

        void touchLastUsed(const QString &id, qint64 timestamp, bool recordLastActive) {
            mutate({}, [&](QSqlDatabase &database) {
                QSqlQuery query(database);
                query.prepare(QStringLiteral("UPDATE profiles SET last_used_at = ? WHERE id = ?"));
                query.addBindValue(timestamp);
                query.addBindValue(id);
                if (!query.exec()) {
                    return false;
                }
                if (recordLastActive) {
                    return writeApplicationState(database, QLatin1String(applicationstate::lastActiveProfileId), id);
                }
                return true;
            });
        }

        void removeProfile(const QString &id, const QString &nextLastActiveId, ProfileRegistry::VoidCallback callback) {
            mutate(std::move(callback), [&](QSqlDatabase &database) {
                QSqlQuery query(database);
                query.prepare(QStringLiteral("DELETE FROM profiles WHERE id = ?"));
                query.addBindValue(id);
                if (!query.exec() || query.numRowsAffected() != 1) {
                    return false;
                }
                return writeApplicationState(
                    database,
                    QLatin1String(applicationstate::lastActiveProfileId),
                    nextLastActiveId
                );
            });
        }

        void setApplicationState(const QString &key, const QString &value, ProfileRegistry::VoidCallback callback) {
            mutate(std::move(callback), [&](QSqlDatabase &database) {
                return writeApplicationState(database, key, value);
            });
        }

        void fetchPasswordVerifier(const QString &id, ProfileRegistry::VerifierCallback callback) {
            QSqlQuery query(m_database);
            query.prepare(QStringLiteral("SELECT password_verifier FROM profiles WHERE id = ?"));
            query.addBindValue(id);
            QString verifier;
            ProfileError error = ProfileError::None;
            if (!query.exec()) {
                error = ProfileError::RegistryBusy;
            } else if (!query.next()) {
                error = ProfileError::ProfileMissing;
            } else {
                verifier = query.value(0).toString();
            }
            if (callback) {
                QMetaObject::invokeMethod(
                    m_facade,
                    [callback = std::move(callback), error, verifier] { callback(error, verifier); },
                    Qt::QueuedConnection
                );
            }
        }

      private:
        void applyConnectionPragmas() {
            QSqlQuery query(m_database);
            query.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
            query.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
            query.exec(QStringLiteral("PRAGMA busy_timeout=2500"));
            query.exec(QStringLiteral("PRAGMA synchronous=FULL"));
        }

        bool passesIntegrityCheck() {
            QSqlQuery query(m_database);
            if (!query.exec(QStringLiteral("PRAGMA quick_check"))) {
                return false;
            }
            return query.next() && query.value(0).toString() == QLatin1String("ok");
        }

        bool ensureSchema() {
            QSqlQuery query(m_database);
            if (!query.exec(QStringLiteral(
                    "CREATE TABLE IF NOT EXISTS schema_version(version INTEGER PRIMARY KEY, applied_at INTEGER "
                    "NOT NULL)"
                ))) {
                return false;
            }
            if (!query.exec(QStringLiteral(
                    "CREATE TABLE IF NOT EXISTS profiles(id TEXT PRIMARY KEY, display_name TEXT NOT NULL, color_seed "
                    "INTEGER NOT NULL, "
                    "avatar_revision INTEGER NOT NULL DEFAULT 0, password_verifier TEXT, lifecycle TEXT NOT NULL "
                    "CHECK(lifecycle IN "
                    "('Creating', 'Ready', 'Deleting')), created_at INTEGER NOT NULL, last_used_at INTEGER NOT NULL)"
                ))) {
                return false;
            }
            if (!query.exec(QStringLiteral(
                    "CREATE TABLE IF NOT EXISTS application_state(key TEXT PRIMARY KEY, value TEXT NOT NULL)"
                ))) {
                return false;
            }
            QSqlQuery versionQuery(m_database);
            versionQuery.prepare(
                QStringLiteral("INSERT OR IGNORE INTO schema_version(version, applied_at) VALUES(1, ?)")
            );
            versionQuery.addBindValue(QDateTime::currentMSecsSinceEpoch());
            if (!versionQuery.exec()) {
                return false;
            }
            QSqlQuery maximumVersion(m_database);
            if (!maximumVersion.exec(QStringLiteral("SELECT MAX(version) FROM schema_version")) ||
                !maximumVersion.next()) {
                return false;
            }
            return maximumVersion.value(0).toInt() == 1;
        }

        static bool writeApplicationState(QSqlDatabase &database, const QString &key, const QString &value) {
            QSqlQuery query(database);
            query.prepare(QStringLiteral(
                "INSERT INTO application_state(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value = "
                "excluded.value"
            ));
            query.addBindValue(key);
            query.addBindValue(value);
            return query.exec();
        }

        bool readSnapshot(ProfileRegistry::Snapshot &snapshot) {
            QSqlQuery query(m_database);
            if (!query.exec(QStringLiteral(
                    "SELECT id, display_name, color_seed, avatar_revision, password_verifier IS NOT NULL, "
                    "lifecycle, created_at, last_used_at FROM profiles"
                ))) {
                return false;
            }
            while (query.next()) {
                const QString identifier = query.value(0).toString();
                const std::optional<ProfileLifecycle> lifecycle = profileLifecycleFromName(query.value(5).toString());
                if (!ProfileId::isCanonical(identifier) || !lifecycle) {
                    return false;
                }
                ProfileSummary summary;
                summary.profileId = identifier;
                summary.displayName = query.value(1).toString();
                summary.colorSeed = query.value(2).toUInt();
                summary.avatarRevision = query.value(3).toInt();
                summary.protectedProfile = query.value(4).toBool();
                summary.lifecycle = *lifecycle;
                summary.createdAt = query.value(6).toLongLong();
                summary.lastUsedAt = query.value(7).toLongLong();
                snapshot.profiles.append(summary);
            }
            QSqlQuery stateQuery(m_database);
            if (!stateQuery.exec(QStringLiteral("SELECT key, value FROM application_state"))) {
                return false;
            }
            while (stateQuery.next()) {
                snapshot.applicationState.insert(stateQuery.value(0).toString(), stateQuery.value(1).toString());
            }
            return true;
        }

        void mutate(ProfileRegistry::VoidCallback callback, const std::function<bool(QSqlDatabase &)> &operation) {
            ProfileError error = ProfileError::None;
            if (!m_database.isOpen()) {
                error = ProfileError::RegistryOpenFailed;
            } else if (!m_database.transaction()) {
                error = ProfileError::RegistryBusy;
            } else if (!operation(m_database)) {
                m_database.rollback();
                error = ProfileError::RegistryWriteFailed;
            } else if (!m_database.commit()) {
                m_database.rollback();
                error = ProfileError::RegistryBusy;
            }
            if (error == ProfileError::None) {
                ProfilePaths::restrictFile(m_databasePath);
                ProfilePaths::restrictFile(m_databasePath + QStringLiteral("-wal"));
                ProfilePaths::restrictFile(m_databasePath + QStringLiteral("-shm"));
                ProfileRegistry::Snapshot snapshot;
                if (readSnapshot(snapshot)) {
                    publish(std::move(snapshot));
                }
            }
            if (callback) {
                QMetaObject::invokeMethod(
                    m_facade,
                    [callback = std::move(callback), error] { callback(error); },
                    Qt::QueuedConnection
                );
            }
        }

        void publish(ProfileRegistry::Snapshot snapshot) {
            QMetaObject::invokeMethod(
                m_facade,
                [facade = m_facade, snapshot = std::move(snapshot)]() mutable {
                    facade->publishSnapshot(std::move(snapshot));
                },
                Qt::QueuedConnection
            );
        }

        void finishLoad(ProfileRegistry::LoadStatus status) {
            QMetaObject::invokeMethod(
                m_facade,
                [facade = m_facade, status] { emit facade->loadFinished(status); },
                Qt::QueuedConnection
            );
        }

        ProfileRegistry *m_facade;
        QString m_databasePath;
        QString m_connectionName;
        QSqlDatabase m_database;
    };

    ProfileRegistry::ProfileRegistry(const QString &databasePath, QObject *parent)
        : QObject(parent),
          m_databasePath(databasePath) {
        m_thread.setObjectName(QStringLiteral("eden-profile-registry"));
        m_worker = new ProfileRegistryWorker(this, m_databasePath);
        m_worker->moveToThread(&m_thread);
        connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
        m_thread.start();
    }

    ProfileRegistry::~ProfileRegistry() {
        if (m_thread.isRunning()) {
            QMetaObject::invokeMethod(m_worker, [] {}, Qt::BlockingQueuedConnection);
        }
        m_thread.quit();
        m_thread.wait();
        if (m_processLock) {
            m_processLock->unlock();
        }
    }

    bool ProfileRegistry::acquireProcessLock(const QString &lockPath) {
        QDir().mkpath(QFileInfo(lockPath).absolutePath());
        m_processLock = std::make_unique<QLockFile>(lockPath);
        m_processLock->setStaleLockTime(0);
        if (m_processLock->tryLock(0)) {
            ProfilePaths::restrictFile(lockPath);
            return true;
        }
        m_processLock.reset();
        return false;
    }

    bool ProfileRegistry::processLockHeld() const {
        return m_processLock && m_processLock->isLocked();
    }

    void ProfileRegistry::loadAsync() {
        dispatch([](ProfileRegistryWorker &worker) { worker.load(); });
    }

    const ProfileRegistry::Snapshot &ProfileRegistry::snapshot() const {
        return m_snapshot;
    }

    QString ProfileRegistry::applicationStateValue(const QString &key) const {
        return m_snapshot.applicationState.value(key);
    }

    std::optional<ProfileSummary> ProfileRegistry::summaryFor(const QString &profileId) const {
        for (const ProfileSummary &summary : m_snapshot.profiles) {
            if (summary.profileId == profileId) {
                return summary;
            }
        }
        return std::nullopt;
    }

    void ProfileRegistry::createProfile(const ProfileRecord &draft, VoidCallback callback) {
        dispatch([draft, callback = std::move(callback)](ProfileRegistryWorker &worker) mutable {
            worker.createProfile(draft, std::move(callback));
        });
    }

    void
    ProfileRegistry::commitProfileReady(const ProfileId &id, const QString &passwordVerifier, VoidCallback callback) {
        dispatch([id = id.toString(), passwordVerifier, callback = std::move(callback)](
                     ProfileRegistryWorker &worker
                 ) mutable { worker.commitProfileReady(id, passwordVerifier, std::move(callback)); });
    }

    void ProfileRegistry::updateDisplayName(const ProfileId &id, const QString &displayName, VoidCallback callback) {
        dispatch(
            [id = id.toString(), displayName, callback = std::move(callback)](ProfileRegistryWorker &worker) mutable {
                worker.updateField(
                    id,
                    QStringLiteral("UPDATE profiles SET display_name = ? WHERE id = ?"),
                    displayName,
                    std::move(callback)
                );
            }
        );
    }

    void ProfileRegistry::updateColorSeed(const ProfileId &id, quint32 colorSeed, VoidCallback callback) {
        dispatch(
            [id = id.toString(), colorSeed, callback = std::move(callback)](ProfileRegistryWorker &worker) mutable {
                worker.updateField(
                    id,
                    QStringLiteral("UPDATE profiles SET color_seed = ? WHERE id = ?"),
                    colorSeed,
                    std::move(callback)
                );
            }
        );
    }

    void ProfileRegistry::incrementAvatarRevision(const ProfileId &id, VoidCallback callback) {
        dispatch([id = id.toString(), callback = std::move(callback)](ProfileRegistryWorker &worker) mutable {
            worker.updateField(
                id,
                QStringLiteral("UPDATE profiles SET avatar_revision = avatar_revision + 1 WHERE id = ?"),
                QVariant(),
                std::move(callback)
            );
        });
    }

    void ProfileRegistry::updatePasswordVerifier(const ProfileId &id, const QString &verifier, VoidCallback callback) {
        dispatch([id = id.toString(), verifier, callback = std::move(callback)](ProfileRegistryWorker &worker) mutable {
            const QVariant value = verifier.isEmpty() ? QVariant(QMetaType::fromType<QString>()) : QVariant(verifier);
            worker.updateField(
                id,
                QStringLiteral("UPDATE profiles SET password_verifier = ? WHERE id = ?"),
                value,
                std::move(callback)
            );
        });
    }

    void ProfileRegistry::touchLastUsed(const ProfileId &id, qint64 timestamp, bool recordLastActive) {
        dispatch([id = id.toString(), timestamp, recordLastActive](ProfileRegistryWorker &worker) {
            worker.touchLastUsed(id, timestamp, recordLastActive);
        });
    }

    void ProfileRegistry::setLifecycle(const ProfileId &id, ProfileLifecycle lifecycle, VoidCallback callback) {
        dispatch(
            [id = id.toString(), lifecycle, callback = std::move(callback)](ProfileRegistryWorker &worker) mutable {
                worker.updateField(
                    id,
                    QStringLiteral("UPDATE profiles SET lifecycle = ? WHERE id = ?"),
                    profileLifecycleName(lifecycle),
                    std::move(callback)
                );
            }
        );
    }

    void ProfileRegistry::removeProfile(const ProfileId &id, const QString &nextLastActiveId, VoidCallback callback) {
        dispatch([id = id.toString(), nextLastActiveId, callback = std::move(callback)](
                     ProfileRegistryWorker &worker
                 ) mutable { worker.removeProfile(id, nextLastActiveId, std::move(callback)); });
    }

    void ProfileRegistry::setApplicationState(const QString &key, const QString &value, VoidCallback callback) {
        dispatch([key, value, callback = std::move(callback)](ProfileRegistryWorker &worker) mutable {
            worker.setApplicationState(key, value, std::move(callback));
        });
    }

    void ProfileRegistry::fetchPasswordVerifier(const ProfileId &id, VerifierCallback callback) {
        dispatch([id = id.toString(), callback = std::move(callback)](ProfileRegistryWorker &worker) mutable {
            worker.fetchPasswordVerifier(id, std::move(callback));
        });
    }

    void ProfileRegistry::publishSnapshot(Snapshot snapshot) {
        m_snapshot = std::move(snapshot);
        emit snapshotChanged();
    }

    void ProfileRegistry::dispatch(std::function<void(ProfileRegistryWorker &)> work) {
        QMetaObject::invokeMethod(
            m_worker,
            [worker = m_worker, work = std::move(work)] { work(*worker); },
            Qt::QueuedConnection
        );
    }

}

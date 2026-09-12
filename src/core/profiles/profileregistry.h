#pragma once

#include "core/profiles/profileerror.h"
#include "core/profiles/profiletypes.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QThread>

#include <functional>
#include <memory>

class QLockFile;

namespace eden::core {

    class ProfileRegistryWorker;

    namespace applicationstate {
        inline constexpr auto lastActiveProfileId = "lastActiveProfileId";
        inline constexpr auto startupDisposition = "startupDisposition";
        inline constexpr auto showProfileChooserOnStartup = "showProfileChooserOnStartup";
        inline constexpr auto legacyMigrationVersion = "legacyMigrationVersion";
    }

    class ProfileRegistry final : public QObject {
        Q_OBJECT

      public:
        enum class LoadStatus { Loaded, Created, Recovery };
        Q_ENUM(LoadStatus)

        struct Snapshot {
            QList<ProfileSummary> profiles;
            QHash<QString, QString> applicationState;
        };

        using VoidCallback = std::function<void(ProfileError)>;
        using VerifierCallback = std::function<void(ProfileError, QString)>;

        explicit ProfileRegistry(const QString &databasePath, QObject *parent = nullptr);
        ~ProfileRegistry() override;

        bool acquireProcessLock(const QString &lockPath);
        bool processLockHeld() const;

        void loadAsync();
        const Snapshot &snapshot() const;
        QString applicationStateValue(const QString &key) const;
        std::optional<ProfileSummary> summaryFor(const QString &profileId) const;

        void createProfile(const ProfileRecord &draft, VoidCallback callback);
        void commitProfileReady(const ProfileId &id, const QString &passwordVerifier, VoidCallback callback);
        void updateDisplayName(const ProfileId &id, const QString &displayName, VoidCallback callback);
        void updateColorSeed(const ProfileId &id, quint32 colorSeed, VoidCallback callback);
        void incrementAvatarRevision(const ProfileId &id, VoidCallback callback);
        void updatePasswordVerifier(const ProfileId &id, const QString &verifier, VoidCallback callback);
        void touchLastUsed(const ProfileId &id, qint64 timestamp, bool recordLastActive);
        void setLifecycle(const ProfileId &id, ProfileLifecycle lifecycle, VoidCallback callback);
        void removeProfile(const ProfileId &id, const QString &nextLastActiveId, VoidCallback callback);
        void setApplicationState(const QString &key, const QString &value, VoidCallback callback = {});
        void fetchPasswordVerifier(const ProfileId &id, VerifierCallback callback);

      signals:
        void loadFinished(eden::core::ProfileRegistry::LoadStatus status);
        void snapshotChanged();

      private:
        friend class ProfileRegistryWorker;

        void publishSnapshot(Snapshot snapshot);
        void dispatch(std::function<void(ProfileRegistryWorker &)> work);

        QString m_databasePath;
        QThread m_thread;
        ProfileRegistryWorker *m_worker = nullptr;
        Snapshot m_snapshot;
        std::unique_ptr<QLockFile> m_processLock;
    };

}

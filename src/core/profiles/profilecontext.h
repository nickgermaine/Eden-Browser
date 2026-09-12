#pragma once

#include "core/profiles/profileerror.h"
#include "core/profiles/profilepaths.h"
#include "core/profiles/profiletypes.h"
#include "engine/enginebackend.h"

#include <QList>
#include <QObject>
#include <QPointer>

#include <functional>
#include <memory>

class QQmlEngine;

namespace eden::engine {
    class EngineProfile;
    class EngineProfileMap;
}

namespace eden::passwords {
    class CredentialVault;
}

namespace eden::core {

    class BookmarkStore;
    class CookieHandoffCoordinator;
    class DownloadManager;
    class HistoryStore;
    class PermissionStore;
    class ProfileDatabase;
    class ProfileSettings;
    class SessionStore;
    class WindowController;

    class ProfileContext final : public QObject {
        Q_OBJECT

      public:
        static std::shared_ptr<ProfileContext>
        create(const ProfileRecord &record, const ProfilePaths &paths, QQmlEngine *qmlEngine, ProfileError *error);
        ~ProfileContext() override;

        ProfileError activate();
        bool isActivated() const;

        const ProfileId &id() const;
        QString idString() const;
        const ProfilePaths &paths() const;
        QString displayName() const;
        quint32 colorSeed() const;
        int avatarRevision() const;
        bool passwordProtected() const;
        void applySummary(const ProfileSummary &summary);

        ProfileSettings *settings() const;
        HistoryStore *history() const;
        BookmarkStore *bookmarks() const;
        DownloadManager *downloads() const;
        SessionStore *sessions() const;
        CookieHandoffCoordinator *cookieHandoff() const;
        passwords::CredentialVault *credentialVault() const;
        PermissionStore *permissions() const;
        engine::EngineProfileMap *engineProfiles() const;
        std::shared_ptr<engine::EngineProfile> engineProfile(engine::Backend backend);
        std::unique_ptr<engine::EngineProfileMap> createPrivateEngineProfileMap();

        void registerWindow(WindowController *controller);
        void unregisterWindow(WindowController *controller);
        QList<WindowController *> windows() const;

        bool signOutInProgress() const;
        void beginSignOutBarrier();
        int activeDownloadCount() const;
        void flushEngines(int deadlineMilliseconds, std::function<void()> completion);
        void shutdownStores();
        void saveSessionNow();

      signals:
        void signOutBarrierChanged();

      private:
        ProfileContext(const ProfileRecord &record, const ProfilePaths &paths, QQmlEngine *qmlEngine);

        ProfileSummary m_summary;
        ProfileId m_id;
        ProfilePaths m_paths;
        QQmlEngine *m_qmlEngine = nullptr;
        std::unique_ptr<ProfileDatabase> m_database;
        std::unique_ptr<ProfileSettings> m_settings;
        std::unique_ptr<HistoryStore> m_history;
        std::unique_ptr<BookmarkStore> m_bookmarks;
        std::unique_ptr<DownloadManager> m_downloads;
        std::unique_ptr<SessionStore> m_sessions;
        std::unique_ptr<CookieHandoffCoordinator> m_cookieHandoff;
        std::unique_ptr<passwords::CredentialVault> m_credentialVault;
        std::unique_ptr<PermissionStore> m_permissions;
        std::unique_ptr<engine::EngineProfileMap> m_engineProfiles;
        QList<QPointer<WindowController>> m_windows;
        bool m_activated = false;
        bool m_signOutInProgress = false;
    };

}

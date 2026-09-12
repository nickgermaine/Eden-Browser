#include "core/profiles/profilecontext.h"

#include "core/bookmarks/bookmarkstore.h"
#include "core/downloads/downloadmanager.h"
#include "core/history/historystore.h"
#include "core/permissions/permissionstore.h"
#include "core/profiles/cookiehandoffcoordinator.h"
#include "core/profiles/profiledatabase.h"
#include "core/profiles/profilesettings.h"
#include "core/profiles/sessionstore.h"
#include "core/profiles/windowregistry.h"
#include "core/window/windowcontroller.h"
#include "engine/enginefactory.h"
#include "engine/engineprofile.h"
#include "engine/engineprofilemap.h"
#include "engine/engineregistry.h"
#include "passwords/credentialvault.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTimer>

#include <sodium.h>

namespace eden::core {

    ProfileContext::ProfileContext(const ProfileRecord &record, const ProfilePaths &paths, QQmlEngine *qmlEngine)
        : m_summary(summaryFromRecord(record)),
          m_id(record.id),
          m_paths(paths),
          m_qmlEngine(qmlEngine),
          m_database(std::make_unique<ProfileDatabase>(paths.databasePath())),
          m_settings(std::make_unique<ProfileSettings>(paths.settingsPath())),
          m_history(std::make_unique<HistoryStore>(paths.databasePath())),
          m_bookmarks(std::make_unique<BookmarkStore>(paths.databasePath())),
          m_downloads(std::make_unique<DownloadManager>(paths.databasePath())),
          m_sessions(std::make_unique<SessionStore>(paths.sessionPath())),
          m_cookieHandoff(std::make_unique<CookieHandoffCoordinator>(record.id.toString())) {
        m_credentialVault = std::make_unique<passwords::CredentialVault>(paths.vaultPath(), record.id.toString(), this);
        m_permissions = std::make_unique<PermissionStore>(paths.databasePath(), this);
        m_sessions->setVault(m_credentialVault.get());
        m_settings->setVault(m_credentialVault.get());
        m_engineProfiles = std::make_unique<engine::EngineProfileMap>(false, [this](engine::Backend backend) {
            const QString backendId = engine::EngineRegistry::instance()->idForBackend(backend);
            if (backendId.isEmpty() || !m_paths.ensureEngineDirectories(backendId)) {
                return std::shared_ptr<engine::EngineProfile>();
            }
            engine::EngineProfileParameters parameters;
            parameters.profileId = m_id.toString();
            parameters.backend = backend;
            parameters.privateProfile = false;
            parameters.dataPath = m_paths.engineDataDirectory(backendId);
            parameters.cachePath = m_paths.engineCacheDirectory(backendId);
            std::shared_ptr<engine::EngineProfile> profile = engine::EngineFactory::create(parameters, m_qmlEngine);
            if (profile) {
                connect(
                    profile.get(),
                    &engine::EngineProfile::downloadStarted,
                    m_downloads.get(),
                    &DownloadManager::beginDownload,
                    Qt::UniqueConnection
                );
                connect(
                    profile.get(),
                    &engine::EngineProfile::downloadUpdated,
                    m_downloads.get(),
                    &DownloadManager::updateDownload,
                    Qt::UniqueConnection
                );
            }
            return profile;
        });
        connect(m_sessions.get(), &SessionStore::saveRequested, this, &ProfileContext::saveSessionNow);
    }

    std::shared_ptr<ProfileContext> ProfileContext::create(
        const ProfileRecord &record,
        const ProfilePaths &paths,
        QQmlEngine *qmlEngine,
        ProfileError *error
    ) {
        const auto fail = [error](ProfileError code) {
            if (error) {
                *error = code;
            }
            return std::shared_ptr<ProfileContext>();
        };
        if (!record.id.isValid() || !paths.isValid() || paths.profileId() != record.id) {
            return fail(ProfileError::InvalidProfileId);
        }
        if (!paths.ensureBaseDirectories()) {
            return fail(ProfileError::DirectoryCreateFailed);
        }
        if (!paths.verifyRestrictedPermissions()) {
            return fail(ProfileError::PermissionRestricted);
        }
        if (error) {
            *error = ProfileError::None;
        }
        return std::shared_ptr<ProfileContext>(new ProfileContext(record, paths, qmlEngine));
    }

    ProfileContext::~ProfileContext() {
        Q_ASSERT(m_windows.isEmpty());
        Q_ASSERT(!m_cookieHandoff || !m_cookieHandoff->active());
    }

    ProfileError ProfileContext::activate() {
        if (m_activated) {
            return ProfileError::None;
        }
        if (!m_credentialVault->initialize()) {
            return ProfileError::VaultOpenFailed;
        }
        QByteArray databaseKey = m_credentialVault->derivedKey(QByteArrayLiteral("profile-database"));
        const auto wipeKey =
            qScopeGuard([&] { sodium_memzero(databaseKey.data(), static_cast<size_t>(databaseKey.size())); });
        if (!m_database->initialize(databaseKey)) {
            return ProfileError::DatabaseOpenFailed;
        }
        if (!m_settings->initialize()) {
            return m_settings->error();
        }
        m_history->initialize(databaseKey);
        m_bookmarks->initialize(databaseKey);
        const bool permissionsReady = m_permissions->initialize(databaseKey);
        const bool downloadsReady = m_downloads->initialize(databaseKey);
        if (!m_history->isInitialized() || !m_bookmarks->isInitialized() || !permissionsReady || !downloadsReady) {
            return ProfileError::DatabaseOpenFailed;
        }
        m_activated = true;
        return ProfileError::None;
    }

    bool ProfileContext::isActivated() const {
        return m_activated;
    }

    const ProfileId &ProfileContext::id() const {
        return m_id;
    }

    QString ProfileContext::idString() const {
        return m_id.toString();
    }

    const ProfilePaths &ProfileContext::paths() const {
        return m_paths;
    }

    QString ProfileContext::displayName() const {
        return m_summary.displayName;
    }

    quint32 ProfileContext::colorSeed() const {
        return m_summary.colorSeed;
    }

    int ProfileContext::avatarRevision() const {
        return m_summary.avatarRevision;
    }

    bool ProfileContext::passwordProtected() const {
        return m_summary.protectedProfile;
    }

    void ProfileContext::applySummary(const ProfileSummary &summary) {
        if (summary.profileId != m_summary.profileId) {
            return;
        }
        const bool changed = summary.displayName != m_summary.displayName || summary.colorSeed != m_summary.colorSeed ||
                             summary.avatarRevision != m_summary.avatarRevision ||
                             summary.protectedProfile != m_summary.protectedProfile;
        m_summary = summary;
        if (changed) {
            for (WindowController *controller : windows()) {
                controller->notifyProfileIdentityChanged();
            }
        }
    }

    ProfileSettings *ProfileContext::settings() const {
        return m_settings.get();
    }

    HistoryStore *ProfileContext::history() const {
        return m_history.get();
    }

    BookmarkStore *ProfileContext::bookmarks() const {
        return m_bookmarks.get();
    }

    DownloadManager *ProfileContext::downloads() const {
        return m_downloads.get();
    }

    SessionStore *ProfileContext::sessions() const {
        return m_sessions.get();
    }

    CookieHandoffCoordinator *ProfileContext::cookieHandoff() const {
        return m_cookieHandoff.get();
    }

    passwords::CredentialVault *ProfileContext::credentialVault() const {
        return m_credentialVault.get();
    }

    PermissionStore *ProfileContext::permissions() const {
        return m_permissions.get();
    }

    engine::EngineProfileMap *ProfileContext::engineProfiles() const {
        return m_engineProfiles.get();
    }

    std::shared_ptr<engine::EngineProfile> ProfileContext::engineProfile(engine::Backend backend) {
        return m_engineProfiles->profile(backend);
    }

    std::unique_ptr<engine::EngineProfileMap> ProfileContext::createPrivateEngineProfileMap() {
        return std::make_unique<engine::EngineProfileMap>(true, [this](engine::Backend backend) {
            engine::EngineProfileParameters parameters;
            parameters.profileId = m_id.toString();
            parameters.backend = backend;
            parameters.privateProfile = true;
            return engine::EngineFactory::create(parameters, m_qmlEngine);
        });
    }

    void ProfileContext::registerWindow(WindowController *controller) {
        if (controller && !m_windows.contains(controller)) {
            m_windows.append(QPointer<WindowController>(controller));
        }
    }

    void ProfileContext::unregisterWindow(WindowController *controller) {
        m_windows.removeAll(QPointer<WindowController>(controller));
        m_windows.removeAll(QPointer<WindowController>());
    }

    QList<WindowController *> ProfileContext::windows() const {
        QList<WindowController *> alive;
        alive.reserve(m_windows.size());
        for (const QPointer<WindowController> &controller : m_windows) {
            if (controller) {
                alive.append(controller.data());
            }
        }
        return alive;
    }

    bool ProfileContext::signOutInProgress() const {
        return m_signOutInProgress;
    }

    void ProfileContext::beginSignOutBarrier() {
        if (m_signOutInProgress) {
            return;
        }
        m_signOutInProgress = true;
        m_sessions->cancelPendingSave();
        m_cookieHandoff->setBlocked(true);
        emit signOutBarrierChanged();
    }

    int ProfileContext::activeDownloadCount() const {
        int count = m_downloads ? m_downloads->activeCount() : 0;
        for (const QPointer<WindowController> &controller : m_windows) {
            if (controller && controller->downloads() && controller->downloads() != m_downloads.get()) {
                count += controller->downloads()->activeCount();
            }
        }
        return count;
    }

    void ProfileContext::flushEngines(int deadlineMilliseconds, std::function<void()> completion) {
        auto flush = [this, deadlineMilliseconds, completion = std::move(completion)]() mutable {
            const QList<std::shared_ptr<engine::EngineProfile>> profiles = m_engineProfiles->liveProfiles();
            if (profiles.isEmpty()) {
                if (completion) {
                    completion();
                }
                return;
            }
            struct FlushState {
                qsizetype remaining = 0;
                bool finished = false;
                std::function<void()> completion;
            };
            auto state = std::make_shared<FlushState>();
            state->remaining = profiles.size();
            state->completion = std::move(completion);
            const auto finish = [state] {
                if (state->finished) {
                    return;
                }
                state->finished = true;
                if (state->completion) {
                    state->completion();
                }
            };
            QTimer::singleShot(deadlineMilliseconds, this, finish);
            for (const std::shared_ptr<engine::EngineProfile> &profile : profiles) {
                profile->flushStorage([state, finish] {
                    if (--state->remaining <= 0) {
                        finish();
                    }
                });
            }
        };
        if (m_history) {
            m_history->close(std::move(flush));
        } else {
            flush();
        }
    }

    void ProfileContext::shutdownStores() {
        m_engineProfiles->clear();
        m_history.reset();
        m_bookmarks.reset();
        m_permissions.reset();
        m_downloads.reset();
    }

    void ProfileContext::saveSessionNow() {
        if (!m_sessions) {
            return;
        }
        QJsonArray windows;
        for (const QPointer<WindowController> &controller : m_windows) {
            if (controller && !controller->isPrivateWindow() && controller->isInitialized()) {
                windows.append(controller->sessionWindow());
            }
        }
        if (windows.isEmpty()) {
            return;
        }
        QJsonObject session;
        session.insert(QStringLiteral("version"), 4);
        session.insert(QStringLiteral("windows"), windows);
        m_sessions->write(session);
    }

}

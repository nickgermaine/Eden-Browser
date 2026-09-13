#include "core/profiles/profilemanager.h"

#include "core/profiles/avatarimageprovider.h"
#include "core/profiles/avatarprocessor.h"
#include "core/profiles/enginestorage.h"
#include "core/profiles/passwordhasher.h"
#include "core/profiles/profilecontext.h"
#include "core/profiles/profileeditorcontroller.h"
#include "core/profiles/profilelistmodel.h"
#include "core/profiles/profilemigration.h"
#include "core/profiles/profilevalidation.h"
#include "core/profiles/sessionstore.h"
#include "core/profiles/windowregistry.h"
#include "core/settings/settingsstore.h"
#include "core/window/tabmodel.h"
#include "core/window/windowcontroller.h"
#include "engine/enginefactory.h"
#include "engine/engineregistry.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocale>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QScopedValueRollback>
#include <QSet>
#include <QThreadPool>

#include <algorithm>

namespace eden::core {

    static ProfileManager *&managerInstance() {
        static ProfileManager *instance = nullptr;
        return instance;
    }

    static QQuickWindow *originatingWindow(QObject *originatingController, WindowRegistry *windows) {
        WindowController *controller = qobject_cast<WindowController *>(originatingController);
        return controller && windows ? windows->windowFor(controller) : nullptr;
    }

    static void centerTransientWindow(QQuickWindow *window, QQuickWindow *parentWindow) {
        if (!window || !parentWindow) {
            return;
        }
        QWindow *previousParent = window->transientParent();
        if (previousParent && previousParent != parentWindow) {
            QObject::disconnect(previousParent, nullptr, window, nullptr);
        }
        const bool parentChanged = previousParent != parentWindow;
        window->setTransientParent(parentWindow);
        window->setModality(Qt::WindowModal);
        const auto center = [window = QPointer<QQuickWindow>(window),
                             parentWindow = QPointer<QQuickWindow>(parentWindow)] {
            if (!window || !parentWindow) {
                return;
            }
            window->setPosition(
                parentWindow->x() + std::max(0, (parentWindow->width() - window->width()) / 2),
                parentWindow->y() + std::max(0, (parentWindow->height() - window->height()) / 2)
            );
        };
        center();
        if (!parentChanged) {
            return;
        }
        QObject::connect(parentWindow, &QQuickWindow::xChanged, window, center);
        QObject::connect(parentWindow, &QQuickWindow::yChanged, window, center);
        QObject::connect(parentWindow, &QQuickWindow::widthChanged, window, center);
        QObject::connect(parentWindow, &QQuickWindow::heightChanged, window, center);
        QObject::connect(window, &QQuickWindow::widthChanged, window, center);
        QObject::connect(window, &QQuickWindow::heightChanged, window, center);
    }

    static QJsonObject migrateLegacyStartupTab(const QJsonObject &window, int sessionVersion) {
        if (sessionVersion >= 4) {
            return window;
        }
        QJsonArray tabs = window.value(QLatin1String("tabs")).toArray();
        if (tabs.size() <= 1) {
            return window;
        }
        qsizetype firstUnpinned = 0;
        while (firstUnpinned < tabs.size() && tabs.at(firstUnpinned).toObject().value("pinned").toBool()) {
            ++firstUnpinned;
        }
        if (firstUnpinned >= tabs.size()) {
            return window;
        }
        const QJsonObject candidate = tabs.at(firstUnpinned).toObject();
        if (candidate.value(QLatin1String("url")).toString() != QLatin1String("about:blank")) {
            return window;
        }
        tabs.removeAt(firstUnpinned);
        QJsonObject migrated = window;
        migrated.insert(QLatin1String("tabs"), tabs);
        const int activeIndex = window.value(QLatin1String("activeIndex")).toInt(0);
        migrated.insert(QLatin1String("activeIndex"), activeIndex > firstUnpinned ? activeIndex - 1 : activeIndex);
        return migrated;
    }

    ProfileManager::ProfileManager(
        ProfileRegistry *registry,
        WindowRegistry *windows,
        const ProfilePaths::Roots &roots,
        QObject *parent
    )
        : QObject(parent),
          m_registry(registry),
          m_windows(windows),
          m_roots(roots),
          m_hasher(std::make_unique<PasswordHasher>()),
          m_model(std::make_unique<ProfileListModel>()) {
        managerInstance() = this;
        connect(EngineStorage::instance(), &EngineStorage::storageFailed, this, [this](const QString &message) {
            enterRecovery(message);
        });
        m_cooldownTimer.setSingleShot(true);
        connect(&m_cooldownTimer, &QTimer::timeout, this, [this] {
            const QString profileId = m_cooldownProfileId;
            m_cooldownProfileId.clear();
            if (!profileId.isEmpty()) {
                emit cooldownFinished(profileId);
            }
        });
        connect(m_registry, &ProfileRegistry::loadFinished, this, &ProfileManager::handleRegistryLoaded);
        connect(m_registry, &ProfileRegistry::snapshotChanged, this, &ProfileManager::handleSnapshotChanged);
        connect(m_windows, &WindowRegistry::windowsChanged, this, &ProfileManager::rebuildModel);
        connect(m_windows, &WindowRegistry::windowsChanged, this, &ProfileManager::browserWindowCountChanged);
        connect(
            this,
            &ProfileManager::startupStateChanged,
            this,
            &ProfileManager::processWindowRequests,
            Qt::QueuedConnection
        );
    }

    ProfileManager::~ProfileManager() {
        if (managerInstance() == this) {
            managerInstance() = nullptr;
        }
    }

    ProfileManager *ProfileManager::instance() {
        return managerInstance();
    }

    void ProfileManager::setQmlEngine(QQmlEngine *engine) {
        m_qmlEngine = engine;
        if (!engine) {
            return;
        }
        m_avatarProvider = new AvatarImageProvider;
        engine->addImageProvider(QLatin1String(AvatarImageProvider::providerName), m_avatarProvider);
        syncAvatarProvider();
    }

    void ProfileManager::setLaunchWindow(QQuickWindow *window) {
        m_launchWindow = window;
    }

    void ProfileManager::configureLaunch(bool privateWindow, const QString &engineName, const QList<QUrl> &urls) {
        m_launchPrivate = privateWindow;
        m_launchEngineName = engineName;
        for (const QUrl &url : urls) {
            m_launchUrls.enqueue(url);
        }
    }

    bool ProfileManager::requestWindow(const WindowLaunchRequest &request) {
        if (m_windowRequests.size() >= 64 || QCoreApplication::closingDown()) {
            return false;
        }
        m_windowRequests.enqueue(request);
        QMetaObject::invokeMethod(this, &ProfileManager::processWindowRequests, Qt::QueuedConnection);
        return true;
    }

    void ProfileManager::processWindowRequests() {
        if (m_windowRequests.isEmpty() || m_processingWindowRequests) {
            return;
        }
        const QScopedValueRollback processing(m_processingWindowRequests, true);
        if (m_startupState == QLatin1String("loading") || m_startupState == QLatin1String("recovery")) {
            if (m_launchWindow) {
                m_launchWindow->raise();
                m_launchWindow->requestActivate();
            }
            return;
        }
        const QString profileId = m_windows->mostRecentActiveProfileId(true);
        const auto context = m_contexts.value(profileId);
        if (!context || !context->isActivated() || context->signOutInProgress()) {
            showChooser();
            return;
        }
        const WindowLaunchRequest request = m_windowRequests.head();
        WindowController *controller =
            createBrowserWindow(context, request.privateWindow, request.engineName, false, request.urls.isEmpty());
        if (!controller) {
            return;
        }
        m_windowRequests.dequeue();
        for (const QUrl &url : request.urls) {
            controller->newTab(url);
        }
        if (QQuickWindow *window = m_windows->windowFor(controller)) {
            window->raise();
            window->requestActivate();
        }
        if (request.urls.isEmpty()) {
            emit controller->focusOmniboxRequested();
        }
        if (!m_windowRequests.isEmpty()) {
            QMetaObject::invokeMethod(this, &ProfileManager::processWindowRequests, Qt::QueuedConnection);
        }
    }

    void ProfileManager::beginStartup() {
        if (m_preparingStorage) {
            return;
        }
        m_preparingStorage = true;
        m_storageRecovery = false;
        m_startupState = QStringLiteral("loading");
        emit startupStateChanged();
        QPointer<ProfileManager> guard(this);
        EngineStorage::instance()->prepare(
            m_roots,
            [guard](const QString &error) {
                if (!guard) {
                    return;
                }
                guard->m_preparingStorage = false;
                if (!error.isEmpty()) {
                    guard->m_storageRecovery = true;
                    guard->enterRecovery(error);
                    return;
                }
                guard->m_startupMessage = QStringLiteral("Opening your profile");
                guard->m_startupDetails.clear();
                emit guard->startupProgressChanged();
                guard->m_registry->loadAsync();
            },
            [guard](const QString &message, qint64 processedBytes) {
                if (!guard || !guard->m_preparingStorage) {
                    return;
                }
                guard->m_startupMessage = message;
                guard->m_startupDetails = processedBytes > 0
                                              ? QStringLiteral("Processed %1. First-time setup can take a few minutes.")
                                                    .arg(QLocale().formattedDataSize(processedBytes))
                                              : QStringLiteral("Existing site data is checked before cleanup.");
                emit guard->startupProgressChanged();
            }
        );
    }

    ProfileListModel *ProfileManager::profileModel() const {
        return m_model.get();
    }

    QString ProfileManager::startupState() const {
        return m_startupState;
    }

    QString ProfileManager::startupMessage() const {
        return m_startupMessage;
    }

    QString ProfileManager::startupDetails() const {
        return m_startupDetails;
    }

    QString ProfileManager::recoveryMessage() const {
        return m_recoveryMessage;
    }

    bool ProfileManager::recoveryCanRetry() const {
        return m_storageRecovery || !m_recoveryProfileId.isEmpty();
    }

    void ProfileManager::retryProfileOpen() {
        if (m_storageRecovery && m_startupState == QLatin1String("recovery")) {
            beginStartup();
            return;
        }
        if (m_startupState != QLatin1String("recovery") || m_recoveryProfileId.isEmpty()) {
            return;
        }
        const QString profileId = m_recoveryProfileId;
        const std::optional<ProfileSummary> summary = m_registry->summaryFor(profileId);
        const ProfileSessionState state = sessionStateFor(profileId);
        if (!summary || summary->lifecycle != ProfileLifecycle::Ready ||
            (state != ProfileSessionState::Locked && state != ProfileSessionState::Unlocked)) {
            return;
        }
        if (summary->protectedProfile && state != ProfileSessionState::Unlocked) {
            showChooser(profileId);
            return;
        }
        openProfile(profileId, m_recoveryBecauseOfSwitch);
    }

    bool ProfileManager::showChooserOnStartup() const {
        return m_registry->applicationStateValue(QLatin1String(applicationstate::showProfileChooserOnStartup)) ==
               QLatin1String("true");
    }

    void ProfileManager::setShowChooserOnStartup(bool show) {
        m_registry->setApplicationState(
            QLatin1String(applicationstate::showProfileChooserOnStartup),
            show ? QStringLiteral("true") : QStringLiteral("false")
        );
        emit showChooserOnStartupChanged();
    }

    int ProfileManager::pendingCleanupCount() const {
        return m_pendingCleanupCount;
    }

    int ProfileManager::browserWindowCount() const {
        return static_cast<int>(m_windows->allControllers().size());
    }

    std::shared_ptr<ProfileContext> ProfileManager::contextFor(const QString &profileId) const {
        return m_contexts.value(profileId);
    }

    ProfileSessionState ProfileManager::sessionStateFor(const QString &profileId) const {
        return m_states.value(profileId, ProfileSessionState::Locked);
    }

    ProfileRegistry *ProfileManager::registry() const {
        return m_registry;
    }

    WindowRegistry *ProfileManager::windowRegistry() const {
        return m_windows;
    }

    const ProfilePaths::Roots &ProfileManager::roots() const {
        return m_roots;
    }

    QStringList ProfileManager::knownBackendIds() const {
        return {
            QStringLiteral("cef"),
            QStringLiteral("qtwebengine"),
            QStringLiteral("webkit"),
            QStringLiteral("servo")
        };
    }

    void ProfileManager::handleRegistryLoaded(ProfileRegistry::LoadStatus status) {
        if (status == ProfileRegistry::LoadStatus::Recovery) {
            enterRecovery(profileErrorMessage(ProfileError::RegistryCorrupt));
            return;
        }
        cleanupStagedDirectories();
        if (status == ProfileRegistry::LoadStatus::Created) {
            if (ProfileMigration::profileDirectoriesPresent(m_roots) && !ProfileMigration::journalExists(m_roots)) {
                enterRecovery(profileErrorMessage(ProfileError::RegistryCorrupt));
                return;
            }
            bootstrapDefaultProfile();
            return;
        }
        if (m_registry->snapshot().profiles.isEmpty()) {
            if (ProfileMigration::profileDirectoriesPresent(m_roots)) {
                enterRecovery(profileErrorMessage(ProfileError::RegistryCorrupt));
                return;
            }
            bootstrapDefaultProfile();
            return;
        }
        resumeInterruptedLifecycles();
        applyStartupDecision();
    }

    void ProfileManager::handleSnapshotChanged() {
        for (const ProfileSummary &summary : m_registry->snapshot().profiles) {
            if (const std::shared_ptr<ProfileContext> context = m_contexts.value(summary.profileId)) {
                context->applySummary(summary);
            }
        }
        syncAvatarProvider();
        rebuildModel();
    }

    void ProfileManager::rebuildModel() {
        const ProfileRegistry::Snapshot &snapshot = m_registry->snapshot();
        int readyCount = 0;
        for (const ProfileSummary &summary : snapshot.profiles) {
            if (summary.lifecycle == ProfileLifecycle::Ready) {
                ++readyCount;
            }
        }
        const QString currentProfileId = m_windows->mostRecentActiveProfileId();
        QList<ProfileListModel::Row> rows;
        rows.reserve(snapshot.profiles.size());
        for (const ProfileSummary &summary : snapshot.profiles) {
            if (summary.lifecycle == ProfileLifecycle::Deleting) {
                continue;
            }
            ProfileListModel::Row row;
            row.summary = summary;
            row.sessionState = profileSessionStateName(sessionStateFor(summary.profileId));
            row.currentProfile = summary.profileId == currentProfileId;
            row.canDelete = summary.lifecycle == ProfileLifecycle::Ready && readyCount > 1;
            rows.append(row);
        }
        std::sort(rows.begin(), rows.end(), [](const ProfileListModel::Row &left, const ProfileListModel::Row &right) {
            if (left.summary.lastUsedAt != right.summary.lastUsedAt) {
                return left.summary.lastUsedAt > right.summary.lastUsedAt;
            }
            if (left.summary.createdAt != right.summary.createdAt) {
                return left.summary.createdAt < right.summary.createdAt;
            }
            return left.summary.profileId < right.summary.profileId;
        });
        m_model->update(std::move(rows));
        emit profileMenuActionsChanged();
    }

    void ProfileManager::syncAvatarProvider() {
        if (!m_avatarProvider) {
            return;
        }
        for (const ProfileSummary &summary : m_registry->snapshot().profiles) {
            const ProfileId identifier = ProfileId::parse(summary.profileId).value_or(ProfileId());
            if (!identifier.isValid()) {
                continue;
            }
            const ProfilePaths paths(m_roots, identifier);
            const QString avatarPath = summary.avatarRevision > 0 ? paths.avatarPath() : QString();
            m_avatarProvider->updateProfile(summary.profileId, summary.displayName, summary.colorSeed, avatarPath);
        }
    }

    void ProfileManager::resumeInterruptedLifecycles() {
        const QList<ProfileSummary> profiles = m_registry->snapshot().profiles;
        for (const ProfileSummary &summary : profiles) {
            const std::optional<ProfileId> identifier = ProfileId::parse(summary.profileId);
            if (!identifier) {
                continue;
            }
            if (summary.lifecycle == ProfileLifecycle::Creating) {
                const ProfilePaths paths(m_roots, *identifier);
                const QDir dataDirectory(paths.dataDirectory());
                if (!dataDirectory.exists() || dataDirectory.isEmpty()) {
                    m_registry->removeProfile(*identifier, nextLastActiveAfterRemoval(summary.profileId), {});
                }
            } else if (summary.lifecycle == ProfileLifecycle::Deleting) {
                stageAndRemoveProfile(summary.profileId);
            }
        }
    }

    void ProfileManager::applyStartupDecision() {
        if (m_startupDecisionApplied) {
            return;
        }
        m_startupDecisionApplied = true;
        const ProfileRegistry::Snapshot &snapshot = m_registry->snapshot();
        QList<ProfileSummary> ready;
        for (const ProfileSummary &summary : snapshot.profiles) {
            if (summary.lifecycle == ProfileLifecycle::Ready) {
                ready.append(summary);
            }
        }
        if (ready.isEmpty()) {
            showChooser();
            return;
        }
        const QString disposition = snapshot.applicationState.value(
            QLatin1String(applicationstate::startupDisposition),
            QStringLiteral("ResumeLast")
        );
        const bool chooserPreference =
            snapshot.applicationState.value(QLatin1String(applicationstate::showProfileChooserOnStartup)) ==
            QLatin1String("true");
        const QString lastActiveId =
            snapshot.applicationState.value(QLatin1String(applicationstate::lastActiveProfileId));
        const ProfileSummary *lastActive = nullptr;
        for (const ProfileSummary &summary : ready) {
            if (summary.profileId == lastActiveId) {
                lastActive = &summary;
            }
        }
        if (ready.size() == 1 && !ready.constFirst().protectedProfile && disposition == QLatin1String("ResumeLast")) {
            openProfile(ready.constFirst().profileId, false);
            return;
        }
        if (lastActive && lastActive->protectedProfile) {
            showChooser();
            return;
        }
        if (disposition == QLatin1String("ChooseProfile") || chooserPreference) {
            showChooser();
            return;
        }
        if (lastActive && !lastActive->protectedProfile) {
            openProfile(lastActive->profileId, false);
            return;
        }
        showChooser();
    }

    void ProfileManager::bootstrapDefaultProfile() {
        const ProfileMigration::Inventory inventory = ProfileMigration::inventoryLegacyData(m_roots);
        const ProfileId identifier = ProfileId::generate();
        ProfileRecord draft;
        draft.id = identifier;
        draft.displayName = QStringLiteral("Default");
        draft.colorSeed = QRandomGenerator::system()->generate();
        draft.createdAt = QDateTime::currentMSecsSinceEpoch();
        draft.lastUsedAt = draft.createdAt;
        QPointer<ProfileManager> guard(this);
        m_registry->createProfile(draft, [guard, identifier, inventory](ProfileError error) {
            if (!guard) {
                return;
            }
            if (error != ProfileError::None) {
                guard->enterRecovery(profileErrorMessage(error));
                return;
            }
            const ProfilePaths paths(guard->m_roots, identifier);
            if (!inventory.any()) {
                if (!paths.ensureBaseDirectories()) {
                    guard->enterRecovery(profileErrorMessage(ProfileError::DirectoryCreateFailed));
                    return;
                }
                guard->m_registry->commitProfileReady(identifier, {}, [guard, identifier](ProfileError commitError) {
                    if (!guard) {
                        return;
                    }
                    if (commitError != ProfileError::None) {
                        guard->enterRecovery(profileErrorMessage(commitError));
                        return;
                    }
                    guard->m_startupDecisionApplied = true;
                    guard->openProfile(identifier.toString(), false);
                });
                return;
            }
            QThreadPool::globalInstance()->start([guard, identifier, inventory] {
                const ProfilePaths paths(guard ? guard->m_roots : ProfilePaths::Roots{}, identifier);
                const ProfileError migrationError =
                    guard ? ProfileMigration::migrate(inventory, paths) : ProfileError::Cancelled;
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(),
                    [guard, identifier, migrationError] {
                        if (!guard) {
                            return;
                        }
                        if (migrationError != ProfileError::None) {
                            guard->enterRecovery(profileErrorMessage(ProfileError::MigrationFailed));
                            return;
                        }
                        guard->m_registry
                            ->commitProfileReady(identifier, {}, [guard, identifier](ProfileError commitError) {
                                if (!guard) {
                                    return;
                                }
                                if (commitError != ProfileError::None) {
                                    guard->enterRecovery(profileErrorMessage(commitError));
                                    return;
                                }
                                guard->m_registry->setApplicationState(
                                    QLatin1String(applicationstate::legacyMigrationVersion),
                                    QStringLiteral("1")
                                );
                                QFile::remove(ProfileMigration::journalPath(guard->m_roots));
                                guard->m_startupDecisionApplied = true;
                                guard->openProfile(identifier.toString(), false);
                            });
                    },
                    Qt::QueuedConnection
                );
            });
        });
    }

    void ProfileManager::enterRecovery(const QString &message, const QString &profileId) {
        m_recoveryMessage = message;
        m_recoveryProfileId = profileId;
        m_startupState = QStringLiteral("recovery");
        emit startupStateChanged();
        if (!m_launchWindow && m_qmlEngine) {
            QQmlComponent component(m_qmlEngine);
            component.loadFromModule("Eden.Ui", "LaunchWindow");
            if (component.isError()) {
                qWarning().noquote() << component.errorString();
                return;
            }
            QObject *created = component.createWithInitialProperties({{QStringLiteral("visible"), false}});
            m_launchWindow = qobject_cast<QQuickWindow *>(created);
            if (!m_launchWindow) {
                delete created;
                return;
            }
            QQmlEngine::setObjectOwnership(m_launchWindow, QQmlEngine::CppOwnership);
            QQuickWindow *window = m_launchWindow;
            connect(window, &QQuickWindow::closing, window, [window] { window->deleteLater(); });
            if (WindowController *active =
                    m_windows->mostRecentProfileController(m_windows->mostRecentActiveProfileId(), false)) {
                centerTransientWindow(window, m_windows->windowFor(active));
            }
        }
        if (m_launchWindow) {
            m_launchWindow->show();
            m_launchWindow->raise();
            m_launchWindow->requestActivate();
            closeChooserWindow();
        }
    }

    void ProfileManager::activateProfile(const QString &profileId) {
        const std::optional<ProfileSummary> summary = m_registry->summaryFor(profileId);
        if (!summary) {
            return;
        }
        if (summary->lifecycle == ProfileLifecycle::Creating) {
            return;
        }
        const ProfileSessionState state = sessionStateFor(profileId);
        if (state == ProfileSessionState::Unlocked) {
            openProfile(profileId, true);
            return;
        }
        if (state != ProfileSessionState::Locked) {
            return;
        }
        if (summary->protectedProfile) {
            emit passwordRequired(profileId);
            return;
        }
        openProfile(profileId, true);
    }

    void ProfileManager::switchToProfile(const QString &profileId) {
        const ProfileSessionState state = sessionStateFor(profileId);
        if (state == ProfileSessionState::Unlocked) {
            openProfile(profileId, true);
            return;
        }
        const std::optional<ProfileSummary> summary = m_registry->summaryFor(profileId);
        if (!summary || summary->lifecycle != ProfileLifecycle::Ready) {
            return;
        }
        if (summary->protectedProfile) {
            showChooser(profileId);
            return;
        }
        openProfile(profileId, true);
    }

    void ProfileManager::submitPassword(const QString &profileId, const QString &password) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const int remaining = remainingCooldownSeconds(profileId, now);
        if (remaining > 0) {
            emit unlockFailed(profileId, profileErrorMessage(ProfileError::WrongPassword), remaining);
            return;
        }
        if (sessionStateFor(profileId) != ProfileSessionState::Locked) {
            return;
        }
        const std::optional<ProfileSummary> summary = m_registry->summaryFor(profileId);
        if (!summary || !summary->protectedProfile) {
            return;
        }
        m_states.insert(profileId, ProfileSessionState::Unlocking);
        rebuildModel();
        QPointer<ProfileManager> guard(this);
        const std::optional<ProfileId> identifier = ProfileId::parse(profileId);
        if (!identifier) {
            m_states.insert(profileId, ProfileSessionState::Locked);
            return;
        }
        m_registry->fetchPasswordVerifier(
            *identifier,
            [guard, profileId, password](ProfileError error, QString verifier) {
                if (!guard) {
                    return;
                }
                if (error != ProfileError::None || verifier.isEmpty()) {
                    guard->m_states.insert(profileId, ProfileSessionState::Locked);
                    guard->rebuildModel();
                    emit guard->unlockFailed(profileId, profileErrorMessage(ProfileError::VerifierFailed), 0);
                    return;
                }
                guard->m_hasher
                    ->verifyPassword(verifier, password, [guard, profileId, verifier](bool matches, bool needsRehash) {
                        if (!guard) {
                            return;
                        }
                        if (!matches) {
                            guard->m_states.insert(profileId, ProfileSessionState::Locked);
                            guard->applyRetryFailure(profileId);
                            guard->rebuildModel();
                            return;
                        }
                        guard->m_retryStates.remove(profileId);
                        guard->finishUnlock(profileId);
                        if (needsRehash) {
                            emit guard->unlockSucceeded(profileId);
                        }
                    });
            }
        );
    }

    void ProfileManager::applyRetryFailure(const QString &profileId) {
        RetryState &retry = m_retryStates[profileId];
        ++retry.failures;
        int delaySeconds = 0;
        if (retry.failures >= 5) {
            delaySeconds = std::min(30, 1 << (retry.failures - 5));
        }
        if (delaySeconds > 0) {
            retry.cooldownUntilMilliseconds = QDateTime::currentMSecsSinceEpoch() + delaySeconds * 1000;
            m_cooldownProfileId = profileId;
            m_cooldownTimer.start(delaySeconds * 1000);
        }
        emit unlockFailed(profileId, profileErrorMessage(ProfileError::WrongPassword), delaySeconds);
    }

    int ProfileManager::remainingCooldownSeconds(const QString &profileId, qint64 nowMilliseconds) const {
        const auto found = m_retryStates.constFind(profileId);
        if (found == m_retryStates.constEnd() || found->cooldownUntilMilliseconds <= nowMilliseconds) {
            return 0;
        }
        return static_cast<int>((found->cooldownUntilMilliseconds - nowMilliseconds + 999) / 1000);
    }

    int ProfileManager::passwordCooldownSeconds(const QString &profileId) const {
        return remainingCooldownSeconds(profileId, QDateTime::currentMSecsSinceEpoch());
    }

    void ProfileManager::finishUnlock(const QString &profileId) {
        m_states.insert(profileId, ProfileSessionState::Unlocked);
        emit unlockSucceeded(profileId);
        openProfile(profileId, true);
    }

    void ProfileManager::openProfile(const QString &profileId, bool becauseOfSwitch) {
        const std::optional<ProfileSummary> summary = m_registry->summaryFor(profileId);
        const std::optional<ProfileId> identifier = ProfileId::parse(profileId);
        if (!summary || !identifier) {
            return;
        }
        m_recoveryBecauseOfSwitch = becauseOfSwitch;
        std::shared_ptr<ProfileContext> context = m_contexts.value(profileId);
        if (context && !context->isActivated() && context->windows().isEmpty()) {
            m_contexts.remove(profileId);
            context.reset();
        }
        if (context) {
            WindowController *existing = m_windows->mostRecentProfileController(profileId, false);
            if (existing) {
                if (QQuickWindow *window = m_windows->windowFor(existing)) {
                    window->raise();
                    window->requestActivate();
                }
                m_recoveryMessage.clear();
                m_recoveryProfileId.clear();
                m_startupState = QStringLiteral("browsing");
                emit startupStateChanged();
                closeChooserWindow();
                closeLaunchWindow();
                return;
            }
        } else {
            ProfileRecord record;
            record.id = *identifier;
            record.displayName = summary->displayName;
            record.colorSeed = summary->colorSeed;
            record.avatarRevision = summary->avatarRevision;
            record.passwordVerifier = summary->protectedProfile ? QStringLiteral("set") : QString();
            record.lifecycle = summary->lifecycle;
            record.createdAt = summary->createdAt;
            record.lastUsedAt = summary->lastUsedAt;
            ProfileError error = ProfileError::None;
            const ProfilePaths paths(m_roots, *identifier);
            context = ProfileContext::create(record, paths, m_qmlEngine, &error);
            if (!context) {
                m_states.insert(profileId, ProfileSessionState::Locked);
                enterRecovery(profileErrorMessage(error), profileId);
                return;
            }
            m_contexts.insert(profileId, context);
            m_states.insert(profileId, ProfileSessionState::Unlocked);
        }
        const bool wantPrivate = m_launchPrivate && !becauseOfSwitch;
        WindowController *controller =
            createBrowserWindow(context, wantPrivate, m_launchEngineName, !wantPrivate, true);
        if (!controller) {
            return;
        }
        m_registry->touchLastUsed(*identifier, QDateTime::currentMSecsSinceEpoch(), true);
        deliverLaunchRequests(controller, context);
        m_recoveryMessage.clear();
        m_recoveryProfileId.clear();
        m_startupState = QStringLiteral("browsing");
        emit startupStateChanged();
        emit profileOpened(profileId);
        closeChooserWindow();
        closeLaunchWindow();
        rebuildModel();
    }

    WindowController *ProfileManager::createBrowserWindow(
        const std::shared_ptr<ProfileContext> &context,
        bool privateWindow,
        const QString &engineName,
        bool restoreSession,
        bool createInitialTab,
        bool restoreGeometry
    ) {
        if (!context || !m_qmlEngine || context->signOutInProgress()) {
            return nullptr;
        }
        const ProfileError activationError = context->activate();
        if (activationError != ProfileError::None) {
            enterRecovery(profileErrorMessage(activationError), context->idString());
            return nullptr;
        }
        QJsonObject session;
        if (restoreSession && !privateWindow) {
            session = context->sessions()->load();
            if (context->sessions()->error() != ProfileError::None) {
                enterRecovery(context->sessions()->errorMessage(), context->idString());
                return nullptr;
            }
        }
        QQmlComponent component(m_qmlEngine);
        component.loadFromModule("Eden.Ui", "BrowserWindow");
        if (component.isError()) {
            qWarning().noquote() << component.errorString();
            return nullptr;
        }
        QObject *created = component.createWithInitialProperties({{QStringLiteral("visible"), false}});
        QQuickWindow *window = qobject_cast<QQuickWindow *>(created);
        WindowController *controller =
            window ? window->findChild<WindowController *>(QStringLiteral("windowController")) : nullptr;
        if (!window || !controller) {
            delete created;
            return nullptr;
        }
        QQmlEngine::setObjectOwnership(window, QQmlEngine::CppOwnership);
        controller->initialize(context, privateWindow, engineName, createInitialTab && !restoreSession);
        if (!controller->isInitialized()) {
            delete window;
            return nullptr;
        }
        connect(window, &QQuickWindow::closing, window, [window] { window->deleteLater(); });
        SettingsStore *settingsStore = SettingsStore::instance();
        if (restoreGeometry) {
            window->resize(settingsStore->windowWidth(), settingsStore->windowHeight());
        }
        if (restoreGeometry && settingsStore->windowMaximized()) {
            window->showMaximized();
        } else {
            window->show();
        }
        if (restoreSession && !privateWindow) {
            QJsonArray sessionWindows = session.value(QLatin1String("windows")).toArray();
            if (sessionWindows.isEmpty() && session.contains(QLatin1String("tabs"))) {
                sessionWindows.append(session);
            }
            const int sessionVersion = session.value(QLatin1String("version")).toInt(0);
            for (qsizetype index = 0; index < sessionWindows.size(); ++index) {
                sessionWindows.replace(
                    index,
                    migrateLegacyStartupTab(sessionWindows.at(index).toObject(), sessionVersion)
                );
            }
            if (!sessionWindows.isEmpty()) {
                QSet<engine::Backend> requiredBackends;
                engine::EngineRegistry *registry = engine::EngineRegistry::instance();
                for (const QJsonValue &windowValue : std::as_const(sessionWindows)) {
                    for (const QJsonValue &tabValue : windowValue.toObject().value(QLatin1String("tabs")).toArray()) {
                        const QJsonObject tab = tabValue.toObject();
                        if (QUrl(tab.value(QLatin1String("url")).toString()).scheme() == QLatin1String("eden")) {
                            continue;
                        }
                        const std::optional<engine::Backend> backend =
                            registry->backendForId(tab.value(QLatin1String("backend")).toString());
                        if (backend) {
                            requiredBackends.insert(*backend);
                        }
                    }
                }
                for (engine::Backend backend : std::as_const(requiredBackends)) {
                    if (!engine::EngineFactory::initialize(backend)) {
                        qCritical().noquote()
                            << "A restored tab engine could not be initialized:" << registry->idForBackend(backend);
                    }
                }
                controller->restoreWindow(sessionWindows.at(0).toObject());
                for (qsizetype index = 1; index < sessionWindows.size(); ++index) {
                    WindowController *extra = createBrowserWindow(context, false, engineName, false, false);
                    if (!extra) {
                        continue;
                    }
                    extra->restoreWindow(sessionWindows.at(index).toObject());
                    if (extra->tabs() && extra->tabs()->rowCount() == 0) {
                        extra->newTab(context->settings()->homePageDestination());
                    }
                }
            }
            if (controller->tabs() && controller->tabs()->rowCount() == 0 && createInitialTab) {
                controller->newTab(context->settings()->homePageDestination());
            }
        }
        return controller;
    }

    void ProfileManager::deliverLaunchRequests(
        WindowController *controller,
        const std::shared_ptr<ProfileContext> &context
    ) {
        Q_UNUSED(context)
        if (!controller) {
            m_launchUrls.clear();
            return;
        }
        bool first = true;
        while (!m_launchUrls.isEmpty()) {
            const QUrl url = m_launchUrls.dequeue();
            if (first && controller->tabs() && controller->tabs()->rowCount() > 0) {
                controller->navigate(url);
            } else {
                controller->newTab(url);
            }
            first = false;
        }
        m_launchPrivate = false;
    }

    void ProfileManager::createProfile(
        const QString &displayName,
        bool requirePassword,
        const QString &password,
        const QString &confirmation,
        const QUrl &avatarSource
    ) {
        const std::optional<QString> normalizedName = normalizedDisplayName(displayName);
        if (!normalizedName) {
            emit createFailed(profileErrorMessage(ProfileError::InvalidDisplayName));
            return;
        }
        if (requirePassword) {
            const ProfileError passwordError = validateProfilePassword(password, confirmation);
            if (passwordError != ProfileError::None) {
                emit createFailed(profileErrorMessage(passwordError));
                return;
            }
        }
        const ProfileId identifier = ProfileId::generate();
        ProfileRecord draft;
        draft.id = identifier;
        draft.displayName = *normalizedName;
        draft.colorSeed = QRandomGenerator::system()->generate();
        draft.createdAt = QDateTime::currentMSecsSinceEpoch();
        draft.lastUsedAt = draft.createdAt;
        QPointer<ProfileManager> guard(this);
        m_registry->createProfile(
            draft,
            [guard, identifier, requirePassword, password, avatarSource](ProfileError error) {
                if (!guard) {
                    return;
                }
                if (error != ProfileError::None) {
                    emit guard->createFailed(profileErrorMessage(error));
                    return;
                }
                const ProfilePaths paths(guard->m_roots, identifier);
                if (!paths.ensureBaseDirectories()) {
                    guard->m_registry
                        ->removeProfile(identifier, guard->nextLastActiveAfterRemoval(identifier.toString()), {});
                    emit guard->createFailed(profileErrorMessage(ProfileError::DirectoryCreateFailed));
                    return;
                }
                const auto finishCreation = [guard, identifier](const QString &verifier) {
                    if (!guard) {
                        return;
                    }
                    guard->m_registry
                        ->commitProfileReady(identifier, verifier, [guard, identifier](ProfileError commitError) {
                            if (!guard) {
                                return;
                            }
                            if (commitError != ProfileError::None) {
                                emit guard->createFailed(profileErrorMessage(commitError));
                                return;
                            }
                            guard->openProfile(identifier.toString(), true);
                        });
                };
                const auto computeVerifier = [guard, requirePassword, password, finishCreation] {
                    if (!guard) {
                        return;
                    }
                    if (!requirePassword) {
                        finishCreation(QString());
                        return;
                    }
                    guard->m_hasher->hashPassword(password, [guard, finishCreation](bool success, QString verifier) {
                        if (!guard) {
                            return;
                        }
                        if (!success) {
                            emit guard->createFailed(profileErrorMessage(ProfileError::VerifierFailed));
                            return;
                        }
                        finishCreation(verifier);
                    });
                };
                if (avatarSource.isValid() && !avatarSource.isEmpty()) {
                    AvatarProcessor::process(
                        avatarSource,
                        paths.avatarPath(),
                        guard,
                        [guard, identifier, computeVerifier](ProfileError avatarError) {
                            if (!guard) {
                                return;
                            }
                            if (avatarError == ProfileError::None) {
                                guard->m_registry->incrementAvatarRevision(identifier, {});
                            }
                            computeVerifier();
                        }
                    );
                } else {
                    computeVerifier();
                }
            }
        );
    }

    void ProfileManager::signOut(const QString &profileId) {
        if (sessionStateFor(profileId) != ProfileSessionState::Unlocked) {
            return;
        }
        const std::shared_ptr<ProfileContext> context = m_contexts.value(profileId);
        if (!context) {
            return;
        }
        m_states.insert(profileId, ProfileSessionState::SigningOut);
        rebuildModel();
        const int downloads = context->activeDownloadCount();
        if (downloads > 0) {
            emit signOutConfirmationRequired(profileId, downloads);
            return;
        }
        confirmSignOut(profileId, true);
    }

    void ProfileManager::confirmSignOut(const QString &profileId, bool proceed) {
        if (sessionStateFor(profileId) != ProfileSessionState::SigningOut) {
            return;
        }
        if (!proceed) {
            m_states.insert(profileId, ProfileSessionState::Unlocked);
            rebuildModel();
            return;
        }
        performSignOut(profileId, true, {});
    }

    void ProfileManager::performSignOut(
        const QString &profileId,
        bool showChooserAfter,
        std::function<void(bool)> completion
    ) {
        if (m_signOutsRunning.contains(profileId)) {
            if (completion) {
                completion(false);
            }
            return;
        }
        const std::shared_ptr<ProfileContext> context = m_contexts.value(profileId);
        if (!context) {
            m_states.insert(profileId, ProfileSessionState::Locked);
            if (completion) {
                completion(true);
            }
            return;
        }
        m_signOutsRunning.insert(profileId);
        m_states.insert(profileId, ProfileSessionState::SigningOut);
        context->beginSignOutBarrier();
        const QList<WindowController *> privateControllers = m_windows->profileControllers(profileId, false, true);
        for (WindowController *controller : privateControllers) {
            controller->closeWindowNow();
        }
        context->saveSessionNow();
        const QList<WindowController *> normalControllers = m_windows->profileControllers(profileId, true, false);
        for (WindowController *controller : normalControllers) {
            controller->closeWindowNow();
        }
        QPointer<ProfileManager> guard(this);
        context->flushEngines(3000, [guard, profileId, context, showChooserAfter, completion] {
            if (!guard) {
                return;
            }
            context->shutdownStores();
            guard->m_signOutsRunning.remove(profileId);
            guard->m_contexts.remove(profileId);
            guard->m_states.insert(profileId, ProfileSessionState::Locked);
            guard->m_retryStates.remove(profileId);
            if (guard->m_windows->normalWindowCount() == 0) {
                guard->m_registry->setApplicationState(
                    QLatin1String(applicationstate::startupDisposition),
                    QStringLiteral("ChooseProfile")
                );
            }
            guard->rebuildModel();
            if (showChooserAfter) {
                guard->showChooser();
            }
            if (completion) {
                completion(true);
            }
        });
    }

    void ProfileManager::deleteProfileFromEditor(const QString &profileId) {
        if (!anyOtherReadyProfile(profileId)) {
            emit operationFailed(profileId, profileErrorMessage(ProfileError::LastProfile));
            return;
        }
        performDeletion(profileId);
    }

    void ProfileManager::requestForgottenPasswordDeletion(const QString &profileId, const QString &typedName) {
        const std::optional<ProfileSummary> summary = m_registry->summaryFor(profileId);
        if (!summary || !summary->protectedProfile || sessionStateFor(profileId) != ProfileSessionState::Locked) {
            return;
        }
        if (!anyOtherReadyProfile(profileId)) {
            emit operationFailed(profileId, profileErrorMessage(ProfileError::LastProfile));
            return;
        }
        if (typedName != summary->displayName) {
            emit operationFailed(profileId, QStringLiteral("Type the profile name exactly to continue."));
            return;
        }
        emit forgottenDeletionConfirmationRequired(profileId, summary->displayName);
    }

    void ProfileManager::confirmForgottenPasswordDeletion(const QString &profileId, bool proceed) {
        if (!proceed) {
            return;
        }
        if (sessionStateFor(profileId) != ProfileSessionState::Locked || m_contexts.contains(profileId)) {
            return;
        }
        if (!anyOtherReadyProfile(profileId)) {
            return;
        }
        performDeletion(profileId);
    }

    void ProfileManager::performDeletion(const QString &profileId) {
        if (m_migrationsRunning.contains(profileId)) {
            return;
        }
        const std::optional<ProfileId> identifier = ProfileId::parse(profileId);
        if (!identifier) {
            return;
        }
        m_states.insert(profileId, ProfileSessionState::Deleting);
        emit editorCloseRequested(profileId);
        QPointer<ProfileManager> guard(this);
        m_registry->setLifecycle(*identifier, ProfileLifecycle::Deleting, [guard, profileId](ProfileError error) {
            if (!guard) {
                return;
            }
            if (error != ProfileError::None) {
                guard->m_states.insert(profileId, ProfileSessionState::Locked);
                emit guard->operationFailed(profileId, profileErrorMessage(error));
                return;
            }
            if (guard->m_contexts.contains(profileId)) {
                guard->performSignOut(profileId, false, [guard, profileId](bool) {
                    if (guard) {
                        guard->m_states.insert(profileId, ProfileSessionState::Deleting);
                        guard->stageAndRemoveProfile(profileId);
                    }
                });
            } else {
                guard->stageAndRemoveProfile(profileId);
            }
        });
    }

    void ProfileManager::stageAndRemoveProfile(const QString &profileId) {
        const std::optional<ProfileId> identifier = ProfileId::parse(profileId);
        if (!identifier) {
            return;
        }
        const ProfilePaths paths(m_roots, *identifier);
        const QStringList owned = paths.ownedDirectories(knownBackendIds());
        QList<std::pair<QString, QString>> stagedPairs;
        bool failed = false;
        for (const QString &directory : owned) {
            if (!QFileInfo::exists(directory)) {
                continue;
            }
            const QString parent = QFileInfo(directory).absolutePath();
            const QString stagingName = ProfilePaths::deletionStagingName(*identifier, ++m_stagingOrdinal);
            const QString stagingPath = parent + QLatin1Char('/') + stagingName;
            if (QDir().rename(directory, stagingPath)) {
                stagedPairs.append({directory, stagingPath});
            } else {
                failed = true;
                break;
            }
        }
        if (failed) {
            for (const auto &[original, stagingPath] : stagedPairs) {
                QDir().rename(stagingPath, original);
            }
            m_registry->setLifecycle(*identifier, ProfileLifecycle::Ready, {});
            m_states.insert(profileId, ProfileSessionState::Locked);
            emit operationFailed(profileId, profileErrorMessage(ProfileError::StagingFailed));
            return;
        }
        QStringList staged;
        staged.reserve(stagedPairs.size());
        for (const auto &[original, stagingPath] : stagedPairs) {
            staged.append(stagingPath);
        }
        QPointer<ProfileManager> guard(this);
        m_registry->removeProfile(
            *identifier,
            nextLastActiveAfterRemoval(profileId),
            [guard, profileId, staged](ProfileError error) {
                if (!guard) {
                    return;
                }
                guard->m_states.remove(profileId);
                guard->m_retryStates.remove(profileId);
                if (guard->m_avatarProvider) {
                    guard->m_avatarProvider->removeProfile(profileId);
                }
                if (error != ProfileError::None) {
                    emit guard->operationFailed(profileId, profileErrorMessage(error));
                    return;
                }
                QThreadPool::globalInstance()->start([guard, staged] {
                    int failures = 0;
                    for (const QString &stagingPath : staged) {
                        if (!QDir(stagingPath).removeRecursively()) {
                            ++failures;
                        }
                    }
                    if (guard && failures > 0) {
                        QMetaObject::invokeMethod(
                            guard.data(),
                            [guard, failures] {
                                if (guard) {
                                    guard->m_pendingCleanupCount += failures;
                                    emit guard->pendingCleanupCountChanged();
                                }
                            },
                            Qt::QueuedConnection
                        );
                    }
                });
                guard->rebuildModel();
                if (guard->m_windows->normalWindowCount() == 0) {
                    guard->showChooser();
                }
            }
        );
    }

    void ProfileManager::cleanupStagedDirectories() {
        const QStringList parents = ProfilePaths::deletionStagingParents(m_roots, knownBackendIds());
        QStringList staged;
        for (const QString &parent : parents) {
            QDirIterator iterator(parent, QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
            while (iterator.hasNext()) {
                const QString path = iterator.next();
                if (ProfilePaths::isDeletionStagingName(QFileInfo(path).fileName())) {
                    staged.append(path);
                }
            }
        }
        if (staged.isEmpty()) {
            return;
        }
        QPointer<ProfileManager> guard(this);
        QThreadPool::globalInstance()->start([guard, staged] {
            int failures = 0;
            for (const QString &path : staged) {
                if (!QDir(path).removeRecursively()) {
                    ++failures;
                }
            }
            if (guard) {
                QMetaObject::invokeMethod(
                    guard.data(),
                    [guard, failures] {
                        if (guard && guard->m_pendingCleanupCount != failures) {
                            guard->m_pendingCleanupCount = failures;
                            emit guard->pendingCleanupCountChanged();
                        }
                    },
                    Qt::QueuedConnection
                );
            }
        });
    }

    void ProfileManager::resolveCreatingProfile(const QString &profileId, bool complete) {
        const std::optional<ProfileSummary> summary = m_registry->summaryFor(profileId);
        const std::optional<ProfileId> identifier = ProfileId::parse(profileId);
        if (!summary || !identifier || summary->lifecycle != ProfileLifecycle::Creating ||
            m_migrationsRunning.contains(profileId)) {
            return;
        }
        if (complete) {
            m_migrationsRunning.insert(profileId);
            const ProfilePaths paths(m_roots, *identifier);
            QPointer<ProfileManager> guard(this);
            QThreadPool::globalInstance()->start([guard, paths, identifier = *identifier] {
                const ProfileError error = ProfileMigration::resume(paths);
                if (!guard) {
                    return;
                }
                QMetaObject::invokeMethod(
                    guard,
                    [guard, identifier, error] {
                        if (!guard) {
                            return;
                        }
                        if (error != ProfileError::None) {
                            guard->m_migrationsRunning.remove(identifier.toString());
                            guard->enterRecovery(profileErrorMessage(error));
                            return;
                        }
                        guard->m_registry
                            ->commitProfileReady(identifier, {}, [guard, identifier](ProfileError committed) {
                                if (!guard) {
                                    return;
                                }
                                guard->m_migrationsRunning.remove(identifier.toString());
                                if (committed == ProfileError::None) {
                                    guard->m_registry->setApplicationState(
                                        QLatin1String(applicationstate::legacyMigrationVersion),
                                        QStringLiteral("1")
                                    );
                                    QFile::remove(ProfileMigration::journalPath(guard->m_roots));
                                } else {
                                    guard->enterRecovery(profileErrorMessage(committed));
                                }
                            });
                    },
                    Qt::QueuedConnection
                );
            });
        } else {
            performDeletion(profileId);
        }
    }

    void ProfileManager::renameProfile(
        const QString &profileId,
        const QString &displayName,
        ProfileRegistry::VoidCallback callback
    ) {
        const std::optional<QString> normalizedName = normalizedDisplayName(displayName);
        const std::optional<ProfileId> identifier = ProfileId::parse(profileId);
        if (!normalizedName || !identifier) {
            if (callback) {
                callback(ProfileError::InvalidDisplayName);
            }
            return;
        }
        m_registry->updateDisplayName(*identifier, *normalizedName, std::move(callback));
    }

    void ProfileManager::recolorProfile(const QString &profileId, ProfileRegistry::VoidCallback callback) {
        const std::optional<ProfileSummary> summary = m_registry->summaryFor(profileId);
        const std::optional<ProfileId> identifier = ProfileId::parse(profileId);
        if (!summary || !identifier) {
            if (callback) {
                callback(ProfileError::ProfileMissing);
            }
            return;
        }
        m_registry->updateColorSeed(*identifier, summary->colorSeed + 1, std::move(callback));
    }

    void ProfileManager::chooseProfileAvatar(
        const QString &profileId,
        const QUrl &sourceUrl,
        ProfileRegistry::VoidCallback callback
    ) {
        const std::optional<ProfileId> identifier = ProfileId::parse(profileId);
        if (!identifier) {
            if (callback) {
                callback(ProfileError::ProfileMissing);
            }
            return;
        }
        const ProfilePaths paths(m_roots, *identifier);
        QPointer<ProfileManager> guard(this);
        AvatarProcessor::process(
            sourceUrl,
            paths.avatarPath(),
            this,
            [guard, identifier = *identifier, callback = std::move(callback)](ProfileError error) {
                if (!guard) {
                    return;
                }
                if (error != ProfileError::None) {
                    if (callback) {
                        callback(error);
                    }
                    return;
                }
                guard->m_registry->incrementAvatarRevision(identifier, std::move(callback));
            }
        );
    }

    void ProfileManager::removeProfileAvatar(const QString &profileId, ProfileRegistry::VoidCallback callback) {
        const std::optional<ProfileId> identifier = ProfileId::parse(profileId);
        if (!identifier) {
            if (callback) {
                callback(ProfileError::ProfileMissing);
            }
            return;
        }
        const ProfilePaths paths(m_roots, *identifier);
        QPointer<ProfileManager> guard(this);
        m_registry->incrementAvatarRevision(
            *identifier,
            [guard, paths, callback = std::move(callback)](ProfileError error) {
                if (error == ProfileError::None) {
                    QFile::remove(paths.avatarPath());
                }
                if (callback) {
                    callback(error);
                }
            }
        );
    }

    void ProfileManager::verifyCurrentPassword(
        const QString &profileId,
        const QString &password,
        std::function<void(bool)> callback
    ) {
        const std::optional<ProfileId> identifier = ProfileId::parse(profileId);
        if (!identifier) {
            if (callback) {
                callback(false);
            }
            return;
        }
        QPointer<ProfileManager> guard(this);
        m_registry->fetchPasswordVerifier(
            *identifier,
            [guard, password, callback = std::move(callback)](ProfileError error, QString verifier) {
                if (!guard) {
                    return;
                }
                if (error != ProfileError::None || verifier.isEmpty()) {
                    callback(false);
                    return;
                }
                guard->m_hasher->verifyPassword(verifier, password, [callback](bool matches, bool) {
                    callback(matches);
                });
            }
        );
    }

    void ProfileManager::changeProfilePassword(
        const QString &profileId,
        const QString &currentPassword,
        const QString &nextPassword,
        ProfileRegistry::VoidCallback callback
    ) {
        const std::optional<ProfileSummary> summary = m_registry->summaryFor(profileId);
        const std::optional<ProfileId> identifier = ProfileId::parse(profileId);
        if (!summary || !identifier) {
            if (callback) {
                callback(ProfileError::ProfileMissing);
            }
            return;
        }
        const ProfileError passwordError = validateProfilePassword(nextPassword, nextPassword);
        if (passwordError != ProfileError::None) {
            if (callback) {
                callback(passwordError);
            }
            return;
        }
        QPointer<ProfileManager> guard(this);
        const std::function<void()> computeAndStore = [guard, identifier = *identifier, nextPassword, callback] {
            if (!guard) {
                return;
            }
            guard->m_hasher->hashPassword(nextPassword, [guard, identifier, callback](bool success, QString verifier) {
                if (!guard) {
                    return;
                }
                if (!success) {
                    if (callback) {
                        callback(ProfileError::VerifierFailed);
                    }
                    return;
                }
                guard->m_registry->updatePasswordVerifier(identifier, verifier, callback);
            });
        };
        if (summary->protectedProfile) {
            verifyCurrentPassword(profileId, currentPassword, [computeAndStore, callback](bool matches) {
                if (!matches) {
                    if (callback) {
                        callback(ProfileError::WrongPassword);
                    }
                    return;
                }
                computeAndStore();
            });
        } else {
            computeAndStore();
        }
    }

    void ProfileManager::removeProfilePassword(
        const QString &profileId,
        const QString &currentPassword,
        ProfileRegistry::VoidCallback callback
    ) {
        const std::optional<ProfileSummary> summary = m_registry->summaryFor(profileId);
        const std::optional<ProfileId> identifier = ProfileId::parse(profileId);
        if (!summary || !identifier) {
            if (callback) {
                callback(ProfileError::ProfileMissing);
            }
            return;
        }
        if (!summary->protectedProfile) {
            if (callback) {
                callback(ProfileError::None);
            }
            return;
        }
        QPointer<ProfileManager> guard(this);
        verifyCurrentPassword(
            profileId,
            currentPassword,
            [guard, identifier = *identifier, callback = std::move(callback)](bool matches) mutable {
                if (!guard) {
                    return;
                }
                if (!matches) {
                    if (callback) {
                        callback(ProfileError::WrongPassword);
                    }
                    return;
                }
                guard->m_registry->updatePasswordVerifier(identifier, QString(), std::move(callback));
            }
        );
    }

    void ProfileManager::openChooser() {
        showChooser();
    }

    void ProfileManager::openCreateWindow(QObject *originatingController) {
        showChooser({}, false, originatingWindow(originatingController, m_windows));
        if (m_chooserWindow) {
            QMetaObject::invokeMethod(m_chooserWindow, "showCreate");
        }
    }

    void ProfileManager::openEditor(const QString &profileId, QObject *originatingController) {
        if (const QPointer<ProfileEditorController> existing = m_editors.value(profileId)) {
            if (existing && existing->window()) {
                existing->window()->raise();
                existing->window()->requestActivate();
                return;
            }
        }
        const std::optional<ProfileSummary> summary = m_registry->summaryFor(profileId);
        if (!summary || !m_qmlEngine || sessionStateFor(profileId) != ProfileSessionState::Unlocked) {
            return;
        }
        ProfileEditorController *editor = new ProfileEditorController(this, profileId);
        QQmlComponent component(m_qmlEngine);
        component.loadFromModule("Eden.Ui", "ProfileEditorWindow");
        if (component.isError()) {
            qWarning().noquote() << component.errorString();
            delete editor;
            return;
        }
        QObject *created = component.createWithInitialProperties({
            {QStringLiteral("editor"), QVariant::fromValue(static_cast<QObject *>(editor))},
            {QStringLiteral("visible"), false},
        });
        QQuickWindow *window = qobject_cast<QQuickWindow *>(created);
        if (!window) {
            delete created;
            delete editor;
            return;
        }
        QQmlEngine::setObjectOwnership(window, QQmlEngine::CppOwnership);
        editor->setWindow(window);
        editor->setParent(window);
        m_editors.insert(profileId, editor);
        connect(window, &QQuickWindow::closing, window, [window] { window->deleteLater(); });
        QQuickWindow *parentWindow = originatingWindow(originatingController, m_windows);
        if (!parentWindow) {
            if (WindowController *active = m_windows->mostRecentProfileController(profileId, false)) {
                parentWindow = m_windows->windowFor(active);
            }
        }
        if (parentWindow) {
            centerTransientWindow(window, parentWindow);
        }
        window->show();
        window->requestActivate();
    }

    QVariantList ProfileManager::profileMenuActions(const QString &profileId, const QString &mode) const {
        QVariantList actions;
        QVariantList switchActions;
        for (const ProfileSummary &summary : m_registry->snapshot().profiles) {
            if (summary.profileId == profileId || summary.lifecycle != ProfileLifecycle::Ready) {
                continue;
            }
            QVariantMap action;
            action.insert("id", QStringLiteral("switch-profile:%1").arg(summary.profileId));
            action.insert("title", summary.displayName);
            action.insert(
                "avatarUrl",
                QStringLiteral("image://eden-profile-avatar/%1?revision=%2&seed=%3")
                    .arg(summary.profileId)
                    .arg(summary.avatarRevision)
                    .arg(summary.colorSeed)
            );
            action.insert("trailingIcon", summary.protectedProfile ? QStringLiteral("lock") : QString());
            switchActions.append(action);
        }
        if (!switchActions.isEmpty()) {
            switchActions.append(QVariantMap{{"id", "add-profile"}, {"title", "Add profile"}, {"icon", "add"}});
            actions.append(
                QVariantMap{
                    {"id", "switch-profile-menu"},
                    {"title", "Switch profile"},
                    {"icon", "user-circle"},
                    {"children", switchActions},
                }
            );
        }
        actions.append(QVariantMap{{"id", "add-profile"}, {"title", "Add profile"}, {"icon", "add"}});
        if (mode == QLatin1String("normal")) {
            actions.append(
                QVariantMap{
                    {"id", "customize-profile"},
                    {"title", "Customize profile"},
                    {"icon", "gallery-edit"},
                }
            );
        }
        actions.append(QVariantMap{{"id", "sign-out"}, {"title", "Sign out"}, {"icon", "logout"}});
        return actions;
    }

    QQuickWindow *ProfileManager::createProfileWindow() {
        if (!m_qmlEngine) {
            return nullptr;
        }
        QQmlComponent component(m_qmlEngine);
        component.loadFromModule("Eden.Ui", "ProfileWindow");
        if (component.isError()) {
            qWarning().noquote() << component.errorString();
            return nullptr;
        }
        QObject *created = component.createWithInitialProperties({{QStringLiteral("visible"), false}});
        QQuickWindow *window = qobject_cast<QQuickWindow *>(created);
        if (!window) {
            delete created;
            return nullptr;
        }
        QQmlEngine::setObjectOwnership(window, QQmlEngine::CppOwnership);
        connect(window, &QQuickWindow::closing, window, [window] { window->deleteLater(); });
        return window;
    }

    void
    ProfileManager::showChooser(const QString &passwordProfileId, bool forgottenMode, QQuickWindow *transientParent) {
        if (!m_chooserWindow) {
            m_chooserWindow = createProfileWindow();
            if (!m_chooserWindow) {
                return;
            }
        }
        if (!transientParent) {
            const QString activeProfile = m_windows->mostRecentActiveProfileId();
            if (!activeProfile.isEmpty()) {
                if (WindowController *active = m_windows->mostRecentProfileController(activeProfile, false)) {
                    transientParent = m_windows->windowFor(active);
                }
            }
        }
        if (transientParent) {
            centerTransientWindow(m_chooserWindow, transientParent);
        }
        m_chooserWindow->show();
        m_chooserWindow->raise();
        m_chooserWindow->requestActivate();
        if (!passwordProfileId.isEmpty()) {
            QMetaObject::invokeMethod(
                m_chooserWindow,
                "showPassword",
                Q_ARG(QVariant, passwordProfileId),
                Q_ARG(QVariant, forgottenMode)
            );
        }
        if (m_startupState == QLatin1String("loading") || m_startupState == QLatin1String("recovery")) {
            m_recoveryMessage.clear();
            m_recoveryProfileId.clear();
            m_startupState = QStringLiteral("chooser");
            emit startupStateChanged();
        }
        closeLaunchWindow();
    }

    void ProfileManager::closeChooserWindow() {
        if (m_chooserWindow) {
            m_chooserWindow->close();
            m_chooserWindow.clear();
        }
    }

    void ProfileManager::closeLaunchWindow() {
        if (m_launchWindow) {
            m_launchWindow->close();
            m_launchWindow.clear();
        }
    }

    void ProfileManager::quitApplication() {
        QCoreApplication::quit();
    }

    void ProfileManager::handleAboutToQuit() {
        for (const std::shared_ptr<ProfileContext> &context : std::as_const(m_contexts)) {
            if (context) {
                context->saveSessionNow();
            }
        }
        if (m_windows->normalWindowCount() > 0) {
            const QString activeProfile = m_windows->mostRecentActiveProfileId();
            m_registry->setApplicationState(
                QLatin1String(applicationstate::startupDisposition),
                QStringLiteral("ResumeLast")
            );
            if (const std::optional<ProfileId> identifier = ProfileId::parse(activeProfile)) {
                m_registry->touchLastUsed(*identifier, QDateTime::currentMSecsSinceEpoch(), true);
            }
        }
    }

    void ProfileManager::shutdownContexts() {
        const QList<QString> profileIds = m_contexts.keys();
        for (const QString &profileId : profileIds) {
            const std::shared_ptr<ProfileContext> context = m_contexts.value(profileId);
            if (!context) {
                continue;
            }
            context->beginSignOutBarrier();
            const QList<WindowController *> controllers = context->windows();
            for (WindowController *controller : controllers) {
                if (!controller) {
                    continue;
                }
                QQuickWindow *window = controller->window();
                controller->prepareToClose();
                delete window;
            }
            context->shutdownStores();
            m_contexts.remove(profileId);
        }
    }

    QString ProfileManager::nextLastActiveAfterRemoval(const QString &removedId) const {
        const ProfileRegistry::Snapshot &snapshot = m_registry->snapshot();
        const QString currentLastActive =
            snapshot.applicationState.value(QLatin1String(applicationstate::lastActiveProfileId));
        if (currentLastActive != removedId && !currentLastActive.isEmpty()) {
            return currentLastActive;
        }
        const ProfileSummary *best = nullptr;
        for (const ProfileSummary &summary : snapshot.profiles) {
            if (summary.profileId == removedId || summary.lifecycle != ProfileLifecycle::Ready) {
                continue;
            }
            if (!best || summary.lastUsedAt > best->lastUsedAt) {
                best = &summary;
            }
        }
        return best ? best->profileId : QString();
    }

    bool ProfileManager::anyOtherReadyProfile(const QString &profileId) const {
        for (const ProfileSummary &summary : m_registry->snapshot().profiles) {
            if (summary.profileId != profileId && summary.lifecycle == ProfileLifecycle::Ready) {
                return true;
            }
        }
        return false;
    }

}

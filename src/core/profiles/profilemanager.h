#pragma once

#include "core/profiles/profileerror.h"
#include "core/profiles/profilepaths.h"
#include "core/profiles/profileregistry.h"
#include "core/profiles/profiletypes.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include <memory>

class QQmlEngine;
class QQuickWindow;

namespace eden::core {

    class AvatarImageProvider;
    class PasswordHasher;
    class ProfileContext;
    class ProfileEditorController;
    class ProfileListModel;
    class WindowController;
    class WindowRegistry;

    class ProfileManager final : public QObject {
        Q_OBJECT
        Q_PROPERTY(eden::core::ProfileListModel *profiles READ profileModel CONSTANT)
        Q_PROPERTY(QString startupState READ startupState NOTIFY startupStateChanged)
        Q_PROPERTY(QString recoveryMessage READ recoveryMessage NOTIFY startupStateChanged)
        Q_PROPERTY(bool recoveryCanRetry READ recoveryCanRetry NOTIFY startupStateChanged)
        Q_PROPERTY(
            bool showChooserOnStartup READ showChooserOnStartup WRITE setShowChooserOnStartup NOTIFY
                showChooserOnStartupChanged
        )
        Q_PROPERTY(int pendingCleanupCount READ pendingCleanupCount NOTIFY pendingCleanupCountChanged)
        Q_PROPERTY(int browserWindowCount READ browserWindowCount NOTIFY browserWindowCountChanged)

      public:
        ProfileManager(
            ProfileRegistry *registry,
            WindowRegistry *windows,
            const ProfilePaths::Roots &roots,
            QObject *parent = nullptr
        );
        ~ProfileManager() override;

        static ProfileManager *instance();

        void setQmlEngine(QQmlEngine *engine);
        void setLaunchWindow(QQuickWindow *window);
        void configureLaunch(bool privateWindow, const QString &engineName, const QList<QUrl> &urls);
        void beginStartup();
        void handleAboutToQuit();
        void shutdownContexts();

        ProfileListModel *profileModel() const;
        QString startupState() const;
        QString recoveryMessage() const;
        bool recoveryCanRetry() const;
        bool showChooserOnStartup() const;
        void setShowChooserOnStartup(bool show);
        int pendingCleanupCount() const;
        int browserWindowCount() const;

        std::shared_ptr<ProfileContext> contextFor(const QString &profileId) const;
        ProfileSessionState sessionStateFor(const QString &profileId) const;
        WindowController *createBrowserWindow(
            const std::shared_ptr<ProfileContext> &context,
            bool privateWindow,
            const QString &engineName,
            bool restoreSession,
            bool createInitialTab,
            bool restoreGeometry = true
        );

        Q_INVOKABLE void activateProfile(const QString &profileId);
        Q_INVOKABLE void retryProfileOpen();
        Q_INVOKABLE void switchToProfile(const QString &profileId);
        Q_INVOKABLE void submitPassword(const QString &profileId, const QString &password);
        Q_INVOKABLE int passwordCooldownSeconds(const QString &profileId) const;
        Q_INVOKABLE void createProfile(
            const QString &displayName,
            bool requirePassword,
            const QString &password,
            const QString &confirmation,
            const QUrl &avatarSource
        );
        Q_INVOKABLE void signOut(const QString &profileId);
        Q_INVOKABLE void confirmSignOut(const QString &profileId, bool proceed);
        Q_INVOKABLE void openChooser();
        Q_INVOKABLE void openCreateWindow(QObject *originatingController = nullptr);
        Q_INVOKABLE void openEditor(const QString &profileId, QObject *originatingController = nullptr);
        Q_INVOKABLE QVariantList profileMenuActions(const QString &profileId, const QString &mode) const;
        Q_INVOKABLE void requestForgottenPasswordDeletion(const QString &profileId, const QString &typedName);
        Q_INVOKABLE void confirmForgottenPasswordDeletion(const QString &profileId, bool proceed);
        Q_INVOKABLE void resolveCreatingProfile(const QString &profileId, bool complete);
        Q_INVOKABLE void quitApplication();

        void deleteProfileFromEditor(const QString &profileId);
        void
        renameProfile(const QString &profileId, const QString &displayName, ProfileRegistry::VoidCallback callback);
        void recolorProfile(const QString &profileId, ProfileRegistry::VoidCallback callback);
        void
        chooseProfileAvatar(const QString &profileId, const QUrl &sourceUrl, ProfileRegistry::VoidCallback callback);
        void removeProfileAvatar(const QString &profileId, ProfileRegistry::VoidCallback callback);
        void changeProfilePassword(
            const QString &profileId,
            const QString &currentPassword,
            const QString &nextPassword,
            ProfileRegistry::VoidCallback callback
        );
        void removeProfilePassword(
            const QString &profileId,
            const QString &currentPassword,
            ProfileRegistry::VoidCallback callback
        );
        void
        verifyCurrentPassword(const QString &profileId, const QString &password, std::function<void(bool)> callback);

        ProfileRegistry *registry() const;
        WindowRegistry *windowRegistry() const;
        const ProfilePaths::Roots &roots() const;
        QStringList knownBackendIds() const;

      signals:
        void startupStateChanged();
        void showChooserOnStartupChanged();
        void pendingCleanupCountChanged();
        void browserWindowCountChanged();
        void passwordRequired(const QString &profileId);
        void unlockFailed(const QString &profileId, const QString &message, int cooldownSeconds);
        void cooldownFinished(const QString &profileId);
        void unlockSucceeded(const QString &profileId);
        void createFailed(const QString &message);
        void signOutConfirmationRequired(const QString &profileId, int downloadCount);
        void forgottenDeletionConfirmationRequired(const QString &profileId, const QString &displayName);
        void operationFailed(const QString &profileId, const QString &message);
        void editorCloseRequested(const QString &profileId);
        void profileOpened(const QString &profileId);
        void profileMenuActionsChanged();

      private:
        struct RetryState {
            int failures = 0;
            qint64 cooldownUntilMilliseconds = 0;
        };

        void handleRegistryLoaded(ProfileRegistry::LoadStatus status);
        void handleSnapshotChanged();
        void rebuildModel();
        void syncAvatarProvider();
        void resumeInterruptedLifecycles();
        void applyStartupDecision();
        void bootstrapDefaultProfile();
        void enterRecovery(const QString &message, const QString &profileId = {});
        void openProfile(const QString &profileId, bool becauseOfSwitch);
        void finishUnlock(const QString &profileId);
        void deliverLaunchRequests(WindowController *controller, const std::shared_ptr<ProfileContext> &context);
        void performSignOut(const QString &profileId, bool showChooserAfter, std::function<void(bool)> completion);
        void performDeletion(const QString &profileId);
        void stageAndRemoveProfile(const QString &profileId);
        void cleanupStagedDirectories();
        void closeChooserWindow();
        void closeLaunchWindow();
        void showChooser(
            const QString &passwordProfileId = {},
            bool forgottenMode = false,
            QQuickWindow *transientParent = nullptr
        );
        QQuickWindow *createProfileWindow();
        void applyRetryFailure(const QString &profileId);
        int remainingCooldownSeconds(const QString &profileId, qint64 nowMilliseconds) const;
        QString nextLastActiveAfterRemoval(const QString &removedId) const;
        bool anyOtherReadyProfile(const QString &profileId) const;

        ProfileRegistry *m_registry;
        WindowRegistry *m_windows;
        ProfilePaths::Roots m_roots;
        QQmlEngine *m_qmlEngine = nullptr;
        AvatarImageProvider *m_avatarProvider = nullptr;
        std::unique_ptr<PasswordHasher> m_hasher;
        std::unique_ptr<ProfileListModel> m_model;
        QHash<QString, std::shared_ptr<ProfileContext>> m_contexts;
        QHash<QString, ProfileSessionState> m_states;
        QHash<QString, RetryState> m_retryStates;
        QHash<QString, QPointer<ProfileEditorController>> m_editors;
        QSet<QString> m_signOutsRunning;
        QSet<QString> m_migrationsRunning;
        QPointer<QQuickWindow> m_launchWindow;
        QPointer<QQuickWindow> m_chooserWindow;
        QString m_startupState = QStringLiteral("loading");
        bool m_preparingStorage = false;
        bool m_storageRecovery = false;
        QString m_recoveryMessage;
        QString m_recoveryProfileId;
        bool m_recoveryBecauseOfSwitch = false;
        QTimer m_cooldownTimer;
        QString m_cooldownProfileId;
        QQueue<QUrl> m_launchUrls;
        QString m_launchEngineName;
        bool m_launchPrivate = false;
        bool m_startupDecisionApplied = false;
        int m_pendingCleanupCount = 0;
        int m_stagingOrdinal = 0;
    };

}

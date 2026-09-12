#include "core/profiles/profileeditorcontroller.h"

#include "core/profiles/profilecolors.h"
#include "core/profiles/profilecontext.h"
#include "core/profiles/profilemanager.h"
#include "core/profiles/profilevalidation.h"

#include <QPointer>
#include <QQuickWindow>

namespace eden::core {

    ProfileEditorController::ProfileEditorController(ProfileManager *manager, const QString &profileId, QObject *parent)
        : QObject(parent),
          m_manager(manager),
          m_profileId(profileId) {
        connect(manager->registry(), &ProfileRegistry::snapshotChanged, this, &ProfileEditorController::stateChanged);
        connect(manager, &ProfileManager::editorCloseRequested, this, [this](const QString &closingProfileId) {
            if (closingProfileId == m_profileId) {
                emit closeRequested();
            }
        });
    }

    QString ProfileEditorController::profileId() const {
        return m_profileId;
    }

    QString ProfileEditorController::displayName() const {
        const std::optional<ProfileSummary> summary = m_manager->registry()->summaryFor(m_profileId);
        return summary ? summary->displayName : QString();
    }

    QString ProfileEditorController::avatarUrl() const {
        const std::optional<ProfileSummary> summary = m_manager->registry()->summaryFor(m_profileId);
        if (!summary) {
            return {};
        }
        return QStringLiteral("image://eden-profile-avatar/%1?revision=%2")
            .arg(summary->profileId)
            .arg(summary->avatarRevision);
    }

    QColor ProfileEditorController::color() const {
        const std::optional<ProfileSummary> summary = m_manager->registry()->summaryFor(m_profileId);
        return profileColorForSeed(summary ? summary->colorSeed : 0);
    }

    bool ProfileEditorController::protectedProfile() const {
        const std::optional<ProfileSummary> summary = m_manager->registry()->summaryFor(m_profileId);
        return summary && summary->protectedProfile;
    }

    bool ProfileEditorController::canDelete() const {
        const ProfileRegistry::Snapshot &snapshot = m_manager->registry()->snapshot();
        int readyCount = 0;
        for (const ProfileSummary &summary : snapshot.profiles) {
            if (summary.lifecycle == ProfileLifecycle::Ready) {
                ++readyCount;
            }
        }
        return readyCount > 1;
    }

    bool ProfileEditorController::busy() const {
        return m_busy;
    }

    bool ProfileEditorController::avatarPending() const {
        return m_avatarPending;
    }

    QString ProfileEditorController::errorMessage() const {
        return m_errorMessage;
    }

    int ProfileEditorController::activeDownloadCount() const {
        const std::shared_ptr<ProfileContext> context = m_manager->contextFor(m_profileId);
        return context ? context->activeDownloadCount() : 0;
    }

    QQuickWindow *ProfileEditorController::window() const {
        return m_window.data();
    }

    void ProfileEditorController::setWindow(QQuickWindow *window) {
        m_window = window;
    }

    void ProfileEditorController::finishOperation(ProfileError error) {
        setBusy(false);
        m_avatarPending = false;
        if (error != ProfileError::None) {
            setError(profileErrorMessage(error));
        } else {
            setError(QString());
        }
        emit stateChanged();
    }

    void ProfileEditorController::setBusy(bool busy) {
        if (m_busy != busy) {
            m_busy = busy;
            emit busyChanged();
        }
    }

    void ProfileEditorController::setError(const QString &message) {
        if (m_errorMessage != message) {
            m_errorMessage = message;
            emit errorMessageChanged();
        }
    }

    void ProfileEditorController::clearError() {
        setError(QString());
    }

    void ProfileEditorController::rename(const QString &displayName) {
        if (m_busy) {
            return;
        }
        if (!normalizedDisplayName(displayName)) {
            setError(profileErrorMessage(ProfileError::InvalidDisplayName));
            return;
        }
        setBusy(true);
        QPointer<ProfileEditorController> guard(this);
        m_manager->renameProfile(m_profileId, displayName, [guard](ProfileError error) {
            if (guard) {
                guard->finishOperation(error);
            }
        });
    }

    void ProfileEditorController::cycleColor() {
        if (m_busy) {
            return;
        }
        setBusy(true);
        QPointer<ProfileEditorController> guard(this);
        m_manager->recolorProfile(m_profileId, [guard](ProfileError error) {
            if (guard) {
                guard->finishOperation(error);
            }
        });
    }

    void ProfileEditorController::chooseAvatar(const QUrl &fileUrl) {
        if (m_busy) {
            return;
        }
        setBusy(true);
        m_avatarPending = true;
        emit busyChanged();
        QPointer<ProfileEditorController> guard(this);
        m_manager->chooseProfileAvatar(m_profileId, fileUrl, [guard](ProfileError error) {
            if (guard) {
                guard->finishOperation(error);
            }
        });
    }

    void ProfileEditorController::removeAvatar() {
        if (m_busy) {
            return;
        }
        setBusy(true);
        QPointer<ProfileEditorController> guard(this);
        m_manager->removeProfileAvatar(m_profileId, [guard](ProfileError error) {
            if (guard) {
                guard->finishOperation(error);
            }
        });
    }

    void ProfileEditorController::setPassword(
        const QString &currentPassword,
        const QString &nextPassword,
        const QString &confirmation
    ) {
        if (m_busy) {
            return;
        }
        const ProfileError validation = validateProfilePassword(nextPassword, confirmation);
        if (validation != ProfileError::None) {
            setError(profileErrorMessage(validation));
            return;
        }
        setBusy(true);
        QPointer<ProfileEditorController> guard(this);
        m_manager->changeProfilePassword(m_profileId, currentPassword, nextPassword, [guard](ProfileError error) {
            if (guard) {
                guard->finishOperation(error);
                if (error == ProfileError::None) {
                    emit guard->passwordChangeSucceeded();
                }
            }
        });
    }

    void ProfileEditorController::removePassword(const QString &currentPassword) {
        if (m_busy) {
            return;
        }
        setBusy(true);
        QPointer<ProfileEditorController> guard(this);
        m_manager->removeProfilePassword(m_profileId, currentPassword, [guard](ProfileError error) {
            if (guard) {
                guard->finishOperation(error);
                if (error == ProfileError::None) {
                    emit guard->passwordChangeSucceeded();
                }
            }
        });
    }

    void ProfileEditorController::requestDeletion(const QString &currentPassword) {
        if (m_busy) {
            return;
        }
        if (!canDelete()) {
            setError(profileErrorMessage(ProfileError::LastProfile));
            return;
        }
        setBusy(true);
        QPointer<ProfileEditorController> guard(this);
        const auto proceed = [guard] {
            if (!guard) {
                return;
            }
            guard->setBusy(false);
            guard->m_manager->deleteProfileFromEditor(guard->m_profileId);
        };
        if (protectedProfile()) {
            m_manager->verifyCurrentPassword(m_profileId, currentPassword, [guard, proceed](bool matches) {
                if (!guard) {
                    return;
                }
                if (!matches) {
                    guard->setBusy(false);
                    guard->setError(profileErrorMessage(ProfileError::WrongPassword));
                    return;
                }
                proceed();
            });
        } else {
            proceed();
        }
    }

}

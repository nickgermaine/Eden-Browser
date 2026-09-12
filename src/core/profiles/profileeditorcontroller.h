#pragma once

#include "core/profiles/profileerror.h"

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QUrl>

class QQuickWindow;

namespace eden::core {

    class ProfileManager;

    class ProfileEditorController final : public QObject {
        Q_OBJECT
        Q_PROPERTY(QString profileId READ profileId CONSTANT)
        Q_PROPERTY(QString displayName READ displayName NOTIFY stateChanged)
        Q_PROPERTY(QString avatarUrl READ avatarUrl NOTIFY stateChanged)
        Q_PROPERTY(QColor color READ color NOTIFY stateChanged)
        Q_PROPERTY(bool protectedProfile READ protectedProfile NOTIFY stateChanged)
        Q_PROPERTY(bool canDelete READ canDelete NOTIFY stateChanged)
        Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
        Q_PROPERTY(bool avatarPending READ avatarPending NOTIFY busyChanged)
        Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
        Q_PROPERTY(int activeDownloadCount READ activeDownloadCount NOTIFY stateChanged)

      public:
        ProfileEditorController(ProfileManager *manager, const QString &profileId, QObject *parent = nullptr);

        QString profileId() const;
        QString displayName() const;
        QString avatarUrl() const;
        QColor color() const;
        bool protectedProfile() const;
        bool canDelete() const;
        bool busy() const;
        bool avatarPending() const;
        QString errorMessage() const;
        int activeDownloadCount() const;

        QQuickWindow *window() const;
        void setWindow(QQuickWindow *window);

        Q_INVOKABLE void rename(const QString &displayName);
        Q_INVOKABLE void cycleColor();
        Q_INVOKABLE void chooseAvatar(const QUrl &fileUrl);
        Q_INVOKABLE void removeAvatar();
        Q_INVOKABLE void
        setPassword(const QString &currentPassword, const QString &nextPassword, const QString &confirmation);
        Q_INVOKABLE void removePassword(const QString &currentPassword);
        Q_INVOKABLE void requestDeletion(const QString &currentPassword);
        Q_INVOKABLE void clearError();

      signals:
        void stateChanged();
        void busyChanged();
        void errorMessageChanged();
        void passwordChangeSucceeded();
        void closeRequested();

      private:
        void finishOperation(ProfileError error);
        void setBusy(bool busy);
        void setError(const QString &message);

        ProfileManager *m_manager;
        QString m_profileId;
        QPointer<QQuickWindow> m_window;
        QString m_errorMessage;
        bool m_busy = false;
        bool m_avatarPending = false;
    };

}

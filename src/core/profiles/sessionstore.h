#pragma once

#include "core/profiles/profileerror.h"

#include <QJsonObject>
#include <QObject>
#include <QTimer>

namespace eden::passwords {
    class CredentialVault;
}

namespace eden::core {

    class SessionStore final : public QObject {
        Q_OBJECT
        Q_PROPERTY(QString errorCode READ errorCode NOTIFY errorChanged)
        Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorChanged)

      public:
        explicit SessionStore(QString sessionPath, QObject *parent = nullptr);

        void setVault(passwords::CredentialVault *vault);
        QJsonObject load();
        bool write(const QJsonObject &session);
        void requestSave();
        void cancelPendingSave();
        bool savePending() const;
        ProfileError error() const;
        QString errorCode() const;
        QString errorMessage() const;

      signals:
        void saveRequested();
        void errorChanged();

      private:
        enum class RestoreState { Unchecked, Ready, Failed };

        bool writeSession(const QJsonObject &session);
        bool setError(ProfileError error);

        QString m_sessionPath;
        passwords::CredentialVault *m_vault = nullptr;
        QTimer m_saveTimer;
        RestoreState m_restoreState = RestoreState::Unchecked;
        ProfileError m_error = ProfileError::None;
    };

}

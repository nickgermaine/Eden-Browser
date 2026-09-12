#include "core/profiles/sessionstore.h"

#include "core/profiles/profilepaths.h"
#include "passwords/credentialvault.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>

namespace eden::core {

    static const QByteArray sessionHeader = QByteArrayLiteral("EDEN-SESSION-1\n");

    SessionStore::SessionStore(QString sessionPath, QObject *parent)
        : QObject(parent),
          m_sessionPath(std::move(sessionPath)) {
        m_saveTimer.setSingleShot(true);
        m_saveTimer.setInterval(1000);
        connect(&m_saveTimer, &QTimer::timeout, this, &SessionStore::saveRequested);
    }

    void SessionStore::setVault(passwords::CredentialVault *vault) {
        m_vault = vault;
    }

    QJsonObject SessionStore::load() {
        m_saveTimer.stop();
        m_restoreState = RestoreState::Failed;
        QFile file(m_sessionPath);
        if (!file.open(QIODevice::ReadOnly)) {
            const QFileInfo info(m_sessionPath);
            if (info.exists() || info.isSymLink()) {
                setError(ProfileError::SessionReadFailed);
            } else {
                m_restoreState = RestoreState::Ready;
                setError(ProfileError::None);
            }
            return {};
        }
        const QByteArray stored = file.readAll();
        if (file.error() != QFileDevice::NoError) {
            setError(ProfileError::SessionReadFailed);
            return {};
        }
        file.close();
        const bool encrypted = stored.startsWith(sessionHeader);
        QByteArray plain = stored;
        if (encrypted) {
            if (!m_vault || !m_vault->available()) {
                setError(ProfileError::SessionReadFailed);
                return {};
            }
            plain = m_vault->openData(stored.sliced(sessionHeader.size()), QByteArrayLiteral("session"));
        }
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(plain, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            setError(ProfileError::SessionInvalid);
            return {};
        }
        const QJsonObject session = document.object();
        if (!encrypted && !writeSession(session)) {
            return {};
        }
        m_restoreState = RestoreState::Ready;
        setError(ProfileError::None);
        return session;
    }

    bool SessionStore::write(const QJsonObject &session) {
        if (m_restoreState == RestoreState::Unchecked) {
            load();
        }
        if (m_restoreState != RestoreState::Ready) {
            return false;
        }
        return writeSession(session);
    }

    bool SessionStore::writeSession(const QJsonObject &session) {
        const QByteArray plain = QJsonDocument(session).toJson(QJsonDocument::Compact);
        if (!m_vault || !m_vault->available()) {
            return setError(ProfileError::SessionWriteFailed);
        }
        const QByteArray sealed = m_vault->sealData(plain, QByteArrayLiteral("session"));
        if (sealed.isEmpty()) {
            return setError(ProfileError::SessionWriteFailed);
        }
        QSaveFile file(m_sessionPath);
        if (!file.open(QIODevice::WriteOnly) || file.write(sessionHeader) != sessionHeader.size() ||
            file.write(sealed) != sealed.size() || !file.commit() || !ProfilePaths::restrictFile(m_sessionPath)) {
            return setError(ProfileError::SessionWriteFailed);
        }
        return setError(ProfileError::None);
    }

    void SessionStore::requestSave() {
        if (m_restoreState != RestoreState::Failed) {
            m_saveTimer.start();
        }
    }

    void SessionStore::cancelPendingSave() {
        m_saveTimer.stop();
    }

    bool SessionStore::savePending() const {
        return m_saveTimer.isActive();
    }

    ProfileError SessionStore::error() const {
        return m_error;
    }

    QString SessionStore::errorCode() const {
        return m_error == ProfileError::None ? QString() : profileErrorCode(m_error);
    }

    QString SessionStore::errorMessage() const {
        return profileErrorMessage(m_error);
    }

    bool SessionStore::setError(ProfileError error) {
        if (m_error != error) {
            m_error = error;
            emit errorChanged();
        }
        return error == ProfileError::None;
    }

}

#include "engine/engineprofile.h"

namespace eden::engine {

    EngineProfile::EngineProfile(const EngineProfileParameters &parameters, QObject *parent)
        : QObject(parent),
          m_parameters(parameters) {}

    EngineProfile::~EngineProfile() = default;

    bool EngineProfile::isPrivate() const {
        return m_parameters.privateProfile;
    }

    const QString &EngineProfile::profileId() const {
        return m_parameters.profileId;
    }

    Backend EngineProfile::backend() const {
        return m_parameters.backend;
    }

    const EngineProfileParameters &EngineProfile::parameters() const {
        return m_parameters;
    }

    bool EngineProfile::supportsPortableCookies() const {
        return false;
    }

    void EngineProfile::exportPortableCookies(CookieSnapshotCallback callback) {
        if (callback) {
            CookieSnapshotResult result;
            result.errorCode = QStringLiteral("unsupported");
            QMetaObject::invokeMethod(
                const_cast<EngineProfile *>(this),
                [callback = std::move(callback), result = std::move(result)] { callback(result); },
                Qt::QueuedConnection
            );
        }
    }

    void EngineProfile::replacePortableCookies(const QList<PortableCookie> &cookies, CookieReplaceCallback callback) {
        Q_UNUSED(cookies)
        if (callback) {
            CookieReplaceResult result;
            result.errorCode = QStringLiteral("unsupported");
            QMetaObject::invokeMethod(
                this,
                [callback = std::move(callback), result = std::move(result)] { callback(result); },
                Qt::QueuedConnection
            );
        }
    }

    void EngineProfile::flushStorage(std::function<void()> completion) {
        if (completion) {
            QMetaObject::invokeMethod(this, std::move(completion), Qt::QueuedConnection);
        }
    }

}

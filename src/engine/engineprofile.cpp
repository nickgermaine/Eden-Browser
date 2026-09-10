#include "engine/engineprofile.h"

#include <atomic>
#include <limits>

namespace eden::engine {

    static quint64 nextDownloadPrefix() {
        static std::atomic<quint64> next{1};
        const quint64 prefix = next.fetch_add(1, std::memory_order_relaxed);
        if (prefix > std::numeric_limits<quint32>::max()) {
            qFatal("Too many engine profiles were created");
        }
        return prefix << 32;
    }

    EngineProfile::EngineProfile(const EngineProfileParameters &parameters, QObject *parent)
        : QObject(parent),
          m_parameters(parameters),
          m_downloadPrefix(nextDownloadPrefix()) {}

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

    quint64 EngineProfile::downloadIdentifier(quint32 nativeId) const {
        return m_downloadPrefix | nativeId;
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

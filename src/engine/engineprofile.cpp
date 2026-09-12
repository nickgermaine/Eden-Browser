#include "engine/engineprofile.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <atomic>
#include <limits>
#include <mutex>

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

    std::shared_ptr<const QString>
    EngineProfile::reserveDownloadPath(const QString &directory, const QString &fileName) {
        struct Reservations {
            std::mutex mutex;
            QSet<QString> paths;
        };
        static const auto reservations = std::make_shared<Reservations>();
        const auto registry = reservations;
        auto destination = std::shared_ptr<QString>(new QString, [registry](QString *path) {
            const std::lock_guard lock(registry->mutex);
            registry->paths.remove(*path);
            delete path;
        });
        QString name = fileName.trimmed();
        for (QChar &character : name) {
            if (character.unicode() < 32 || character.unicode() == 127 || character == '/' || character == '\\') {
                character = '_';
            }
        }
        if (name.isEmpty() || name == "." || name == "..") {
            name = "download";
        }
        const QFileInfo nameInfo(name);
        const QString suffix = nameInfo.suffix();
        const QString base = nameInfo.completeBaseName().isEmpty() ? QString("download") : nameInfo.completeBaseName();
        const QDir targetDirectory(QDir::cleanPath(QDir(directory).absolutePath()));
        const std::lock_guard lock(registry->mutex);
        QString candidate = targetDirectory.absoluteFilePath(name);
        for (quint64 sequence = 1; registry->paths.contains(candidate) || QFileInfo::exists(candidate) ||
                                   QFileInfo(candidate).isSymbolicLink();
             ++sequence) {
            const QString unique = suffix.isEmpty() ? QString("%1 (%2)").arg(base).arg(sequence)
                                                    : QString("%1 (%2).%3").arg(base).arg(sequence).arg(suffix);
            candidate = targetDirectory.absoluteFilePath(unique);
        }
        *destination = QDir::cleanPath(candidate);
        registry->paths.insert(*destination);
        return destination;
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

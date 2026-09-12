#include "core/profiles/profilepaths.h"

#include <QDir>
#include <QFileDevice>
#include <QFileInfo>
#include <QStandardPaths>

namespace eden::core {

    static QString joined(const QString &base, const QString &relative) {
        return base + QLatin1Char('/') + relative;
    }

    ProfilePaths::Roots ProfilePaths::standardRoots() {
        return Roots{
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation),
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation),
        };
    }

    QString ProfilePaths::registryDatabasePath(const Roots &roots) {
        return joined(roots.dataRoot, QStringLiteral("profiles.sqlite"));
    }

    QString ProfilePaths::registryLockPath(const Roots &roots) {
        return joined(roots.dataRoot, QStringLiteral("profiles.sqlite.lock"));
    }

    QString ProfilePaths::profilesDirectory(const Roots &roots) {
        return joined(roots.dataRoot, QStringLiteral("profiles"));
    }

    QString ProfilePaths::engineDataDirectory(const Roots &roots) {
        return joined(roots.dataRoot, QStringLiteral("engine-data"));
    }

    QString ProfilePaths::engineDataDirectory(const Roots &roots, const QString &backendId) {
        return joined(engineDataDirectory(roots), backendId);
    }

    QString ProfilePaths::profileCacheDirectory(const Roots &roots) {
        return joined(roots.cacheRoot, QStringLiteral("profiles"));
    }

    QStringList ProfilePaths::deletionStagingParents(const Roots &roots, const QStringList &backendIds) {
        QStringList parents;
        parents.reserve(backendIds.size() + 2);
        parents.append(profilesDirectory(roots));
        for (const QString &backendId : backendIds) {
            parents.append(joined(engineDataDirectory(roots, backendId), QStringLiteral("profiles")));
        }
        parents.append(profileCacheDirectory(roots));
        return parents;
    }

    bool ProfilePaths::isDeletionStagingName(const QString &directoryName) {
        return directoryName.startsWith(QLatin1String(".deleting-"));
    }

    QString ProfilePaths::deletionStagingName(const ProfileId &id, int ordinal) {
        return QStringLiteral(".deleting-%1-%2").arg(id.toString()).arg(ordinal);
    }

    ProfilePaths::ProfilePaths(const Roots &roots, const ProfileId &id)
        : m_roots(roots),
          m_id(id) {}

    bool ProfilePaths::isValid() const {
        return m_id.isValid() && !m_roots.dataRoot.isEmpty() && !m_roots.cacheRoot.isEmpty();
    }

    const ProfileId &ProfilePaths::profileId() const {
        return m_id;
    }

    const ProfilePaths::Roots &ProfilePaths::roots() const {
        return m_roots;
    }

    QString ProfilePaths::dataDirectory() const {
        return joined(profilesDirectory(m_roots), m_id.toString());
    }

    QString ProfilePaths::cacheDirectory() const {
        return joined(profileCacheDirectory(m_roots), m_id.toString());
    }

    QString ProfilePaths::avatarPath() const {
        return joined(dataDirectory(), QStringLiteral("avatar.png"));
    }

    QString ProfilePaths::databasePath() const {
        return joined(dataDirectory(), QStringLiteral("eden.sqlite"));
    }

    QString ProfilePaths::settingsPath() const {
        return joined(dataDirectory(), QStringLiteral("settings.ini"));
    }

    QString ProfilePaths::sessionPath() const {
        return joined(dataDirectory(), QStringLiteral("session.json"));
    }

    QString ProfilePaths::vaultPath() const {
        return joined(dataDirectory(), QStringLiteral("vault.sqlite"));
    }

    QString ProfilePaths::extensionsDirectory() const {
        return joined(dataDirectory(), QStringLiteral("extensions"));
    }

    QString ProfilePaths::engineDataDirectory(const QString &backendId) const {
        return joined(joined(engineDataDirectory(m_roots, backendId), QStringLiteral("profiles")), m_id.toString());
    }

    QString ProfilePaths::engineCacheDirectory(const QString &backendId) const {
        return joined(cacheDirectory(), backendId);
    }

    QStringList ProfilePaths::ownedDirectories(const QStringList &backendIds) const {
        QStringList directories;
        directories.reserve(backendIds.size() + 2);
        directories.append(dataDirectory());
        for (const QString &backendId : backendIds) {
            directories.append(engineDataDirectory(backendId));
        }
        directories.append(cacheDirectory());
        return directories;
    }

    static bool createRestrictedDirectory(const QString &path) {
        if (!QDir().mkpath(path)) {
            return false;
        }
        return ProfilePaths::restrictDirectory(path);
    }

    bool ProfilePaths::ensureBaseDirectories() const {
        if (!isValid()) {
            return false;
        }
        return createRestrictedDirectory(dataDirectory()) && createRestrictedDirectory(extensionsDirectory()) &&
               createRestrictedDirectory(cacheDirectory());
    }

    bool ProfilePaths::ensureEngineDirectories(const QString &backendId) const {
        if (!isValid() || backendId.isEmpty()) {
            return false;
        }
        return createRestrictedDirectory(engineDataDirectory(backendId)) &&
               createRestrictedDirectory(engineCacheDirectory(backendId));
    }

    bool ProfilePaths::verifyRestrictedPermissions() const {
        const QFileInfo info(dataDirectory());
        if (!info.exists() || !info.isDir()) {
            return false;
        }
        if (info.isSymLink()) {
            return false;
        }
        return info.isReadable() && info.isWritable() && info.isExecutable();
    }

    bool ProfilePaths::contains(const QString &path) const {
        const QString cleaned = QDir::cleanPath(path);
        if (cleaned.contains(QLatin1String(".."))) {
            return false;
        }
        const QString dataPrefix = QDir::cleanPath(dataDirectory()) + QLatin1Char('/');
        const QString cachePrefix = QDir::cleanPath(cacheDirectory()) + QLatin1Char('/');
        return cleaned.startsWith(dataPrefix) || cleaned.startsWith(cachePrefix) ||
               cleaned == QDir::cleanPath(dataDirectory()) || cleaned == QDir::cleanPath(cacheDirectory());
    }

    bool ProfilePaths::restrictDirectory(const QString &path) {
        return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }

    bool ProfilePaths::restrictFile(const QString &path) {
        return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }

}

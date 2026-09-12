#pragma once

#include "core/profiles/profileid.h"

#include <QString>
#include <QStringList>

namespace eden::core {

    class ProfilePaths {
      public:
        struct Roots {
            QString dataRoot;
            QString cacheRoot;
        };

        static Roots standardRoots();
        static QString registryDatabasePath(const Roots &roots);
        static QString registryLockPath(const Roots &roots);
        static QString profilesDirectory(const Roots &roots);
        static QString engineDataDirectory(const Roots &roots);
        static QString engineDataDirectory(const Roots &roots, const QString &backendId);
        static QString profileCacheDirectory(const Roots &roots);
        static QStringList deletionStagingParents(const Roots &roots, const QStringList &backendIds);
        static bool isDeletionStagingName(const QString &directoryName);
        static QString deletionStagingName(const ProfileId &id, int ordinal);

        ProfilePaths() = default;
        ProfilePaths(const Roots &roots, const ProfileId &id);

        bool isValid() const;
        const ProfileId &profileId() const;
        const Roots &roots() const;

        QString dataDirectory() const;
        QString cacheDirectory() const;
        QString avatarPath() const;
        QString databasePath() const;
        QString settingsPath() const;
        QString sessionPath() const;
        QString vaultPath() const;
        QString extensionsDirectory() const;
        QString engineDataDirectory(const QString &backendId) const;
        QString engineCacheDirectory(const QString &backendId) const;
        QStringList ownedDirectories(const QStringList &backendIds) const;

        bool ensureBaseDirectories() const;
        bool ensureEngineDirectories(const QString &backendId) const;
        bool verifyRestrictedPermissions() const;
        bool contains(const QString &path) const;

        static bool restrictDirectory(const QString &path);
        static bool restrictFile(const QString &path);

      private:
        Roots m_roots;
        ProfileId m_id;
    };

}

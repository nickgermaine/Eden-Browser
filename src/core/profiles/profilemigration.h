#pragma once

#include "core/profiles/profileerror.h"
#include "core/profiles/profilepaths.h"

#include <QString>

namespace eden::core {

    class ProfileMigration {
      public:
        struct Inventory {
            QString legacyDatabasePath;
            QString legacySessionPath;
            QString legacyWebEngineDataPath;
            QString legacyWebEngineCachePath;
            QString legacyCefRootPath;
            QString legacyConfigPath;

            bool any() const;
        };

        static Inventory inventoryLegacyData(const ProfilePaths::Roots &roots);
        static bool journalExists(const ProfilePaths::Roots &roots);
        static ProfileError resume(const ProfilePaths &destination);
        static ProfileError migrate(const Inventory &inventory, const ProfilePaths &destination);
        static QString journalPath(const ProfilePaths::Roots &roots);
        static bool profileDirectoriesPresent(const ProfilePaths::Roots &roots);
    };

}

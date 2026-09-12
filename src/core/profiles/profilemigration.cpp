#include "core/profiles/profilemigration.h"
#include "core/profiles/enginestorage.h"

#include "core/profiles/profileid.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

namespace eden::core {

    bool ProfileMigration::Inventory::any() const {
        return !legacyDatabasePath.isEmpty() || !legacySessionPath.isEmpty() || !legacyWebEngineDataPath.isEmpty() ||
               !legacyWebEngineCachePath.isEmpty() || !legacyCefRootPath.isEmpty();
    }

    static QString presentPath(const QString &path) {
        return QFileInfo::exists(path) ? path : QString();
    }

    ProfileMigration::Inventory ProfileMigration::inventoryLegacyData(const ProfilePaths::Roots &roots) {
        Inventory inventory;
        inventory.legacyDatabasePath = presentPath(roots.dataRoot + QStringLiteral("/eden.db"));
        inventory.legacySessionPath = presentPath(roots.dataRoot + QStringLiteral("/session.json"));
        inventory.legacyWebEngineDataPath = presentPath(roots.dataRoot + QStringLiteral("/webengine"));
        inventory.legacyWebEngineCachePath = presentPath(roots.cacheRoot + QStringLiteral("/webengine"));
        const QString configuredDataHome = qEnvironmentVariable("XDG_DATA_HOME");
        const QString dataHome =
            configuredDataHome.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/share") : configuredDataHome;
        inventory.legacyCefRootPath = presentPath(dataHome + QStringLiteral("/eden/cef"));
        inventory.legacyConfigPath = presentPath(
            QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/eden/eden.conf")
        );
        return inventory;
    }

    QString ProfileMigration::journalPath(const ProfilePaths::Roots &roots) {
        return roots.dataRoot + QStringLiteral("/migration-journal.json");
    }

    bool ProfileMigration::journalExists(const ProfilePaths::Roots &roots) {
        return QFileInfo::exists(journalPath(roots));
    }

    bool ProfileMigration::profileDirectoriesPresent(const ProfilePaths::Roots &roots) {
        QDirIterator iterator(ProfilePaths::profilesDirectory(roots), QDir::Dirs | QDir::NoDotAndDotDot);
        while (iterator.hasNext()) {
            const QString name = QFileInfo(iterator.next()).fileName();
            if (ProfileId::isCanonical(name)) {
                return true;
            }
        }
        return false;
    }

    struct MigrationStep {
        QString source;
        QString destination;
    };

    static bool writeJournal(const QString &path, const QList<MigrationStep> &steps) {
        QJsonArray operations;
        for (const MigrationStep &step : steps) {
            QJsonObject operation;
            operation.insert(QStringLiteral("source"), step.source);
            operation.insert(QStringLiteral("destination"), step.destination);
            operation.insert(QStringLiteral("operation"), QStringLiteral("move"));
            operations.append(operation);
        }
        QJsonObject journal;
        journal.insert(QStringLiteral("version"), 1);
        journal.insert(QStringLiteral("operations"), operations);
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            return false;
        }
        file.write(QJsonDocument(journal).toJson(QJsonDocument::Compact));
        return file.commit();
    }

    static bool moveEntry(const QString &source, const QString &destination) {
        if (!QFileInfo::exists(source)) {
            return true;
        }
        if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
            return false;
        }
        return EngineStorage::migrateLegacyPath(source, destination);
    }

    static bool validateSqliteFile(const QString &path) {
        if (!QFileInfo::exists(path)) {
            return true;
        }
        const QString connectionName =
            QStringLiteral("eden-migration-check-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        bool valid = false;
        {
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            database.setDatabaseName(path);
            if (database.open()) {
                QSqlQuery query(database);
                valid = query.exec(QStringLiteral("PRAGMA quick_check")) && query.next() &&
                        query.value(0).toString() == QLatin1String("ok");
                database.close();
            }
        }
        QSqlDatabase::removeDatabase(connectionName);
        return valid;
    }

    static bool validateJsonFile(const QString &path) {
        if (!QFileInfo::exists(path)) {
            return true;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return false;
        }
        QJsonParseError parseError{};
        QJsonDocument::fromJson(file.readAll(), &parseError);
        return parseError.error == QJsonParseError::NoError;
    }

    static void migrateProfileOwnedSettings(const QString &legacyConfigPath, const QString &profileSettingsPath) {
        if (legacyConfigPath.isEmpty() || QFileInfo::exists(profileSettingsPath)) {
            return;
        }
        QSettings legacy(legacyConfigPath, QSettings::IniFormat);
        QSettings profile(profileSettingsPath, QSettings::IniFormat);
        const auto copyKey = [&legacy, &profile](const QString &key) {
            if (legacy.contains(key)) {
                profile.setValue(key, legacy.value(key));
            }
        };
        copyKey(QStringLiteral("engine/default"));
        copyKey(QStringLiteral("search/name"));
        copyKey(QStringLiteral("search/url"));
        copyKey(QStringLiteral("search/suggestions"));
        copyKey(QStringLiteral("appearance/tabLayout"));
        profile.sync();
        legacy.remove(QStringLiteral("engine/default"));
        legacy.remove(QStringLiteral("search/name"));
        legacy.remove(QStringLiteral("search/url"));
        legacy.remove(QStringLiteral("search/suggestions"));
        legacy.sync();
        ProfilePaths::restrictFile(profileSettingsPath);
    }

    ProfileError ProfileMigration::resume(const ProfilePaths &destination) {
        QFile journal(journalPath(destination.roots()));
        if (!journal.exists()) {
            return ProfileError::None;
        }
        if (!journal.open(QIODevice::ReadOnly) || journal.size() > 1024 * 1024) {
            return ProfileError::MigrationFailed;
        }
        const QJsonObject saved = QJsonDocument::fromJson(journal.readAll()).object();
        if (saved.value("version").toInt() != 1 || !saved.value("operations").isArray()) {
            return ProfileError::MigrationFailed;
        }
        const Inventory inventory = inventoryLegacyData(destination.roots());
        const QList<MigrationStep> allowed{
            {inventory.legacyDatabasePath, destination.databasePath()},
            {inventory.legacySessionPath, destination.sessionPath()},
            {inventory.legacyWebEngineDataPath, destination.engineDataDirectory("qtwebengine")},
            {inventory.legacyWebEngineCachePath, destination.engineCacheDirectory("qtwebengine")},
            {inventory.legacyCefRootPath.isEmpty() ? QString() : inventory.legacyCefRootPath + "/default",
             destination.engineDataDirectory("cef")}
        };
        for (const QJsonValue &entry : saved.value("operations").toArray()) {
            const QJsonObject operation = entry.toObject();
            const QString target = operation.value("destination").toString();
            const QString source = operation.value("source").toString();
            bool valid = false;
            for (const auto &step : allowed) {
                if (target == step.destination &&
                    (source == step.source ||
                     (step.source.isEmpty() && !QFileInfo::exists(source) && QFileInfo::exists(target)))) {
                    valid = true;
                    break;
                }
            }
            if (!valid || operation.value("operation").toString() != "move" || !moveEntry(source, target)) {
                return ProfileError::MigrationFailed;
            }
        }
        if (!validateSqliteFile(destination.databasePath()) || !validateJsonFile(destination.sessionPath())) {
            return ProfileError::MigrationFailed;
        }
        migrateProfileOwnedSettings(inventory.legacyConfigPath, destination.settingsPath());
        return ProfileError::None;
    }

    ProfileError ProfileMigration::migrate(const Inventory &inventory, const ProfilePaths &destination) {
        if (!destination.isValid() || !destination.ensureBaseDirectories()) {
            return ProfileError::DirectoryCreateFailed;
        }
        QList<MigrationStep> steps;
        const auto planMove = [&steps](const QString &source, const QString &target) {
            if (!source.isEmpty()) {
                steps.append(MigrationStep{source, target});
            }
        };
        planMove(inventory.legacyDatabasePath, destination.databasePath());
        planMove(inventory.legacySessionPath, destination.sessionPath());
        planMove(inventory.legacyWebEngineDataPath, destination.engineDataDirectory(QStringLiteral("qtwebengine")));
        planMove(inventory.legacyWebEngineCachePath, destination.engineCacheDirectory(QStringLiteral("qtwebengine")));
        if (!inventory.legacyCefRootPath.isEmpty()) {
            planMove(
                inventory.legacyCefRootPath + QStringLiteral("/default"),
                destination.engineDataDirectory(QStringLiteral("cef"))
            );
        }
        const QString journal = journalPath(destination.roots());
        if (!steps.isEmpty() && !writeJournal(journal, steps)) {
            return ProfileError::MigrationFailed;
        }
        for (const MigrationStep &step : steps) {
            if (!moveEntry(step.source, step.destination)) {
                return ProfileError::MigrationFailed;
            }
        }
        if (!validateSqliteFile(destination.databasePath()) || !validateJsonFile(destination.sessionPath())) {
            return ProfileError::MigrationFailed;
        }
        migrateProfileOwnedSettings(inventory.legacyConfigPath, destination.settingsPath());
        if (!inventory.legacyCefRootPath.isEmpty()) {
            QDir legacyCefRoot(inventory.legacyCefRootPath);
            if (legacyCefRoot.exists() && legacyCefRoot.isEmpty()) {
                legacyCefRoot.removeRecursively();
            }
        }
        return ProfileError::None;
    }

}

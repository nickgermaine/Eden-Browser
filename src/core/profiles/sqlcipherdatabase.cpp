#include "core/profiles/sqlcipherdatabase.h"

#include "core/profiles/profilepaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <sqlcipher/sqlite3.h>

namespace eden::core {

    bool SqlCipherDatabase::initialize(const QString &path, const QByteArray &key) {
        if (key.size() != 32 || !QDir().mkpath(QFileInfo(path).absolutePath())) {
            return false;
        }
        if (QFileInfo::exists(path)) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                return false;
            }
            const qint64 size = file.size();
            const bool plaintext = file.read(16).startsWith("SQLite format 3");
            file.close();
            if (plaintext) {
                if (!migratePlaintext(path, key)) {
                    return false;
                }
            } else if (size > 0) {
                if (size < 4096) {
                    return false;
                }
                sqlite3 *existing = open(path, key, false);
                const bool encrypted = existing && readable(existing);
                if (existing) {
                    sqlite3_close_v2(existing);
                }
                if (!encrypted) {
                    return false;
                }
            }
        }
        sqlite3 *database = open(path, key, true);
        if (!database) {
            return false;
        }
        const bool initialized =
            readable(database) && execute(database, "PRAGMA journal_mode=WAL") &&
            execute(database, "PRAGMA secure_delete=ON") && execute(database, "PRAGMA busy_timeout=2500") &&
            execute(
                database,
                "CREATE TABLE IF NOT EXISTS migrations("
                "version INTEGER PRIMARY KEY,applied_at INTEGER NOT NULL)"
            ) &&
            execute(
                database,
                "CREATE TABLE IF NOT EXISTS history("
                "url TEXT PRIMARY KEY,title TEXT NOT NULL,visited_at INTEGER NOT NULL,"
                "visit_count INTEGER NOT NULL DEFAULT 1)"
            ) &&
            execute(database, "CREATE INDEX IF NOT EXISTS history_visited_at ON history(visited_at DESC,url ASC)") &&
            execute(
                database,
                "CREATE TABLE IF NOT EXISTS bookmarks("
                "url TEXT PRIMARY KEY,title TEXT NOT NULL,folder TEXT NOT NULL DEFAULT '',"
                "created_at INTEGER NOT NULL)"
            ) &&
            execute(
                database,
                "CREATE TABLE IF NOT EXISTS site_permissions("
                "origin TEXT NOT NULL,permission TEXT NOT NULL,verdict INTEGER NOT NULL DEFAULT 0,"
                "requested_at INTEGER NOT NULL,updated_at INTEGER NOT NULL,"
                "PRIMARY KEY(origin,permission))"
            ) &&
            execute(database, "CREATE INDEX IF NOT EXISTS site_permissions_origin ON site_permissions(origin)") &&
            execute(
                database,
                "CREATE TABLE IF NOT EXISTS downloads("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,file_name TEXT NOT NULL,source_url TEXT NOT NULL,"
                "target_path TEXT NOT NULL,received_bytes INTEGER NOT NULL,total_bytes INTEGER NOT NULL,"
                "state TEXT NOT NULL,started_at INTEGER NOT NULL,updated_at INTEGER NOT NULL)"
            ) &&
            execute(database, "CREATE INDEX IF NOT EXISTS downloads_started_at ON downloads(started_at DESC)") &&
            execute(
                database,
                "INSERT OR IGNORE INTO migrations(version,applied_at) "
                "VALUES(1,unixepoch('subsec')*1000)"
            ) &&
            execute(
                database,
                "INSERT OR IGNORE INTO migrations(version,applied_at) "
                "VALUES(2,unixepoch('subsec')*1000)"
            );
        sqlite3_close_v2(database);
        if (initialized) {
            restrictFiles(path);
        }
        return initialized;
    }

    sqlite3 *SqlCipherDatabase::open(const QString &path, const QByteArray &key, bool create) {
        if (key.size() != 32) {
            return nullptr;
        }
        sqlite3 *database = nullptr;
        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | (create ? SQLITE_OPEN_CREATE : 0);
        if (sqlite3_open_v2(QFile::encodeName(path).constData(), &database, flags, nullptr) != SQLITE_OK) {
            if (database) {
                sqlite3_close_v2(database);
            }
            return nullptr;
        }
        const QByteArray keyStatement = QByteArrayLiteral("PRAGMA key=\"x'") + key.toHex() + QByteArrayLiteral("'\"");
        if (!execute(database, keyStatement) || !execute(database, "PRAGMA cipher_memory_security=ON") ||
            !execute(database, "PRAGMA busy_timeout=2500")) {
            sqlite3_close_v2(database);
            return nullptr;
        }
        return database;
    }

    bool SqlCipherDatabase::execute(sqlite3 *database, const QByteArray &sql) {
        if (!database) {
            return false;
        }
        char *message = nullptr;
        const int result = sqlite3_exec(database, sql.constData(), nullptr, nullptr, &message);
        if (message) {
            sqlite3_free(message);
        }
        return result == SQLITE_OK;
    }

    void SqlCipherDatabase::restrictFiles(const QString &path) {
        ProfilePaths::restrictFile(path);
        ProfilePaths::restrictFile(path + QStringLiteral("-wal"));
        ProfilePaths::restrictFile(path + QStringLiteral("-shm"));
    }

    bool SqlCipherDatabase::migratePlaintext(const QString &path, const QByteArray &key) {
        sqlite3 *plaintext = nullptr;
        if (sqlite3_open_v2(
                QFile::encodeName(path).constData(),
                &plaintext,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                nullptr
            ) != SQLITE_OK ||
            !readable(plaintext)) {
            if (plaintext) {
                sqlite3_close_v2(plaintext);
            }
            return false;
        }
        execute(plaintext, "PRAGMA wal_checkpoint(TRUNCATE)");
        execute(plaintext, "PRAGMA journal_mode=DELETE");
        const QString temporaryPath = path + QStringLiteral(".encrypting");
        const QString previousPath = path + QStringLiteral(".plaintext-migration");
        QFile::remove(temporaryPath);
        discard(previousPath);
        char *attach = sqlite3_mprintf(
            "ATTACH DATABASE %Q AS encrypted KEY \"x'%q'\"",
            QFile::encodeName(temporaryPath).constData(),
            key.toHex().constData()
        );
        const bool exported = attach && execute(plaintext, QByteArray(attach)) &&
                              execute(plaintext, "SELECT sqlcipher_export('encrypted')") &&
                              execute(plaintext, "DETACH DATABASE encrypted");
        if (attach) {
            sqlite3_free(attach);
        }
        sqlite3_close_v2(plaintext);
        if (!exported) {
            QFile::remove(temporaryPath);
            return false;
        }
        sqlite3 *verification = open(temporaryPath, key, false);
        const bool valid = verification && readable(verification);
        if (verification) {
            sqlite3_close_v2(verification);
        }
        if (!valid || !QFile::rename(path, previousPath)) {
            QFile::remove(temporaryPath);
            return false;
        }
        if (!QFile::rename(temporaryPath, path)) {
            QFile::rename(previousPath, path);
            return false;
        }
        discard(previousPath);
        discard(path + QStringLiteral("-wal"));
        QFile::remove(path + QStringLiteral("-shm"));
        restrictFiles(path);
        return true;
    }

    bool SqlCipherDatabase::readable(sqlite3 *database) {
        sqlite3_stmt *statement = nullptr;
        if (!database ||
            sqlite3_prepare_v2(database, "SELECT count(*) FROM sqlite_master", -1, &statement, nullptr) != SQLITE_OK) {
            return false;
        }
        const bool result = sqlite3_step(statement) == SQLITE_ROW;
        sqlite3_finalize(statement);
        return result;
    }

    void SqlCipherDatabase::discard(const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadWrite)) {
            QFile::remove(path);
            return;
        }
        const qint64 size = file.size();
        QByteArray zeros(1024 * 1024, '\0');
        file.seek(0);
        qint64 written = 0;
        while (written < size) {
            const qint64 chunk = qMin<qint64>(zeros.size(), size - written);
            const qint64 result = file.write(zeros.constData(), chunk);
            if (result <= 0) {
                break;
            }
            written += result;
        }
        file.flush();
        file.close();
        QFile::remove(path);
    }

}

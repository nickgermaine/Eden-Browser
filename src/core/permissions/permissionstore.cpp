#include "core/permissions/permissionstore.h"

#include "core/profiles/sqlcipherdatabase.h"

#include <QDateTime>
#include <QScopeGuard>

#include <sqlcipher/sqlite3.h>

namespace eden::core {

    PermissionStore::PermissionStore(QString databasePath, QObject *parent)
        : QObject(parent),
          m_databasePath(std::move(databasePath)) {}

    PermissionStore::~PermissionStore() {
        close();
    }

    bool PermissionStore::initialize(const QByteArray &key) {
        if (m_available) {
            return true;
        }
        m_database = SqlCipherDatabase::open(m_databasePath, key, false);
        if (!m_database ||
            !SqlCipherDatabase::execute(
                m_database,
                "CREATE TABLE IF NOT EXISTS site_permissions("
                "origin TEXT NOT NULL,permission TEXT NOT NULL,verdict INTEGER NOT NULL DEFAULT 0,"
                "requested_at INTEGER NOT NULL,updated_at INTEGER NOT NULL,PRIMARY KEY(origin,permission))"
            ) ||
            !SqlCipherDatabase::execute(
                m_database,
                "CREATE INDEX IF NOT EXISTS site_permissions_origin ON site_permissions(origin)"
            ) ||
            !SqlCipherDatabase::execute(
                m_database,
                "UPDATE site_permissions SET verdict=0 WHERE permission='screen sharing' AND verdict=1"
            )) {
            close();
            return false;
        }
        m_available = true;
        emit availableChanged();
        return true;
    }

    bool PermissionStore::available() const {
        return m_available;
    }

    PermissionStore::Verdict PermissionStore::verdict(const QUrl &origin, const QString &permission) const {
        const QUrl normalized = normalizedOrigin(origin);
        const QString canonical = canonicalPermission(permission);
        if (!m_available || normalized.isEmpty() || canonical.isEmpty()) {
            return Ask;
        }
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(
                m_database,
                "SELECT verdict FROM site_permissions WHERE origin=? AND permission=?",
                -1,
                &statement,
                nullptr
            ) != SQLITE_OK) {
            return Ask;
        }
        const auto finalize = qScopeGuard([statement] { sqlite3_finalize(statement); });
        const QByteArray encodedOrigin = normalized.toString().toUtf8();
        const QByteArray encodedPermission = canonical.toUtf8();
        sqlite3_bind_text(statement, 1, encodedOrigin.constData(), encodedOrigin.size(), SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, encodedPermission.constData(), encodedPermission.size(), SQLITE_TRANSIENT);
        if (sqlite3_step(statement) != SQLITE_ROW) {
            return Ask;
        }
        const int stored = sqlite3_column_int(statement, 0);
        return stored == Granted ? Granted : stored == Denied ? Denied : Ask;
    }

    QString PermissionStore::state(const QUrl &origin, const QString &permission) const {
        return verdictName(verdict(origin, permission));
    }

    QVariantList PermissionStore::permissionsForOrigin(const QUrl &origin, bool includeDefaults) const {
        QVariantList result;
        const QUrl normalized = normalizedOrigin(origin);
        if (!m_available || normalized.isEmpty()) {
            return result;
        }
        QHash<QString, Verdict> stored;
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(
                m_database,
                "SELECT permission,verdict FROM site_permissions WHERE origin=? ORDER BY permission",
                -1,
                &statement,
                nullptr
            ) == SQLITE_OK) {
            const QByteArray encodedOrigin = normalized.toString().toUtf8();
            sqlite3_bind_text(statement, 1, encodedOrigin.constData(), encodedOrigin.size(), SQLITE_TRANSIENT);
            while (sqlite3_step(statement) == SQLITE_ROW) {
                const QString permission =
                    QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(statement, 0)));
                const int value = sqlite3_column_int(statement, 1);
                stored.insert(permission, value == Granted ? Granted : value == Denied ? Denied : Ask);
            }
        }
        sqlite3_finalize(statement);
        const QStringList types = includeDefaults ? permissionTypes() : stored.keys();
        for (const QString &permission : types) {
            const Verdict value = stored.value(permission, Ask);
            QVariantMap item;
            item.insert("id", permission);
            item.insert("title", permissionTitle(permission));
            item.insert("state", verdictName(value));
            item.insert("allowed", value == Granted);
            item.insert("requested", stored.contains(permission));
            result.append(item);
        }
        return result;
    }

    QVariantList PermissionStore::sites(const QString &filter) const {
        QVariantList result;
        if (!m_available) {
            return result;
        }
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(
                m_database,
                "SELECT origin,count(*),sum(CASE WHEN verdict=1 THEN 1 ELSE 0 END),"
                "sum(CASE WHEN verdict=2 THEN 1 ELSE 0 END) FROM site_permissions "
                "GROUP BY origin ORDER BY origin COLLATE NOCASE",
                -1,
                &statement,
                nullptr
            ) != SQLITE_OK) {
            return result;
        }
        const auto finalize = qScopeGuard([statement] { sqlite3_finalize(statement); });
        const QString query = filter.trimmed();
        while (sqlite3_step(statement) == SQLITE_ROW) {
            const QString origin = QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(statement, 0)));
            const QUrl url(origin);
            if (!query.isEmpty() && !origin.contains(query, Qt::CaseInsensitive) &&
                !url.host().contains(query, Qt::CaseInsensitive)) {
                continue;
            }
            QVariantMap item;
            item.insert("origin", origin);
            item.insert("host", url.host().isEmpty() ? origin : url.host());
            item.insert("permissionCount", sqlite3_column_int(statement, 1));
            item.insert("allowedCount", sqlite3_column_int(statement, 2));
            item.insert("blockedCount", sqlite3_column_int(statement, 3));
            result.append(item);
        }
        return result;
    }

    bool PermissionStore::setPermission(const QUrl &origin, const QString &permission, bool allowed) {
        return upsert(
            origin,
            permission,
            allowed ? (canonicalPermission(permission) == "screen sharing" ? Ask : Granted) : Denied,
            false
        );
    }

    bool PermissionStore::resetPermission(const QUrl &origin, const QString &permission) {
        return upsert(origin, permission, Ask, false);
    }

    bool PermissionStore::resetOrigin(const QUrl &origin) {
        const QUrl normalized = normalizedOrigin(origin);
        if (!m_available || normalized.isEmpty()) {
            return false;
        }
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(
                m_database,
                "UPDATE site_permissions SET verdict=0,updated_at=? WHERE origin=?",
                -1,
                &statement,
                nullptr
            ) != SQLITE_OK) {
            return false;
        }
        const auto finalize = qScopeGuard([statement] { sqlite3_finalize(statement); });
        const QByteArray encodedOrigin = normalized.toString().toUtf8();
        sqlite3_bind_int64(statement, 1, QDateTime::currentMSecsSinceEpoch());
        sqlite3_bind_text(statement, 2, encodedOrigin.constData(), encodedOrigin.size(), SQLITE_TRANSIENT);
        const bool changed = sqlite3_step(statement) == SQLITE_DONE;
        if (changed) {
            SqlCipherDatabase::restrictFiles(m_databasePath);
            emit permissionsChanged(normalized);
        }
        return changed;
    }

    bool PermissionStore::noteRequested(const QUrl &origin, const QString &permission) {
        return upsert(origin, permission, Ask, true);
    }

    QUrl PermissionStore::normalizedOrigin(const QUrl &origin) {
        if (!origin.isValid() || origin.host().isEmpty() || (origin.scheme() != "http" && origin.scheme() != "https")) {
            return {};
        }
        QUrl normalized;
        normalized.setScheme(origin.scheme().toLower());
        normalized.setHost(origin.host().toLower());
        const int port = origin.port();
        if (port > 0 &&
            !((normalized.scheme() == "https" && port == 443) || (normalized.scheme() == "http" && port == 80))) {
            normalized.setPort(port);
        }
        return normalized;
    }

    QString PermissionStore::canonicalPermission(const QString &permission) {
        const QString value = permission.trimmed().toLower();
        if (value == "geolocation") {
            return "location";
        }
        if (value == "screen audio" || value == "screen capture" || value == "desktop video" ||
            value == "desktop audio and video") {
            return "screen sharing";
        }
        if (value == "clipboard read and write") {
            return "clipboard";
        }
        if (value == "midi") {
            return "midi devices";
        }
        if (value == "audio capture") {
            return "microphone";
        }
        if (value == "video capture" || value == "audio and video capture") {
            return "camera";
        }
        return permissionTypes().contains(value) ? value : QString();
    }

    QString PermissionStore::permissionTitle(const QString &permission) {
        static const QHash<QString, QString> titles = {
            {"notifications", "Notifications"},
            {"location", "Location"},
            {"camera", "Camera"},
            {"microphone", "Microphone"},
            {"screen sharing", "Screen sharing"},
            {"clipboard", "Clipboard"},
            {"autoplay", "Autoplay with sound"},
            {"popups", "Popups and redirects"},
            {"midi devices", "MIDI devices"},
            {"multiple downloads", "Multiple downloads"},
            {"pointer lock", "Pointer lock"},
            {"site storage", "Site storage"},
            {"window management", "Window management"},
            {"files", "File system access"},
            {"local fonts", "Local fonts"},
        };
        return titles.value(permission, permission);
    }

    QStringList PermissionStore::permissionTypes() {
        return {
            "notifications",
            "location",
            "camera",
            "microphone",
            "screen sharing",
            "clipboard",
            "autoplay",
            "popups",
            "midi devices",
            "multiple downloads",
            "pointer lock",
            "site storage",
            "window management",
            "files",
            "local fonts",
        };
    }

    QString PermissionStore::verdictName(Verdict verdict) {
        return verdict == Granted  ? QStringLiteral("allowed")
               : verdict == Denied ? QStringLiteral("blocked")
                                   : QStringLiteral("ask");
    }

    bool PermissionStore::upsert(const QUrl &origin, const QString &permission, Verdict verdict, bool preserveVerdict) {
        const QUrl normalized = normalizedOrigin(origin);
        const QString canonical = canonicalPermission(permission);
        if (!m_available || normalized.isEmpty() || canonical.isEmpty()) {
            return false;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const char *sql = preserveVerdict
                              ? "INSERT INTO site_permissions(origin,permission,verdict,requested_at,updated_at) "
                                "VALUES(?,?,0,?,?) ON CONFLICT(origin,permission) DO UPDATE SET requested_at=?"
                              : "INSERT INTO site_permissions(origin,permission,verdict,requested_at,updated_at) "
                                "VALUES(?,?,?,?,?) ON CONFLICT(origin,permission) DO UPDATE SET "
                                "verdict=excluded.verdict,updated_at=excluded.updated_at";
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(m_database, sql, -1, &statement, nullptr) != SQLITE_OK) {
            return false;
        }
        const auto finalize = qScopeGuard([statement] { sqlite3_finalize(statement); });
        const QByteArray encodedOrigin = normalized.toString().toUtf8();
        const QByteArray encodedPermission = canonical.toUtf8();
        sqlite3_bind_text(statement, 1, encodedOrigin.constData(), encodedOrigin.size(), SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, encodedPermission.constData(), encodedPermission.size(), SQLITE_TRANSIENT);
        if (preserveVerdict) {
            sqlite3_bind_int64(statement, 3, now);
            sqlite3_bind_int64(statement, 4, now);
            sqlite3_bind_int64(statement, 5, now);
        } else {
            sqlite3_bind_int(statement, 3, verdict);
            sqlite3_bind_int64(statement, 4, now);
            sqlite3_bind_int64(statement, 5, now);
        }
        const bool changed = sqlite3_step(statement) == SQLITE_DONE;
        if (changed) {
            SqlCipherDatabase::restrictFiles(m_databasePath);
            emit permissionsChanged(normalized);
        }
        return changed;
    }

    void PermissionStore::close() {
        if (m_database) {
            sqlite3_close_v2(m_database);
            m_database = nullptr;
        }
        if (m_available) {
            m_available = false;
            emit availableChanged();
        }
    }

}

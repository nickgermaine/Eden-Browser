#include "core/favicons/faviconstore.h"
#include "core/profiles/sqlcipherdatabase.h"

#include <QBuffer>
#include <QDateTime>
#include <QImage>

#include <sqlcipher/sqlite3.h>

namespace eden::core {

    static constexpr int storedIconExtent = 48;

    FaviconStore::FaviconStore(QString databasePath, QObject *parent)
        : QObject(parent),
          m_databasePath(std::move(databasePath)) {}

    FaviconStore::~FaviconStore() {
        if (m_database) {
            sqlite3_close_v2(m_database);
        }
    }

    bool FaviconStore::initialize(const QByteArray &key) {
        if (m_database) {
            return true;
        }
        if (m_databasePath.isEmpty() || key.size() != 32) {
            return false;
        }
        m_database = SqlCipherDatabase::open(m_databasePath, key, false);
        if (!m_database) {
            return false;
        }
        if (!SqlCipherDatabase::execute(
                m_database,
                "CREATE TABLE IF NOT EXISTS favicons("
                "host TEXT PRIMARY KEY,icon TEXT NOT NULL,updated_at INTEGER NOT NULL)"
            )) {
            sqlite3_close_v2(m_database);
            m_database = nullptr;
            return false;
        }
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(m_database, "SELECT host,icon FROM favicons", -1, &statement, nullptr) == SQLITE_OK) {
            while (sqlite3_step(statement) == SQLITE_ROW) {
                const QString host = QString::fromUtf8(
                    reinterpret_cast<const char *>(sqlite3_column_text(statement, 0)),
                    sqlite3_column_bytes(statement, 0)
                );
                const QString icon = QString::fromUtf8(
                    reinterpret_cast<const char *>(sqlite3_column_text(statement, 1)),
                    sqlite3_column_bytes(statement, 1)
                );
                m_icons.insert(host, QUrl(icon));
            }
        }
        sqlite3_finalize(statement);
        ++m_revision;
        emit changed();
        return true;
    }

    bool FaviconStore::isInitialized() const {
        return m_database;
    }

    int FaviconStore::revision() const {
        return m_revision;
    }

    QString FaviconStore::keyFor(const QUrl &pageUrl) {
        const QString scheme = pageUrl.scheme();
        if (scheme != "http" && scheme != "https") {
            return {};
        }
        return pageUrl.host().toLower();
    }

    QUrl FaviconStore::normalizedIcon(const QUrl &iconUrl) {
        const QString text = iconUrl.toString();
        if (!text.startsWith("data:image/")) {
            return {};
        }
        const qsizetype marker = text.indexOf(";base64,");
        if (marker < 0) {
            return {};
        }
        QImage image = QImage::fromData(QByteArray::fromBase64(text.sliced(marker + 8).toLatin1()));
        if (image.isNull()) {
            return {};
        }
        if (image.width() > storedIconExtent || image.height() > storedIconExtent) {
            image = image.scaled(storedIconExtent, storedIconExtent, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        QByteArray encoded;
        QBuffer buffer(&encoded);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
            return {};
        }
        return QUrl(QString("data:image/png;base64,%1").arg(QString::fromLatin1(encoded.toBase64())));
    }

    QUrl FaviconStore::iconFor(const QUrl &pageUrl) const {
        const QString key = keyFor(pageUrl);
        return key.isEmpty() ? QUrl() : m_icons.value(key);
    }

    void FaviconStore::record(const QUrl &pageUrl, const QUrl &iconUrl) {
        const QString key = keyFor(pageUrl);
        if (key.isEmpty() || iconUrl.isEmpty()) {
            return;
        }
        const QUrl icon = normalizedIcon(iconUrl);
        if (icon.isEmpty() || m_icons.value(key) == icon) {
            return;
        }
        m_icons.insert(key, icon);
        if (m_database) {
            sqlite3_stmt *statement = nullptr;
            if (sqlite3_prepare_v2(
                    m_database,
                    "INSERT INTO favicons(host,icon,updated_at) VALUES(?,?,?) "
                    "ON CONFLICT(host) DO UPDATE SET icon=excluded.icon,updated_at=excluded.updated_at",
                    -1,
                    &statement,
                    nullptr
                ) == SQLITE_OK) {
                const QByteArray encodedKey = key.toUtf8();
                const QByteArray encodedIcon = icon.toString().toUtf8();
                sqlite3_bind_text(statement, 1, encodedKey.constData(), encodedKey.size(), SQLITE_TRANSIENT);
                sqlite3_bind_text(statement, 2, encodedIcon.constData(), encodedIcon.size(), SQLITE_TRANSIENT);
                sqlite3_bind_int64(statement, 3, QDateTime::currentMSecsSinceEpoch());
                if (sqlite3_step(statement) == SQLITE_DONE) {
                    SqlCipherDatabase::restrictFiles(m_databasePath);
                }
            }
            sqlite3_finalize(statement);
        }
        ++m_revision;
        emit changed();
    }

    void FaviconStore::clear() {
        if (m_database) {
            SqlCipherDatabase::execute(m_database, "DELETE FROM favicons");
        }
        if (!m_icons.isEmpty()) {
            m_icons.clear();
            ++m_revision;
            emit changed();
        }
    }

}

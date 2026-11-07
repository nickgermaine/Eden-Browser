#include "core/history/recentlyclosedstore.h"
#include "core/profiles/sqlcipherdatabase.h"
#include "core/urlsanitizer.h"

#include <QDateTime>

#include <sqlcipher/sqlite3.h>

namespace eden::core {

    static QString closedColumnText(sqlite3_stmt *statement, int column) {
        const unsigned char *value = sqlite3_column_text(statement, column);
        return value ? QString::fromUtf8(reinterpret_cast<const char *>(value), sqlite3_column_bytes(statement, column))
                     : QString();
    }

    RecentlyClosedStore::RecentlyClosedStore(QString databasePath, QObject *parent)
        : QAbstractListModel(parent),
          m_databasePath(std::move(databasePath)) {}

    RecentlyClosedStore::~RecentlyClosedStore() {
        if (m_database) {
            sqlite3_close_v2(m_database);
        }
    }

    bool RecentlyClosedStore::initialize(const QByteArray &key) {
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
                "CREATE TABLE IF NOT EXISTS closed_tabs("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,url TEXT NOT NULL,title TEXT NOT NULL,"
                "closed_at INTEGER NOT NULL)"
            )) {
            sqlite3_close_v2(m_database);
            m_database = nullptr;
            return false;
        }
        QList<Entry> entries;
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(
                m_database,
                "SELECT id,title,url,closed_at FROM closed_tabs ORDER BY closed_at DESC,id DESC LIMIT ?",
                -1,
                &statement,
                nullptr
            ) == SQLITE_OK) {
            sqlite3_bind_int(statement, 1, capacity);
            while (sqlite3_step(statement) == SQLITE_ROW) {
                entries.append(
                    {sqlite3_column_int64(statement, 0),
                     closedColumnText(statement, 1),
                     urlWithoutCredentials(QUrl(closedColumnText(statement, 2))),
                     sqlite3_column_int64(statement, 3)}
                );
            }
        }
        sqlite3_finalize(statement);
        beginResetModel();
        m_entries = std::move(entries);
        endResetModel();
        return true;
    }

    bool RecentlyClosedStore::isInitialized() const {
        return m_database;
    }

    int RecentlyClosedStore::rowCount(const QModelIndex &parent) const {
        return parent.isValid() ? 0 : m_entries.size();
    }

    QVariant RecentlyClosedStore::data(const QModelIndex &index, int role) const {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size()) {
            return {};
        }
        const Entry &entry = m_entries.at(index.row());
        if (role == ClosedIdRole) {
            return entry.id;
        }
        if (role == TitleRole) {
            return entry.title;
        }
        if (role == UrlRole) {
            return entry.url;
        }
        if (role == ClosedAtRole) {
            return QDateTime::fromMSecsSinceEpoch(entry.closedAt);
        }
        return {};
    }

    QHash<int, QByteArray> RecentlyClosedStore::roleNames() const {
        return {{ClosedIdRole, "closedId"}, {TitleRole, "title"}, {UrlRole, "url"}, {ClosedAtRole, "closedAt"}};
    }

    void RecentlyClosedStore::add(const QUrl &url, const QString &title) {
        const QUrl sanitizedUrl = urlWithoutCredentials(url);
        const QString scheme = sanitizedUrl.scheme();
        if (!sanitizedUrl.isValid() || sanitizedUrl.isEmpty() || scheme == "about" || scheme == "eden") {
            return;
        }
        Entry entry{0, title, sanitizedUrl, QDateTime::currentMSecsSinceEpoch()};
        if (m_database) {
            sqlite3_stmt *statement = nullptr;
            if (sqlite3_prepare_v2(
                    m_database,
                    "INSERT INTO closed_tabs(url,title,closed_at) VALUES(?,?,?)",
                    -1,
                    &statement,
                    nullptr
                ) == SQLITE_OK) {
                const QByteArray encodedUrl = sanitizedUrl.toString().toUtf8();
                const QByteArray encodedTitle = title.toUtf8();
                sqlite3_bind_text(statement, 1, encodedUrl.constData(), encodedUrl.size(), SQLITE_TRANSIENT);
                sqlite3_bind_text(statement, 2, encodedTitle.constData(), encodedTitle.size(), SQLITE_TRANSIENT);
                sqlite3_bind_int64(statement, 3, entry.closedAt);
                if (sqlite3_step(statement) == SQLITE_DONE) {
                    entry.id = sqlite3_last_insert_rowid(m_database);
                }
            }
            sqlite3_finalize(statement);
            const QByteArray trim = QByteArrayLiteral("DELETE FROM closed_tabs WHERE id NOT IN(SELECT id FROM "
                                                      "closed_tabs ORDER BY closed_at DESC,id DESC LIMIT ") +
                                    QByteArray::number(capacity) + QByteArrayLiteral(")");
            SqlCipherDatabase::execute(m_database, trim);
            SqlCipherDatabase::restrictFiles(m_databasePath);
        }
        if (entry.id == 0) {
            entry.id = m_nextTransientId--;
        }
        beginInsertRows({}, 0, 0);
        m_entries.prepend(entry);
        endInsertRows();
        while (m_entries.size() > capacity) {
            const int last = m_entries.size() - 1;
            beginRemoveRows({}, last, last);
            m_entries.removeLast();
            endRemoveRows();
        }
    }

    void RecentlyClosedStore::removeRow(int row) {
        if (row < 0 || row >= m_entries.size()) {
            return;
        }
        const qint64 id = m_entries.at(row).id;
        if (m_database && id > 0) {
            SqlCipherDatabase::execute(
                m_database,
                QByteArrayLiteral("DELETE FROM closed_tabs WHERE id=") + QByteArray::number(id)
            );
        }
        beginRemoveRows({}, row, row);
        m_entries.removeAt(row);
        endRemoveRows();
    }

    QUrl RecentlyClosedStore::take(qint64 closedId) {
        for (int row = 0; row < m_entries.size(); ++row) {
            if (m_entries.at(row).id == closedId) {
                const QUrl url = m_entries.at(row).url;
                removeRow(row);
                return url;
            }
        }
        return {};
    }

    void RecentlyClosedStore::removeLatest(const QUrl &url) {
        const QUrl sanitizedUrl = urlWithoutCredentials(url);
        for (int row = 0; row < m_entries.size(); ++row) {
            if (m_entries.at(row).url == sanitizedUrl) {
                removeRow(row);
                return;
            }
        }
    }

    void RecentlyClosedStore::clear() {
        if (m_database) {
            SqlCipherDatabase::execute(m_database, "DELETE FROM closed_tabs");
        }
        if (!m_entries.isEmpty()) {
            beginRemoveRows({}, 0, m_entries.size() - 1);
            m_entries.clear();
            endRemoveRows();
        }
    }

}

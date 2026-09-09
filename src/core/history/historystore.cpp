#include "core/history/historystore.h"
#include "core/profiles/sqlcipherdatabase.h"
#include "core/urlsanitizer.h"

#include <QDateTime>

#include <sqlcipher/sqlite3.h>

#include <algorithm>

namespace eden::core {

    static QString historyColumnText(sqlite3_stmt *statement, int column) {
        const unsigned char *value = sqlite3_column_text(statement, column);
        return value ? QString::fromUtf8(reinterpret_cast<const char *>(value)) : QString();
    }

    HistoryStore::HistoryStore(QString databasePath, QObject *parent)
        : QAbstractListModel(parent),
          m_databasePath(std::move(databasePath)) {}

    HistoryStore::~HistoryStore() {
        if (m_database) {
            sqlite3_close_v2(m_database);
        }
    }

    void HistoryStore::initialize(const QByteArray &key) {
        if (m_database || key.size() != 32) {
            return;
        }
        m_database = SqlCipherDatabase::open(m_databasePath, key, false);
        if (m_database) {
            reload();
        }
    }

    bool HistoryStore::isInitialized() const {
        return m_database;
    }

    int HistoryStore::rowCount(const QModelIndex &parent) const {
        return parent.isValid() ? 0 : m_entries.size();
    }

    QVariant HistoryStore::data(const QModelIndex &index, int role) const {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size()) {
            return {};
        }
        const Entry &entry = m_entries.at(index.row());
        if (role == TitleRole) {
            return entry.title;
        }
        if (role == UrlRole) {
            return entry.url;
        }
        if (role == VisitedAtRole) {
            return QDateTime::fromMSecsSinceEpoch(entry.visitedAt);
        }
        if (role == VisitCountRole) {
            return entry.visitCount;
        }
        return {};
    }

    QHash<int, QByteArray> HistoryStore::roleNames() const {
        return {{TitleRole, "title"}, {UrlRole, "url"}, {VisitedAtRole, "visitedAt"}, {VisitCountRole, "visitCount"}};
    }

    void HistoryStore::recordVisit(const QUrl &url, const QString &title) {
        if (!isInitialized()) {
            return;
        }
        const QUrl sanitizedUrl = urlWithoutCredentials(url);
        if (!sanitizedUrl.isValid() || sanitizedUrl.isEmpty() || sanitizedUrl.scheme() == "about") {
            return;
        }
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(
                m_database,
                "INSERT INTO history(url, title, visited_at, visit_count) VALUES(?, ?, ?, 1) ON CONFLICT(url) DO "
                "UPDATE "
                "SET title = "
                "excluded.title, visited_at = excluded.visited_at, visit_count = history.visit_count + 1",
                -1,
                &statement,
                nullptr
            ) != SQLITE_OK) {
            return;
        }
        const QByteArray encodedUrl = sanitizedUrl.toString().toUtf8();
        const QByteArray encodedTitle = title.toUtf8();
        sqlite3_bind_text(statement, 1, encodedUrl.constData(), encodedUrl.size(), SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, encodedTitle.constData(), encodedTitle.size(), SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 3, QDateTime::currentMSecsSinceEpoch());
        if (sqlite3_step(statement) == SQLITE_DONE) {
            SqlCipherDatabase::restrictFiles(m_databasePath);
            sqlite3_finalize(statement);
            reload();
            return;
        }
        sqlite3_finalize(statement);
    }

    QStringList HistoryStore::search(const QString &queryText, int limit) const {
        QStringList results;
        const QString normalizedQuery = queryText.trimmed();
        if (normalizedQuery.isEmpty() || limit <= 0) {
            return results;
        }
        struct Match {
            const Entry *entry;
            bool prefix;
        };
        QList<Match> matches;
        for (const Entry &entry : m_entries) {
            const QString url = entry.url.toString();
            const bool titleMatch = entry.title.contains(normalizedQuery, Qt::CaseInsensitive);
            const bool urlMatch = url.contains(normalizedQuery, Qt::CaseInsensitive);
            if (titleMatch || urlMatch) {
                matches.append({&entry, url.startsWith(normalizedQuery, Qt::CaseInsensitive)});
            }
        }
        std::stable_sort(matches.begin(), matches.end(), [](const Match &left, const Match &right) {
            if (left.prefix != right.prefix) {
                return left.prefix;
            }
            if (left.entry->visitCount != right.entry->visitCount) {
                return left.entry->visitCount > right.entry->visitCount;
            }
            return left.entry->visitedAt > right.entry->visitedAt;
        });
        const qsizetype resultCount = std::min<qsizetype>(limit, matches.size());
        results.reserve(resultCount);
        for (qsizetype index = 0; index < resultCount; ++index) {
            const Entry &entry = *matches.at(index).entry;
            results.append(entry.title + "\n" + entry.url.toString());
        }
        return results;
    }

    void HistoryStore::remove(const QUrl &url) {
        if (!m_database) {
            return;
        }
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(m_database, "DELETE FROM history WHERE url=?", -1, &statement, nullptr) != SQLITE_OK) {
            return;
        }
        const QByteArray encodedUrl = urlWithoutCredentials(url).toString().toUtf8();
        sqlite3_bind_text(statement, 1, encodedUrl.constData(), encodedUrl.size(), SQLITE_TRANSIENT);
        if (sqlite3_step(statement) == SQLITE_DONE) {
            SqlCipherDatabase::restrictFiles(m_databasePath);
            sqlite3_finalize(statement);
            reload();
            return;
        }
        sqlite3_finalize(statement);
    }

    void HistoryStore::clear() {
        if (SqlCipherDatabase::execute(m_database, "DELETE FROM history")) {
            SqlCipherDatabase::restrictFiles(m_databasePath);
            reload();
        }
    }

    void HistoryStore::reload() {
        QList<Entry> entries;
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(
                m_database,
                "SELECT title,url,visited_at,visit_count FROM history ORDER BY visited_at DESC LIMIT 1000",
                -1,
                &statement,
                nullptr
            ) == SQLITE_OK) {
            while (sqlite3_step(statement) == SQLITE_ROW) {
                entries.append(
                    {historyColumnText(statement, 0),
                     urlWithoutCredentials(QUrl(historyColumnText(statement, 1))),
                     sqlite3_column_int64(statement, 2),
                     sqlite3_column_int(statement, 3)}
                );
            }
        }
        sqlite3_finalize(statement);
        beginResetModel();
        m_entries = std::move(entries);
        endResetModel();
    }

}

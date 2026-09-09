#include "core/bookmarks/bookmarkstore.h"
#include "core/profiles/sqlcipherdatabase.h"
#include "core/urlsanitizer.h"

#include <QDateTime>

#include <sqlcipher/sqlite3.h>

#include <algorithm>

namespace eden::core {

    static QString bookmarkColumnText(sqlite3_stmt *statement, int column) {
        const unsigned char *value = sqlite3_column_text(statement, column);
        return value ? QString::fromUtf8(reinterpret_cast<const char *>(value)) : QString();
    }

    BookmarkStore::BookmarkStore(QString databasePath, QObject *parent)
        : QAbstractListModel(parent),
          m_databasePath(std::move(databasePath)) {}

    BookmarkStore::~BookmarkStore() {
        if (m_database) {
            sqlite3_close_v2(m_database);
        }
    }

    void BookmarkStore::initialize(const QByteArray &key) {
        if (m_database || key.size() != 32) {
            return;
        }
        m_database = SqlCipherDatabase::open(m_databasePath, key, false);
        if (m_database) {
            reload();
        }
    }

    bool BookmarkStore::isInitialized() const {
        return m_database;
    }

    int BookmarkStore::rowCount(const QModelIndex &parent) const {
        return parent.isValid() ? 0 : m_entries.size();
    }

    QVariant BookmarkStore::data(const QModelIndex &index, int role) const {
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
        if (role == FolderRole) {
            return entry.folder;
        }
        if (role == CreatedAtRole) {
            return QDateTime::fromMSecsSinceEpoch(entry.createdAt);
        }
        return {};
    }

    QHash<int, QByteArray> BookmarkStore::roleNames() const {
        return {{TitleRole, "title"}, {UrlRole, "url"}, {FolderRole, "folder"}, {CreatedAtRole, "createdAt"}};
    }

    bool BookmarkStore::contains(const QUrl &url) const {
        if (!m_database) {
            return false;
        }
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(m_database, "SELECT 1 FROM bookmarks WHERE url=? LIMIT 1", -1, &statement, nullptr) !=
            SQLITE_OK) {
            return false;
        }
        const QByteArray encodedUrl = urlWithoutCredentials(url).toString().toUtf8();
        sqlite3_bind_text(statement, 1, encodedUrl.constData(), encodedUrl.size(), SQLITE_TRANSIENT);
        const bool found = sqlite3_step(statement) == SQLITE_ROW;
        sqlite3_finalize(statement);
        return found;
    }

    void BookmarkStore::add(const QUrl &url, const QString &title, const QString &folder) {
        const QUrl sanitizedUrl = urlWithoutCredentials(url);
        if (!sanitizedUrl.isValid() || sanitizedUrl.isEmpty()) {
            return;
        }
        if (!m_database) {
            return;
        }
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(
                m_database,
                "INSERT INTO bookmarks(url, title, folder, created_at) VALUES(?, ?, ?, ?) ON CONFLICT(url) DO UPDATE "
                "SET "
                "title = "
                "excluded.title, folder = excluded.folder",
                -1,
                &statement,
                nullptr
            ) != SQLITE_OK) {
            return;
        }
        const QByteArray encodedUrl = sanitizedUrl.toString().toUtf8();
        const QByteArray encodedTitle = title.toUtf8();
        const QByteArray encodedFolder = (folder.isNull() ? QStringLiteral("") : folder).toUtf8();
        sqlite3_bind_text(statement, 1, encodedUrl.constData(), encodedUrl.size(), SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, encodedTitle.constData(), encodedTitle.size(), SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, encodedFolder.constData(), encodedFolder.size(), SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 4, QDateTime::currentMSecsSinceEpoch());
        if (sqlite3_step(statement) == SQLITE_DONE) {
            SqlCipherDatabase::restrictFiles(m_databasePath);
            sqlite3_finalize(statement);
            reload();
            return;
        }
        sqlite3_finalize(statement);
    }

    void BookmarkStore::remove(const QUrl &url) {
        if (!m_database) {
            return;
        }
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(m_database, "DELETE FROM bookmarks WHERE url=?", -1, &statement, nullptr) != SQLITE_OK) {
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

    void BookmarkStore::toggle(const QUrl &url, const QString &title) {
        if (contains(url)) {
            remove(url);
        } else {
            add(url, title);
        }
    }

    QStringList BookmarkStore::search(const QString &queryText, int limit) const {
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
            return left.entry->createdAt > right.entry->createdAt;
        });
        const qsizetype resultCount = std::min<qsizetype>(limit, matches.size());
        results.reserve(resultCount);
        for (qsizetype index = 0; index < resultCount; ++index) {
            const Entry &entry = *matches.at(index).entry;
            results.append(entry.title + "\n" + entry.url.toString());
        }
        return results;
    }

    void BookmarkStore::reload() {
        QList<Entry> entries;
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(
                m_database,
                "SELECT title,url,folder,created_at FROM bookmarks ORDER BY created_at DESC",
                -1,
                &statement,
                nullptr
            ) == SQLITE_OK) {
            while (sqlite3_step(statement) == SQLITE_ROW) {
                entries.append(
                    {bookmarkColumnText(statement, 0),
                     urlWithoutCredentials(QUrl(bookmarkColumnText(statement, 1))),
                     bookmarkColumnText(statement, 2),
                     sqlite3_column_int64(statement, 3)}
                );
            }
        }
        sqlite3_finalize(statement);
        beginResetModel();
        m_entries = std::move(entries);
        endResetModel();
    }

}

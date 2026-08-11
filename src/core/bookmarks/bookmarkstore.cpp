#include "core/bookmarks/bookmarkstore.h"
#include "core/urlsanitizer.h"

#include <QDateTime>
#include <QDir>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>

namespace eden::core {

static QString bookmarkDatabasePath() {
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(directory);
    return directory + "/eden.db";
}

BookmarkStore::BookmarkStore(QObject *parent)
    : QAbstractListModel(parent),
      m_connectionName("eden-bookmarks-" + QUuid::createUuid().toString(QUuid::WithoutBraces)),
      m_database(QSqlDatabase::addDatabase("QSQLITE", m_connectionName)) {
    m_database.setDatabaseName(bookmarkDatabasePath());
    initialize();
    reload();
}

BookmarkStore::~BookmarkStore() {
    m_database.close();
    m_database = {};
    QSqlDatabase::removeDatabase(m_connectionName);
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
    QSqlQuery query(m_database);
    query.prepare("SELECT 1 FROM bookmarks WHERE url = ? LIMIT 1");
    query.addBindValue(urlWithoutCredentials(url).toString());
    return query.exec() && query.next();
}

void BookmarkStore::add(const QUrl &url, const QString &title, const QString &folder) {
    const QUrl sanitizedUrl = urlWithoutCredentials(url);
    if (!sanitizedUrl.isValid() || sanitizedUrl.isEmpty()) {
        return;
    }
    QSqlQuery query(m_database);
    query.prepare("INSERT INTO bookmarks(url, title, folder, created_at) VALUES(?, ?, ?, ?) ON CONFLICT(url) DO UPDATE SET title = "
                  "excluded.title, folder = excluded.folder");
    query.addBindValue(sanitizedUrl.toString());
    query.addBindValue(title);
    query.addBindValue(folder);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (query.exec()) {
        reload();
    }
}

void BookmarkStore::remove(const QUrl &url) {
    QSqlQuery query(m_database);
    query.prepare("DELETE FROM bookmarks WHERE url = ?");
    query.addBindValue(urlWithoutCredentials(url).toString());
    if (query.exec()) {
        reload();
    }
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

void BookmarkStore::initialize() {
    if (!m_database.open()) {
        return;
    }
    QSqlQuery query(m_database);
    query.exec("PRAGMA journal_mode=WAL");
    query.exec("CREATE TABLE IF NOT EXISTS migrations(version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL)");
    query.exec("CREATE TABLE IF NOT EXISTS bookmarks(url TEXT PRIMARY KEY, title TEXT NOT NULL, folder TEXT NOT NULL DEFAULT '', "
               "created_at INTEGER NOT NULL)");
    query.exec("INSERT OR IGNORE INTO migrations(version, applied_at) VALUES(1, strftime('%s','now'))");
}

void BookmarkStore::reload() {
    QList<Entry> entries;
    QSqlQuery query(m_database);
    if (query.exec("SELECT title, url, folder, created_at FROM bookmarks ORDER BY created_at DESC")) {
        while (query.next()) {
            entries.append({query.value(0).toString(), urlWithoutCredentials(QUrl(query.value(1).toString())), query.value(2).toString(),
                            query.value(3).toLongLong()});
        }
    }
    beginResetModel();
    m_entries = std::move(entries);
    endResetModel();
}

}

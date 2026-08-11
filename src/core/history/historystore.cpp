#include "core/history/historystore.h"
#include "core/urlsanitizer.h"

#include <QDateTime>
#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>

namespace eden::core {

static QString databasePath() {
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(directory);
    return directory + "/eden.db";
}

HistoryStore::HistoryStore(QObject *parent)
    : QAbstractListModel(parent),
      m_connectionName("eden-history-" + QUuid::createUuid().toString(QUuid::WithoutBraces)),
      m_database(QSqlDatabase::addDatabase("QSQLITE", m_connectionName)) {
    m_database.setDatabaseName(databasePath());
    initialize();
    reload();
}

HistoryStore::~HistoryStore() {
    m_database.close();
    m_database = {};
    QSqlDatabase::removeDatabase(m_connectionName);
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
    const QUrl sanitizedUrl = urlWithoutCredentials(url);
    if (!sanitizedUrl.isValid() || sanitizedUrl.isEmpty() || sanitizedUrl.scheme() == "about") {
        return;
    }
    QSqlQuery query(m_database);
    query.prepare("INSERT INTO history(url, title, visited_at, visit_count) VALUES(?, ?, ?, 1) ON CONFLICT(url) DO UPDATE SET title = "
                  "excluded.title, visited_at = excluded.visited_at, visit_count = history.visit_count + 1");
    query.addBindValue(sanitizedUrl.toString());
    query.addBindValue(title);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (query.exec()) {
        reload();
    }
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
    QSqlQuery query(m_database);
    query.prepare("DELETE FROM history WHERE url = ?");
    query.addBindValue(urlWithoutCredentials(url).toString());
    if (query.exec()) {
        reload();
    }
}

void HistoryStore::clear() {
    QSqlQuery query(m_database);
    if (query.exec("DELETE FROM history")) {
        reload();
    }
}

void HistoryStore::initialize() {
    if (!m_database.open()) {
        return;
    }
    QSqlQuery query(m_database);
    query.exec("PRAGMA journal_mode=WAL");
    query.exec("CREATE TABLE IF NOT EXISTS migrations(version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL)");
    query.exec("CREATE TABLE IF NOT EXISTS history(url TEXT PRIMARY KEY, title TEXT NOT NULL, visited_at INTEGER NOT NULL, visit_count "
               "INTEGER NOT NULL DEFAULT 1)");
    query.exec("INSERT OR IGNORE INTO migrations(version, applied_at) VALUES(1, strftime('%s','now'))");
}

void HistoryStore::reload() {
    QList<Entry> entries;
    QSqlQuery query(m_database);
    if (query.exec("SELECT title, url, visited_at, visit_count FROM history ORDER BY visited_at DESC LIMIT 1000")) {
        while (query.next()) {
            entries.append({query.value(0).toString(), urlWithoutCredentials(QUrl(query.value(1).toString())), query.value(2).toLongLong(),
                            query.value(3).toInt()});
        }
    }
    beginResetModel();
    m_entries = std::move(entries);
    endResetModel();
}

}

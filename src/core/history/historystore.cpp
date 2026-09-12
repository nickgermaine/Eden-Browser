#include "core/history/historystore.h"
#include "core/profiles/sqlcipherdatabase.h"
#include "core/urlsanitizer.h"

#include <QDateTime>
#include <QFutureWatcher>
#include <QMap>
#include <QPointer>
#include <QPromise>
#include <QThreadPool>

#include <sqlcipher/sqlite3.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <optional>

namespace eden::core {

    static QString historyColumnText(sqlite3_stmt *statement, int column) {
        const unsigned char *value = sqlite3_column_text(statement, column);
        return value ? QString::fromUtf8(reinterpret_cast<const char *>(value), sqlite3_column_bytes(statement, column))
                     : QString();
    }

    class HistoryStore::Private {
      public:
        enum class Kind { Visit, Remove, Clear, Close };

        struct Result {
            quint64 sequence = 0;
            Kind kind = Kind::Visit;
            Entry entry;
            std::optional<Entry> boundary;
            ProfileError error = ProfileError::None;
        };

        struct Operation {
            quint64 sequence;
            Kind kind;
            Entry entry;
            std::shared_ptr<QPromise<Result>> completion;
        };

        struct Worker {
            Worker(sqlite3 *connection, QString path, qint64 latestVisit)
                : database(connection),
                  databasePath(std::move(path)),
                  latestTimestamp(latestVisit) {}

            ~Worker() {
                sqlite3_close_v2(database);
            }

            Result execute(const Operation &operation) {
                Result
                    result{operation.sequence, operation.kind, operation.entry, {}, ProfileError::HistoryWriteFailed};
                if (operation.kind == Kind::Close) {
                    sqlite3_close_v2(database);
                    database = nullptr;
                    result.error = ProfileError::None;
                    return result;
                }
                if (!SqlCipherDatabase::execute(database, "BEGIN IMMEDIATE")) {
                    return result;
                }
                bool saved = false;
                if (operation.kind == Kind::Clear) {
                    saved = SqlCipherDatabase::execute(database, "DELETE FROM history");
                } else {
                    sqlite3_stmt *statement = nullptr;
                    const char *sql =
                        operation.kind == Kind::Visit
                            ? "INSERT INTO history(url,title,visited_at,visit_count) VALUES(?,?,?,1) "
                              "ON CONFLICT(url) DO UPDATE SET title=excluded.title,visited_at=excluded.visited_at,"
                              "visit_count=history.visit_count+1 RETURNING visit_count"
                            : "DELETE FROM history WHERE url=?";
                    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK) {
                        const QByteArray url = operation.entry.url.toString().toUtf8();
                        saved =
                            sqlite3_bind_text(statement, 1, url.constData(), url.size(), SQLITE_TRANSIENT) == SQLITE_OK;
                        if (saved && operation.kind == Kind::Visit) {
                            const QByteArray title = operation.entry.title.toUtf8();
                            saved =
                                sqlite3_bind_text(statement, 2, title.constData(), title.size(), SQLITE_TRANSIENT) ==
                                    SQLITE_OK &&
                                sqlite3_bind_int64(statement, 3, operation.entry.visitedAt) == SQLITE_OK &&
                                sqlite3_step(statement) == SQLITE_ROW;
                            if (saved) {
                                result.entry.visitCount = sqlite3_column_int64(statement, 0);
                            }
                        }
                        saved = saved && sqlite3_step(statement) == SQLITE_DONE;
                    }
                    sqlite3_finalize(statement);
                }
                if (saved && (operation.kind == Kind::Remove ||
                              (operation.kind == Kind::Visit && operation.entry.visitedAt <= latestTimestamp))) {
                    sqlite3_stmt *statement = nullptr;
                    saved = sqlite3_prepare_v2(
                                database,
                                "SELECT title,url,visited_at,visit_count FROM history "
                                "ORDER BY visited_at DESC,url ASC LIMIT 1 OFFSET 999",
                                -1,
                                &statement,
                                nullptr
                            ) == SQLITE_OK;
                    if (saved) {
                        const int step = sqlite3_step(statement);
                        saved = step == SQLITE_ROW || step == SQLITE_DONE;
                        if (step == SQLITE_ROW) {
                            result.boundary = Entry{
                                historyColumnText(statement, 0),
                                urlWithoutCredentials(QUrl(historyColumnText(statement, 1))),
                                sqlite3_column_int64(statement, 2),
                                sqlite3_column_int64(statement, 3)
                            };
                        }
                    }
                    sqlite3_finalize(statement);
                }
                if (!saved || !SqlCipherDatabase::execute(database, "COMMIT")) {
                    SqlCipherDatabase::execute(database, "ROLLBACK");
                    return result;
                }
                if (!permissionsRestricted) {
                    SqlCipherDatabase::restrictFiles(databasePath);
                    permissionsRestricted = true;
                }
                if (operation.kind == Kind::Clear) {
                    latestTimestamp = 0;
                } else if (operation.kind == Kind::Visit) {
                    latestTimestamp = std::max(latestTimestamp, operation.entry.visitedAt);
                }
                result.error = ProfileError::None;
                return result;
            }

            static void drain(const std::shared_ptr<Worker> &worker) {
                for (;;) {
                    std::optional<Operation> operation;
                    {
                        const std::lock_guard lock(worker->mutex);
                        if (worker->operations.empty()) {
                            worker->running = false;
                            return;
                        }
                        operation = std::move(worker->operations.front());
                        worker->operations.pop_front();
                    }
                    operation->completion->addResult(worker->execute(*operation));
                    operation->completion->finish();
                }
            }

            sqlite3 *database;
            QString databasePath;
            qint64 latestTimestamp;
            bool permissionsRestricted = false;
            std::mutex mutex;
            std::deque<Operation> operations;
            bool running = false;
        };

        explicit Private(HistoryStore *store)
            : owner(store) {}

        ~Private() {
            if (worker) {
                QThreadPool::globalInstance()->start([connection = std::move(worker)] {});
            }
        }

        static bool precedes(const Entry &left, const Entry &right) {
            return left.visitedAt != right.visitedAt ? left.visitedAt > right.visitedAt
                                                     : left.url.toString().toUtf8() < right.url.toString().toUtf8();
        }

        void enqueue(Kind kind, Entry entry) {
            auto completion = std::make_shared<QPromise<Result>>();
            completion->start();
            auto *watcher = new QFutureWatcher<Result>(owner);
            QObject::connect(watcher, &QFutureWatcher<Result>::finished, owner, [this, watcher] {
                const Result completed = watcher->result();
                results.insert(completed.sequence, completed);
                watcher->deleteLater();
                if (applyingResults) {
                    return;
                }
                applyingResults = true;
                const QPointer<HistoryStore> survivingOwner(owner);
                while (results.contains(nextResult)) {
                    const Result result = results.take(nextResult++);
                    --pending;
                    if (result.kind == Kind::Close) {
                        closed = true;
                        emit owner->closed();
                        if (!survivingOwner) {
                            return;
                        }
                        continue;
                    }
                    if (result.error == ProfileError::None) {
                        apply(result);
                    }
                    owner->setError(result.error);
                    if (!survivingOwner) {
                        return;
                    }
                    if (result.error == ProfileError::None) {
                        emit owner->changed();
                        if (!survivingOwner) {
                            return;
                        }
                    }
                }
                applyingResults = false;
                if (pending == 0) {
                    emit owner->writesFinished();
                }
            });
            watcher->setFuture(completion->future());
            ++pending;
            bool start = false;
            {
                const std::lock_guard lock(worker->mutex);
                worker->operations.push_back({nextOperation++, kind, std::move(entry), std::move(completion)});
                start = !worker->running;
                worker->running = true;
            }
            if (start) {
                QThreadPool::globalInstance()->start([connection = worker] { Worker::drain(connection); });
            }
        }

        int find(const QUrl &url) const {
            const auto &entries = owner->m_entries;
            const auto found =
                std::find_if(entries.cbegin(), entries.cend(), [&url](const Entry &entry) { return entry.url == url; });
            return found == entries.cend() ? -1 : static_cast<int>(found - entries.cbegin());
        }

        void removeRow(int row) {
            if (row >= 0) {
                owner->beginRemoveRows({}, row, row);
                owner->m_entries.removeAt(row);
                owner->endRemoveRows();
            }
        }

        void insertOrUpdate(const Entry &entry) {
            auto &entries = owner->m_entries;
            const int previous = find(entry.url);
            int destination = static_cast<int>(
                std::lower_bound(entries.cbegin(), entries.cend(), entry, precedes) - entries.cbegin()
            );
            if (previous >= 0 && previous < destination) {
                --destination;
            }
            if (previous >= 0) {
                if (previous != destination) {
                    owner->beginMoveRows(
                        {},
                        previous,
                        previous,
                        {},
                        destination > previous ? destination + 1 : destination
                    );
                    entries.move(previous, destination);
                    owner->endMoveRows();
                }
                QList<int> roles;
                const Entry &old = entries.at(destination);
                if (old.title != entry.title) {
                    roles.append(TitleRole);
                }
                if (old.visitedAt != entry.visitedAt) {
                    roles.append(VisitedAtRole);
                }
                if (old.visitCount != entry.visitCount) {
                    roles.append(VisitCountRole);
                }
                entries[destination] = entry;
                if (!roles.isEmpty()) {
                    emit owner->dataChanged(owner->index(destination), owner->index(destination), roles);
                }
            } else if (destination < 1000) {
                owner->beginInsertRows({}, destination, destination);
                entries.insert(destination, entry);
                owner->endInsertRows();
                if (entries.size() > 1000) {
                    removeRow(1000);
                }
            }
        }

        void apply(const Result &result) {
            if (result.kind == Kind::Clear) {
                if (!owner->m_entries.isEmpty()) {
                    owner->beginRemoveRows({}, 0, owner->m_entries.size() - 1);
                    owner->m_entries.clear();
                    owner->endRemoveRows();
                }
                return;
            }
            if (result.kind == Kind::Remove || (result.boundary && precedes(*result.boundary, result.entry))) {
                removeRow(find(result.entry.url));
            } else {
                insertOrUpdate(result.entry);
            }
            if (result.boundary && owner->m_entries.size() < 1000 && find(result.boundary->url) < 0) {
                insertOrUpdate(*result.boundary);
            }
        }

        HistoryStore *owner;
        std::shared_ptr<Worker> worker;
        QMap<quint64, Result> results;
        quint64 nextOperation = 0;
        quint64 nextResult = 0;
        int pending = 0;
        bool applyingResults = false;
        bool closing = false;
        bool closed = false;
    };

    HistoryStore::HistoryStore(QString databasePath, QObject *parent)
        : QAbstractListModel(parent),
          d(std::make_unique<Private>(this)),
          m_databasePath(std::move(databasePath)) {}

    HistoryStore::~HistoryStore() = default;

    void HistoryStore::initialize(const QByteArray &key) {
        if (d->worker) {
            return;
        }
        if (key.size() != 32) {
            setError(ProfileError::HistoryReadFailed);
            return;
        }
        sqlite3 *database = SqlCipherDatabase::open(m_databasePath, key, false);
        sqlite3_stmt *statement = nullptr;
        QList<Entry> entries;
        bool loaded =
            database &&
            sqlite3_prepare_v2(
                database,
                "SELECT title,url,visited_at,visit_count FROM history ORDER BY visited_at DESC,url ASC LIMIT 1000",
                -1,
                &statement,
                nullptr
            ) == SQLITE_OK;
        if (loaded) {
            int step;
            while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
                entries.append(
                    {historyColumnText(statement, 0),
                     urlWithoutCredentials(QUrl(historyColumnText(statement, 1))),
                     sqlite3_column_int64(statement, 2),
                     sqlite3_column_int64(statement, 3)}
                );
            }
            loaded = step == SQLITE_DONE;
        }
        sqlite3_finalize(statement);
        if (!loaded) {
            sqlite3_close_v2(database);
            setError(ProfileError::HistoryReadFailed);
            return;
        }
        d->worker = std::make_shared<Private::Worker>(
            database,
            m_databasePath,
            entries.isEmpty() ? 0 : entries.constFirst().visitedAt
        );
        beginResetModel();
        m_entries = std::move(entries);
        endResetModel();
        setError(ProfileError::None);
    }

    bool HistoryStore::isInitialized() const {
        return d->worker && !d->closing;
    }

    void HistoryStore::close(std::function<void()> completion) {
        if (!d->worker || d->closed) {
            if (completion) {
                completion();
            }
            return;
        }
        if (completion) {
            connect(
                this,
                &HistoryStore::closed,
                this,
                [completion = std::move(completion)] { completion(); },
                Qt::SingleShotConnection
            );
        }
        if (!d->closing) {
            d->closing = true;
            d->enqueue(Private::Kind::Close, {});
        }
    }

    bool HistoryStore::hasPendingWrites() const {
        return d->pending > 0;
    }

    ProfileError HistoryStore::error() const {
        return m_error;
    }

    void HistoryStore::setError(ProfileError error) {
        if (m_error != error) {
            m_error = error;
            emit errorChanged();
        }
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
        d->enqueue(Private::Kind::Visit, {title, sanitizedUrl, QDateTime::currentMSecsSinceEpoch(), 0});
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
        if (isInitialized()) {
            d->enqueue(Private::Kind::Remove, {{}, urlWithoutCredentials(url), 0, 0});
        }
    }

    void HistoryStore::clear() {
        if (isInitialized()) {
            d->enqueue(Private::Kind::Clear, {});
        }
    }

}

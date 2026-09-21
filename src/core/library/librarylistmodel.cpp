#include "core/library/librarylistmodel.h"

#include "core/bookmarks/bookmarkstore.h"
#include "core/downloads/downloadmanager.h"
#include "core/favicons/faviconstore.h"
#include "core/history/historystore.h"
#include "core/history/recentlyclosedstore.h"

#include <QDateTime>

#include <algorithm>

namespace eden::core {

    LibraryListModel::LibraryListModel(Mode mode, QObject *parent)
        : QAbstractListModel(parent),
          m_mode(mode) {
        m_rebuildTimer.setSingleShot(true);
        m_rebuildTimer.setInterval(0);
        connect(&m_rebuildTimer, &QTimer::timeout, this, &LibraryListModel::rebuild);
    }

    void LibraryListModel::watch(QAbstractItemModel *source) {
        if (!source) {
            return;
        }
        connect(source, &QAbstractItemModel::rowsInserted, this, &LibraryListModel::scheduleRebuild);
        connect(source, &QAbstractItemModel::rowsRemoved, this, &LibraryListModel::scheduleRebuild);
        connect(source, &QAbstractItemModel::rowsMoved, this, &LibraryListModel::scheduleRebuild);
        connect(source, &QAbstractItemModel::modelReset, this, &LibraryListModel::scheduleRebuild);
        connect(source, &QAbstractItemModel::dataChanged, this, &LibraryListModel::scheduleRebuild);
        connect(source, &QObject::destroyed, this, &LibraryListModel::scheduleRebuild);
    }

    void LibraryListModel::setHistory(HistoryStore *history, RecentlyClosedStore *closed) {
        m_history = history;
        m_closed = closed;
        watch(history);
        watch(closed);
        rebuild();
    }

    void LibraryListModel::setBookmarks(BookmarkStore *bookmarks) {
        m_bookmarks = bookmarks;
        watch(bookmarks);
        rebuild();
    }

    void LibraryListModel::setDownloads(DownloadManager *downloads) {
        m_downloads = downloads;
        watch(downloads);
        rebuild();
    }

    void LibraryListModel::setFavicons(FaviconStore *favicons) {
        m_favicons = favicons;
        if (favicons) {
            connect(favicons, &FaviconStore::changed, this, [this] {
                if (!m_rows.isEmpty()) {
                    emit dataChanged(index(0), index(m_rows.size() - 1), {FaviconRole});
                }
            });
        }
    }

    int LibraryListModel::rowCount(const QModelIndex &parent) const {
        return parent.isValid() ? 0 : m_rows.size();
    }

    QVariant LibraryListModel::data(const QModelIndex &index, int role) const {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
            return {};
        }
        const Row &row = m_rows.at(index.row());
        switch (role) {
        case HeaderRole:
            return row.header;
        case SectionIdRole:
            return row.sectionId;
        case SectionTitleRole:
            return row.sectionTitle;
        case CollapsedRole:
            return m_collapsed.contains(row.sectionId);
        case TitleRole:
            return row.title;
        case UrlRole:
            return row.url;
        case FaviconRole:
            return !row.header && m_favicons ? m_favicons->iconFor(row.url) : QUrl();
        case TimestampRole:
            return QDateTime::fromMSecsSinceEpoch(row.timestamp);
        case ClosedIdRole:
            return row.closedId;
        case SourceRowRole:
            return row.sourceRow;
        case TargetPathRole:
            return row.targetPath;
        case ReceivedBytesRole:
            return row.receivedBytes;
        case TotalBytesRole:
            return row.totalBytes;
        case DownloadStateRole:
            return row.downloadState;
        default:
            return {};
        }
    }

    QHash<int, QByteArray> LibraryListModel::roleNames() const {
        return {
            {HeaderRole, "header"},
            {SectionIdRole, "sectionId"},
            {SectionTitleRole, "sectionTitle"},
            {CollapsedRole, "collapsed"},
            {TitleRole, "title"},
            {UrlRole, "url"},
            {FaviconRole, "favicon"},
            {TimestampRole, "timestamp"},
            {ClosedIdRole, "closedId"},
            {SourceRowRole, "sourceRow"},
            {TargetPathRole, "targetPath"},
            {ReceivedBytesRole, "receivedBytes"},
            {TotalBytesRole, "totalBytes"},
            {DownloadStateRole, "downloadState"}
        };
    }

    QString LibraryListModel::query() const {
        return m_query;
    }

    void LibraryListModel::setQuery(const QString &query) {
        const QString normalized = query.trimmed();
        if (m_query == normalized) {
            return;
        }
        m_query = normalized;
        emit queryChanged();
        rebuild();
    }

    int LibraryListModel::itemCount() const {
        return m_itemCount;
    }

    bool LibraryListModel::reorderable() const {
        return m_mode == Mode::Bookmarks && m_query.isEmpty();
    }

    void LibraryListModel::toggleSection(const QString &sectionId) {
        if (sectionId.isEmpty()) {
            return;
        }
        if (!m_collapsed.remove(sectionId)) {
            m_collapsed.insert(sectionId);
        }
        rebuild();
    }

    bool LibraryListModel::moveItem(int fromRow, int toRow) {
        if (!reorderable() || !m_bookmarks || fromRow < 0 || fromRow >= m_rows.size() || toRow < 0 ||
            toRow >= m_rows.size()) {
            return false;
        }
        const bool moved = m_bookmarks->move(m_rows.at(fromRow).sourceRow, m_rows.at(toRow).sourceRow);
        if (moved) {
            rebuild();
        }
        return moved;
    }

    QString LibraryListModel::sectionForTimestamp(qint64 timestamp, qint64 now) {
        const QDateTime today = QDateTime::fromMSecsSinceEpoch(now).date().startOfDay();
        if (timestamp >= today.toMSecsSinceEpoch()) {
            return "recent";
        }
        if (timestamp >= today.addDays(-1).toMSecsSinceEpoch()) {
            return "yesterday";
        }
        if (timestamp >= today.addDays(-7).toMSecsSinceEpoch()) {
            return "week";
        }
        return "earlier";
    }

    QString LibraryListModel::sectionTitle(const QString &sectionId) const {
        if (sectionId == "closed") {
            return "Recently closed";
        }
        if (sectionId == "recent") {
            return m_mode == Mode::Downloads ? "Today" : "Recents";
        }
        if (sectionId == "yesterday") {
            return "Yesterday";
        }
        if (sectionId == "week") {
            return "Last 7 Days";
        }
        return "Earlier";
    }

    bool LibraryListModel::matches(const QString &title, const QString &url) const {
        return m_query.isEmpty() || title.contains(m_query, Qt::CaseInsensitive) ||
               url.contains(m_query, Qt::CaseInsensitive);
    }

    void LibraryListModel::appendSectioned(QList<Row> &rows, const QList<Row> &items) const {
        QString currentSection;
        for (const Row &item : items) {
            if (item.sectionId != currentSection) {
                currentSection = item.sectionId;
                Row header;
                header.header = true;
                header.sectionId = currentSection;
                header.sectionTitle = sectionTitle(currentSection);
                header.key = currentSection;
                rows.append(header);
            }
            if (!m_collapsed.contains(currentSection)) {
                rows.append(item);
            }
        }
    }

    QList<LibraryListModel::Row> LibraryListModel::buildRows() const {
        QList<Row> rows;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_mode == Mode::History) {
            QList<Row> items;
            if (m_closed) {
                const int count = m_closed->rowCount();
                int taken = 0;
                for (int source = 0; source < count && taken < recentlyClosedLimit; ++source) {
                    const QModelIndex sourceIndex = m_closed->index(source);
                    Row item;
                    item.sectionId = "closed";
                    item.title = m_closed->data(sourceIndex, RecentlyClosedStore::TitleRole).toString();
                    item.url = m_closed->data(sourceIndex, RecentlyClosedStore::UrlRole).toUrl();
                    if (!matches(item.title, item.url.toString())) {
                        continue;
                    }
                    item.closedId = m_closed->data(sourceIndex, RecentlyClosedStore::ClosedIdRole).toLongLong();
                    item.timestamp =
                        m_closed->data(sourceIndex, RecentlyClosedStore::ClosedAtRole).toDateTime().toMSecsSinceEpoch();
                    item.key = QString("closed:%1").arg(item.closedId);
                    item.sourceRow = source;
                    items.append(item);
                    ++taken;
                }
            }
            if (m_history) {
                const int count = m_history->rowCount();
                for (int source = 0; source < count; ++source) {
                    const QModelIndex sourceIndex = m_history->index(source);
                    Row item;
                    item.title = m_history->data(sourceIndex, HistoryStore::TitleRole).toString();
                    item.url = m_history->data(sourceIndex, HistoryStore::UrlRole).toUrl();
                    const QString url = item.url.toString();
                    if (!matches(item.title, url)) {
                        continue;
                    }
                    item.timestamp =
                        m_history->data(sourceIndex, HistoryStore::VisitedAtRole).toDateTime().toMSecsSinceEpoch();
                    item.sectionId = sectionForTimestamp(item.timestamp, now);
                    item.key = url;
                    item.sourceRow = source;
                    items.append(item);
                }
            }
            appendSectioned(rows, items);
        } else if (m_mode == Mode::Bookmarks) {
            if (m_bookmarks) {
                const int count = m_bookmarks->rowCount();
                for (int source = 0; source < count; ++source) {
                    const QModelIndex sourceIndex = m_bookmarks->index(source);
                    Row item;
                    item.title = m_bookmarks->data(sourceIndex, BookmarkStore::TitleRole).toString();
                    item.url = m_bookmarks->data(sourceIndex, BookmarkStore::UrlRole).toUrl();
                    const QString url = item.url.toString();
                    if (!matches(item.title, url)) {
                        continue;
                    }
                    item.timestamp =
                        m_bookmarks->data(sourceIndex, BookmarkStore::CreatedAtRole).toDateTime().toMSecsSinceEpoch();
                    item.key = url;
                    item.sourceRow = source;
                    rows.append(item);
                }
            }
        } else if (m_downloads) {
            QList<Row> items;
            const int count = m_downloads->rowCount();
            for (int source = 0; source < count; ++source) {
                const QModelIndex sourceIndex = m_downloads->index(source);
                Row item;
                item.title = m_downloads->data(sourceIndex, DownloadManager::FileNameRole).toString();
                item.url = m_downloads->data(sourceIndex, DownloadManager::SourceUrlRole).toUrl();
                if (!matches(item.title, item.url.toString())) {
                    continue;
                }
                item.targetPath = m_downloads->data(sourceIndex, DownloadManager::TargetPathRole).toString();
                item.receivedBytes = m_downloads->data(sourceIndex, DownloadManager::ReceivedBytesRole).toLongLong();
                item.totalBytes = m_downloads->data(sourceIndex, DownloadManager::TotalBytesRole).toLongLong();
                item.downloadState = m_downloads->data(sourceIndex, DownloadManager::StateRole).toString();
                item.timestamp =
                    m_downloads->data(sourceIndex, DownloadManager::StartedAtRole).toDateTime().toMSecsSinceEpoch();
                item.sectionId = sectionForTimestamp(item.timestamp, now);
                item.key = QString("%1|%2").arg(item.timestamp).arg(item.targetPath);
                item.sourceRow = source;
                items.append(item);
            }
            appendSectioned(rows, items);
        }
        return rows;
    }

    void LibraryListModel::scheduleRebuild() {
        m_rebuildTimer.start();
    }

    void LibraryListModel::rebuild() {
        m_rebuildTimer.stop();
        QList<Row> next = buildRows();
        const auto same = [](const Row &left, const Row &right) {
            return left.header == right.header && left.sectionId == right.sectionId && left.key == right.key;
        };
        const int oldSize = m_rows.size();
        const int newSize = next.size();
        int prefix = 0;
        while (prefix < oldSize && prefix < newSize && same(m_rows.at(prefix), next.at(prefix))) {
            ++prefix;
        }
        int suffix = 0;
        while (suffix < oldSize - prefix && suffix < newSize - prefix &&
               same(m_rows.at(oldSize - 1 - suffix), next.at(newSize - 1 - suffix))) {
            ++suffix;
        }
        const int removed = oldSize - prefix - suffix;
        const int inserted = newSize - prefix - suffix;
        if (removed > 0) {
            beginRemoveRows({}, prefix, prefix + removed - 1);
            m_rows.remove(prefix, removed);
            endRemoveRows();
        }
        if (inserted > 0) {
            beginInsertRows({}, prefix, prefix + inserted - 1);
            for (int offset = 0; offset < inserted; ++offset) {
                m_rows.insert(prefix + offset, next.at(prefix + offset));
            }
            endInsertRows();
        }
        m_rows = std::move(next);
        if (!m_rows.isEmpty()) {
            emit dataChanged(index(0), index(m_rows.size() - 1));
        }
        const int items =
            static_cast<int>(std::count_if(m_rows.cbegin(), m_rows.cend(), [](const Row &row) { return !row.header; }));
        if (items != m_itemCount) {
            m_itemCount = items;
            emit itemCountChanged();
        }
    }

}

#pragma once

#include <QAbstractListModel>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QUrl>

namespace eden::core {

    class BookmarkStore;
    class DownloadManager;
    class FaviconStore;
    class HistoryStore;
    class RecentlyClosedStore;

    class LibraryListModel final : public QAbstractListModel {
        Q_OBJECT
        Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
        Q_PROPERTY(int itemCount READ itemCount NOTIFY itemCountChanged)
        Q_PROPERTY(bool reorderable READ reorderable NOTIFY queryChanged)

      public:
        enum Role {
            HeaderRole = Qt::UserRole + 1,
            SectionIdRole,
            SectionTitleRole,
            CollapsedRole,
            TitleRole,
            UrlRole,
            FaviconRole,
            TimestampRole,
            ClosedIdRole,
            SourceRowRole,
            TargetPathRole,
            ReceivedBytesRole,
            TotalBytesRole,
            DownloadStateRole
        };

        enum class Mode { History, Bookmarks, Downloads };

        static constexpr int recentlyClosedLimit = 10;

        LibraryListModel(Mode mode, QObject *parent = nullptr);

        void setHistory(HistoryStore *history, RecentlyClosedStore *closed);
        void setBookmarks(BookmarkStore *bookmarks);
        void setDownloads(DownloadManager *downloads);
        void setFavicons(FaviconStore *favicons);

        int rowCount(const QModelIndex &parent = {}) const override;
        QVariant data(const QModelIndex &index, int role) const override;
        QHash<int, QByteArray> roleNames() const override;

        QString query() const;
        void setQuery(const QString &query);
        int itemCount() const;
        bool reorderable() const;

        Q_INVOKABLE void toggleSection(const QString &sectionId);
        Q_INVOKABLE bool moveItem(int fromRow, int toRow);

        static QString sectionForTimestamp(qint64 timestamp, qint64 now);

      signals:
        void queryChanged();
        void itemCountChanged();

      private:
        struct Row {
            bool header = false;
            QString sectionId;
            QString sectionTitle;
            QString key;
            QString title;
            QUrl url;
            qint64 timestamp = 0;
            qint64 closedId = 0;
            int sourceRow = -1;
            QString targetPath;
            qint64 receivedBytes = 0;
            qint64 totalBytes = 0;
            QString downloadState;
        };

        void watch(QAbstractItemModel *source);
        void scheduleRebuild();
        void rebuild();
        QList<Row> buildRows() const;
        bool matches(const QString &title, const QString &url) const;
        QString sectionTitle(const QString &sectionId) const;
        void appendSectioned(QList<Row> &rows, const QList<Row> &items) const;

        Mode m_mode;
        QPointer<HistoryStore> m_history;
        QPointer<RecentlyClosedStore> m_closed;
        QPointer<BookmarkStore> m_bookmarks;
        QPointer<DownloadManager> m_downloads;
        QPointer<FaviconStore> m_favicons;
        QString m_query;
        QSet<QString> m_collapsed;
        QList<Row> m_rows;
        int m_itemCount = 0;
        QTimer m_rebuildTimer;
    };

}

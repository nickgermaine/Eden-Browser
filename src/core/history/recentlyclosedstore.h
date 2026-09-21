#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QUrl>

struct sqlite3;

namespace eden::core {

    class RecentlyClosedStore final : public QAbstractListModel {
        Q_OBJECT

      public:
        enum Role { ClosedIdRole = Qt::UserRole + 1, TitleRole, UrlRole, ClosedAtRole };

        static constexpr int capacity = 25;

        explicit RecentlyClosedStore(QString databasePath = {}, QObject *parent = nullptr);
        ~RecentlyClosedStore() override;

        bool initialize(const QByteArray &key);
        bool isInitialized() const;

        int rowCount(const QModelIndex &parent = {}) const override;
        QVariant data(const QModelIndex &index, int role) const override;
        QHash<int, QByteArray> roleNames() const override;

        void add(const QUrl &url, const QString &title);
        QUrl take(qint64 closedId);
        void removeLatest(const QUrl &url);
        Q_INVOKABLE void clear();

      private:
        struct Entry {
            qint64 id = 0;
            QString title;
            QUrl url;
            qint64 closedAt = 0;
        };

        void removeRow(int row);

        QString m_databasePath;
        sqlite3 *m_database = nullptr;
        QList<Entry> m_entries;
        qint64 m_nextTransientId = -1;
    };

}

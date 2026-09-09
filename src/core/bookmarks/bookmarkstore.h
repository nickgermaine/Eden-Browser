#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QUrl>

struct sqlite3;

namespace eden::core {

    class BookmarkStore final : public QAbstractListModel {
        Q_OBJECT

      public:
        enum Role { TitleRole = Qt::UserRole + 1, UrlRole, FolderRole, CreatedAtRole };

        explicit BookmarkStore(QString databasePath, QObject *parent = nullptr);
        ~BookmarkStore() override;

        void initialize(const QByteArray &key);
        bool isInitialized() const;

        int rowCount(const QModelIndex &parent = {}) const override;
        QVariant data(const QModelIndex &index, int role) const override;
        QHash<int, QByteArray> roleNames() const override;

        Q_INVOKABLE bool contains(const QUrl &url) const;
        Q_INVOKABLE void add(const QUrl &url, const QString &title, const QString &folder = {});
        Q_INVOKABLE void remove(const QUrl &url);
        Q_INVOKABLE void toggle(const QUrl &url, const QString &title);
        QStringList search(const QString &query, int limit = 8) const;

      private:
        struct Entry {
            QString title;
            QUrl url;
            QString folder;
            qint64 createdAt;
        };

        void reload();

        QString m_databasePath;
        sqlite3 *m_database = nullptr;
        QList<Entry> m_entries;
    };

}

#pragma once

#include <QAbstractListModel>
#include <QSqlDatabase>
#include <QUrl>

namespace eden::core {

class BookmarkStore final : public QAbstractListModel {
    Q_OBJECT

  public:
    enum Role { TitleRole = Qt::UserRole + 1, UrlRole, FolderRole, CreatedAtRole };

    explicit BookmarkStore(QObject *parent = nullptr);
    ~BookmarkStore() override;

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

    void initialize();
    void reload();

    QString m_connectionName;
    QSqlDatabase m_database;
    QList<Entry> m_entries;
};

}

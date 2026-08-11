#pragma once

#include <QAbstractListModel>
#include <QSqlDatabase>
#include <QUrl>

namespace eden::core {

class HistoryStore final : public QAbstractListModel {
    Q_OBJECT

  public:
    enum Role { TitleRole = Qt::UserRole + 1, UrlRole, VisitedAtRole, VisitCountRole };

    explicit HistoryStore(QObject *parent = nullptr);
    ~HistoryStore() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void recordVisit(const QUrl &url, const QString &title);
    QStringList search(const QString &query, int limit = 8) const;
    Q_INVOKABLE void remove(const QUrl &url);
    Q_INVOKABLE void clear();

  private:
    struct Entry {
        QString title;
        QUrl url;
        qint64 visitedAt;
        int visitCount;
    };

    void initialize();
    void reload();

    QString m_connectionName;
    QSqlDatabase m_database;
    QList<Entry> m_entries;
};

}

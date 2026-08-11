#pragma once

#include <QAbstractListModel>
#include <QUrl>

namespace eden::core {

class DownloadManager final : public QAbstractListModel {
    Q_OBJECT

  public:
    enum Role { FileNameRole = Qt::UserRole + 1, SourceUrlRole, TargetPathRole, ReceivedBytesRole, TotalBytesRole, StateRole };

    explicit DownloadManager(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void beginDownload(int id, const QString &fileName, const QUrl &sourceUrl, const QString &targetPath, qint64 totalBytes);
    void updateDownload(int id, qint64 receivedBytes, qint64 totalBytes, const QString &state);
    Q_INVOKABLE void clearFinished();

  private:
    struct Download {
        int id;
        QString fileName;
        QUrl sourceUrl;
        QString targetPath;
        qint64 receivedBytes;
        qint64 totalBytes;
        QString state;
    };

    QList<Download> m_downloads;
};

}

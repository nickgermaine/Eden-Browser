#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QUrl>

struct sqlite3;

namespace eden::core {

    class DownloadManager final : public QAbstractListModel {
        Q_OBJECT

      public:
        enum Role {
            FileNameRole = Qt::UserRole + 1,
            SourceUrlRole,
            TargetPathRole,
            ReceivedBytesRole,
            TotalBytesRole,
            StateRole
        };

        explicit DownloadManager(const QString &databasePath = {}, QObject *parent = nullptr);
        ~DownloadManager() override;

        bool initialize(const QByteArray &key);
        bool isInitialized() const;
        int rowCount(const QModelIndex &parent = {}) const override;
        QVariant data(const QModelIndex &index, int role) const override;
        QHash<int, QByteArray> roleNames() const override;

        void beginDownload(
            quint64 id,
            const QString &fileName,
            const QUrl &sourceUrl,
            const QString &targetPath,
            qint64 totalBytes
        );
        void updateDownload(quint64 id, qint64 receivedBytes, qint64 totalBytes, const QString &state);
        int activeCount() const;
        Q_INVOKABLE void clearFinished();
        Q_INVOKABLE void openDownload(const QString &targetPath) const;
        Q_INVOKABLE void showInFolder(const QString &targetPath) const;

      private:
        struct Download {
            qint64 persistentId = 0;
            quint64 runtimeId = 0;
            QString fileName;
            QUrl sourceUrl;
            QString targetPath;
            qint64 receivedBytes = 0;
            qint64 totalBytes = -1;
            QString state;
            qint64 startedAt = 0;
            qint64 updatedAt = 0;
            qint64 persistedAt = 0;
        };

        void persistUpdate(Download &download);

        QString m_databasePath;
        sqlite3 *m_database = nullptr;
        QList<Download> m_downloads;
        bool m_initialized = false;
    };

}

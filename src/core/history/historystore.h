#pragma once

#include "core/profiles/profileerror.h"

#include <QAbstractListModel>
#include <QByteArray>
#include <QUrl>

#include <functional>
#include <memory>

namespace eden::core {

    class HistoryStore final : public QAbstractListModel {
        Q_OBJECT

      public:
        enum Role { TitleRole = Qt::UserRole + 1, UrlRole, VisitedAtRole, VisitCountRole };

        explicit HistoryStore(QString databasePath, QObject *parent = nullptr);
        ~HistoryStore() override;

        void initialize(const QByteArray &key);
        bool isInitialized() const;
        void close(std::function<void()> completion);
        bool hasPendingWrites() const;
        ProfileError error() const;

        int rowCount(const QModelIndex &parent = {}) const override;
        QVariant data(const QModelIndex &index, int role) const override;
        QHash<int, QByteArray> roleNames() const override;

        void recordVisit(const QUrl &url, const QString &title);
        QStringList search(const QString &query, int limit = 8) const;
        Q_INVOKABLE void remove(const QUrl &url);
        Q_INVOKABLE void clear();

      signals:
        void closed();
        void changed();
        void writesFinished();
        void errorChanged();

      private:
        struct Entry {
            QString title;
            QUrl url;
            qint64 visitedAt = 0;
            qint64 visitCount = 0;
        };

        void setError(ProfileError error);

        class Private;
        std::unique_ptr<Private> d;

        QString m_databasePath;
        ProfileError m_error = ProfileError::None;
        QList<Entry> m_entries;
    };

}

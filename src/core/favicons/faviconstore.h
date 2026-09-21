#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QUrl>

struct sqlite3;

namespace eden::core {

    class FaviconStore final : public QObject {
        Q_OBJECT
        Q_PROPERTY(int revision READ revision NOTIFY changed)

      public:
        explicit FaviconStore(QString databasePath = {}, QObject *parent = nullptr);
        ~FaviconStore() override;

        bool initialize(const QByteArray &key);
        bool isInitialized() const;
        int revision() const;

        Q_INVOKABLE QUrl iconFor(const QUrl &pageUrl) const;
        void record(const QUrl &pageUrl, const QUrl &iconUrl);
        void clear();

        static QString keyFor(const QUrl &pageUrl);
        static QUrl normalizedIcon(const QUrl &iconUrl);

      signals:
        void changed();

      private:
        QString m_databasePath;
        sqlite3 *m_database = nullptr;
        QHash<QString, QUrl> m_icons;
        int m_revision = 0;
    };

}

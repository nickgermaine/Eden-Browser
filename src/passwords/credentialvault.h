#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QDateTime>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

struct sqlite3;

namespace eden::passwords {

    class CredentialVault final : public QAbstractListModel {
        Q_OBJECT
        Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
        Q_PROPERTY(bool available READ available NOTIFY availableChanged)
        Q_PROPERTY(QString error READ error NOTIFY errorChanged)

      public:
        enum Role { IdRole = Qt::UserRole + 1, SiteRole, UsernameRole, UpdatedAtRole };
        Q_ENUM(Role)

        CredentialVault(QString databasePath, QString profileId, QObject *parent = nullptr);
        ~CredentialVault() override;

        bool initialize();
        int rowCount(const QModelIndex &parent = {}) const override;
        QVariant data(const QModelIndex &index, int role) const override;
        QHash<int, QByteArray> roleNames() const override;

        QString filter() const;
        void setFilter(const QString &filter);
        bool available() const;
        QString error() const;

        Q_INVOKABLE qint64
        saveCredential(qint64 id, const QString &site, const QString &username, const QString &password);
        Q_INVOKABLE bool removeCredential(qint64 id);
        Q_INVOKABLE QString revealPassword(qint64 id) const;
        Q_INVOKABLE QString usernameForCredential(qint64 id) const;
        Q_INVOKABLE QVariantList credentialsForUrl(const QUrl &url) const;
        Q_INVOKABLE QVariantList credentialSites(const QString &filter = {}) const;
        Q_INVOKABLE QVariantList credentialsForSite(const QString &site) const;
        Q_INVOKABLE QVariantList formProfiles() const;
        Q_INVOKABLE qint64 saveFormProfile(qint64 id, const QVariantMap &fields);
        Q_INVOKABLE bool removeFormProfile(qint64 id);
        QByteArray sealData(const QByteArray &plain, const QByteArray &purpose) const;
        QByteArray openData(const QByteArray &sealed, const QByteArray &purpose) const;
        QByteArray derivedKey(const QByteArray &purpose) const;

      signals:
        void filterChanged();
        void availableChanged();
        void errorChanged();
        void formProfilesChanged();

      private:
        struct Entry {
            qint64 id = 0;
            QString site;
            QString username;
            qint64 updatedAt = 0;
        };

        QByteArray loadOrCreateKey();
        QByteArray seal(const QByteArray &plain, const QByteArray &purpose) const;
        QByteArray open(const QByteArray &sealed, const QByteArray &purpose) const;
        QByteArray siteHash(const QString &site) const;
        QString normalizedSite(const QUrl &url) const;
        bool execute(const QByteArray &sql) const;
        void restrictFiles() const;
        void reload();
        void setError(const QString &error);
        void close();

        QString m_databasePath;
        QString m_profileId;
        QByteArray m_key;
        sqlite3 *m_database = nullptr;
        QList<Entry> m_allEntries;
        QList<Entry> m_entries;
        QString m_filter;
        QString m_error;
        bool m_available = false;
    };

}

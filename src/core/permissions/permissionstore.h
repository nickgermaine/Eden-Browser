#pragma once

#include <QByteArray>
#include <QObject>
#include <QUrl>
#include <QVariantList>

struct sqlite3;

namespace eden::core {

    class PermissionStore final : public QObject {
        Q_OBJECT
        Q_PROPERTY(bool available READ available NOTIFY availableChanged)

      public:
        enum Verdict { Ask, Granted, Denied };
        Q_ENUM(Verdict)

        PermissionStore(QString databasePath, QObject *parent = nullptr);
        ~PermissionStore() override;

        bool initialize(const QByteArray &key);
        bool available() const;
        Verdict verdict(const QUrl &origin, const QString &permission) const;

        Q_INVOKABLE QString state(const QUrl &origin, const QString &permission) const;
        Q_INVOKABLE QVariantList permissionsForOrigin(const QUrl &origin, bool includeDefaults = true) const;
        Q_INVOKABLE QVariantList sites(const QString &filter = {}) const;
        Q_INVOKABLE bool setPermission(const QUrl &origin, const QString &permission, bool allowed);
        Q_INVOKABLE bool resetPermission(const QUrl &origin, const QString &permission);
        Q_INVOKABLE bool resetOrigin(const QUrl &origin);
        bool noteRequested(const QUrl &origin, const QString &permission);

      signals:
        void availableChanged();
        void permissionsChanged(const QUrl &origin);

      private:
        static QUrl normalizedOrigin(const QUrl &origin);
        static QString canonicalPermission(const QString &permission);
        static QString permissionTitle(const QString &permission);
        static QStringList permissionTypes();
        static QString verdictName(Verdict verdict);
        bool upsert(const QUrl &origin, const QString &permission, Verdict verdict, bool preserveVerdict);
        void close();

        QString m_databasePath;
        sqlite3 *m_database = nullptr;
        bool m_available = false;
    };

}

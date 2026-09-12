#pragma once

#include "core/profiles/profiletypes.h"

#include <QAbstractListModel>
#include <QColor>
#include <QList>

namespace eden::core {

    class ProfileListModel final : public QAbstractListModel {
        Q_OBJECT
        Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

      public:
        enum Role {
            ProfileIdRole = Qt::UserRole + 1,
            DisplayNameRole,
            AvatarUrlRole,
            ColorRole,
            ProtectedProfileRole,
            SessionStateRole,
            LastUsedAtRole,
            CurrentProfileRole,
            CanDeleteRole,
            LifecycleRole
        };

        struct Row {
            ProfileSummary summary;
            QString sessionState;
            bool currentProfile = false;
            bool canDelete = false;
        };

        explicit ProfileListModel(QObject *parent = nullptr);

        int rowCount(const QModelIndex &parent = {}) const override;
        QVariant data(const QModelIndex &index, int role) const override;
        QHash<int, QByteArray> roleNames() const override;

        void update(QList<Row> rows);
        Q_INVOKABLE int indexOfProfile(const QString &profileId) const;
        Q_INVOKABLE QVariantMap profileAt(int row) const;

      signals:
        void countChanged();

      private:
        QList<Row> m_rows;
    };

}

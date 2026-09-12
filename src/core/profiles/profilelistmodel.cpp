#include "core/profiles/profilelistmodel.h"

#include "core/profiles/profilecolors.h"

namespace eden::core {

    ProfileListModel::ProfileListModel(QObject *parent)
        : QAbstractListModel(parent) {}

    int ProfileListModel::rowCount(const QModelIndex &parent) const {
        return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
    }

    QVariant ProfileListModel::data(const QModelIndex &index, int role) const {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
            return {};
        }
        const Row &row = m_rows.at(index.row());
        switch (role) {
        case ProfileIdRole:
            return row.summary.profileId;
        case DisplayNameRole:
            return row.summary.displayName;
        case AvatarUrlRole:
            return QStringLiteral("image://eden-profile-avatar/%1?revision=%2&seed=%3")
                .arg(row.summary.profileId)
                .arg(row.summary.avatarRevision)
                .arg(row.summary.colorSeed);
        case ColorRole:
            return profileColorForSeed(row.summary.colorSeed);
        case ProtectedProfileRole:
            return row.summary.protectedProfile;
        case SessionStateRole:
            return row.sessionState;
        case LastUsedAtRole:
            return row.summary.lastUsedAt;
        case CurrentProfileRole:
            return row.currentProfile;
        case CanDeleteRole:
            return row.canDelete;
        case LifecycleRole:
            return profileLifecycleName(row.summary.lifecycle);
        }
        return {};
    }

    QHash<int, QByteArray> ProfileListModel::roleNames() const {
        return {
            {ProfileIdRole, "profileId"},
            {DisplayNameRole, "displayName"},
            {AvatarUrlRole, "avatarUrl"},
            {ColorRole, "color"},
            {ProtectedProfileRole, "protectedProfile"},
            {SessionStateRole, "sessionState"},
            {LastUsedAtRole, "lastUsedAt"},
            {CurrentProfileRole, "currentProfile"},
            {CanDeleteRole, "canDelete"},
            {LifecycleRole, "lifecycle"}
        };
    }

    void ProfileListModel::update(QList<Row> rows) {
        const bool sameShape =
            rows.size() == m_rows.size() &&
            std::equal(rows.cbegin(), rows.cend(), m_rows.cbegin(), [](const Row &left, const Row &right) {
                return left.summary.profileId == right.summary.profileId;
            });
        if (!sameShape) {
            beginResetModel();
            m_rows = std::move(rows);
            endResetModel();
            emit countChanged();
            return;
        }
        for (qsizetype index = 0; index < rows.size(); ++index) {
            const Row &incoming = rows.at(index);
            const Row &existing = m_rows.at(index);
            const bool changed = incoming.summary.displayName != existing.summary.displayName ||
                                 incoming.summary.colorSeed != existing.summary.colorSeed ||
                                 incoming.summary.avatarRevision != existing.summary.avatarRevision ||
                                 incoming.summary.protectedProfile != existing.summary.protectedProfile ||
                                 incoming.summary.lastUsedAt != existing.summary.lastUsedAt ||
                                 incoming.summary.lifecycle != existing.summary.lifecycle ||
                                 incoming.sessionState != existing.sessionState ||
                                 incoming.currentProfile != existing.currentProfile ||
                                 incoming.canDelete != existing.canDelete;
            if (changed) {
                m_rows[index] = incoming;
                emit dataChanged(this->index(static_cast<int>(index)), this->index(static_cast<int>(index)));
            }
        }
    }

    int ProfileListModel::indexOfProfile(const QString &profileId) const {
        for (qsizetype index = 0; index < m_rows.size(); ++index) {
            if (m_rows.at(index).summary.profileId == profileId) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    QVariantMap ProfileListModel::profileAt(int row) const {
        if (row < 0 || row >= m_rows.size()) {
            return {};
        }
        QVariantMap value;
        const QModelIndex modelIndex = index(row);
        const QHash<int, QByteArray> roles = roleNames();
        for (auto iterator = roles.cbegin(); iterator != roles.cend(); ++iterator) {
            value.insert(QString::fromUtf8(iterator.value()), data(modelIndex, iterator.key()));
        }
        return value;
    }

}

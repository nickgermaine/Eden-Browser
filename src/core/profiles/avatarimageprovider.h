#pragma once

#include <QHash>
#include <QMutex>
#include <QQuickImageProvider>

namespace eden::core {

    class AvatarImageProvider final : public QQuickImageProvider {
      public:
        static constexpr auto providerName = "eden-profile-avatar";

        AvatarImageProvider();

        void updateProfile(
            const QString &profileId,
            const QString &displayName,
            quint32 colorSeed,
            const QString &avatarPath
        );
        void removeProfile(const QString &profileId);

        QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

        static QImage generatedAvatar(const QString &displayName, quint32 colorSeed, int side);

      private:
        struct Metadata {
            QString displayName;
            quint32 colorSeed = 0;
            QString avatarPath;
        };

        mutable QMutex m_mutex;
        QHash<QString, Metadata> m_profiles;
    };

}

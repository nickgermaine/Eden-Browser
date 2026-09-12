#include "core/profiles/avatarimageprovider.h"

#include "core/profiles/profilecolors.h"
#include "core/profiles/profileid.h"

#include <QFileInfo>
#include <QFont>
#include <QMutexLocker>
#include <QPainter>

namespace eden::core {

    AvatarImageProvider::AvatarImageProvider()
        : QQuickImageProvider(QQuickImageProvider::Image) {}

    void AvatarImageProvider::updateProfile(
        const QString &profileId,
        const QString &displayName,
        quint32 colorSeed,
        const QString &avatarPath
    ) {
        QMutexLocker locker(&m_mutex);
        m_profiles.insert(profileId, Metadata{displayName, colorSeed, avatarPath});
    }

    void AvatarImageProvider::removeProfile(const QString &profileId) {
        QMutexLocker locker(&m_mutex);
        m_profiles.remove(profileId);
    }

    QImage AvatarImageProvider::generatedAvatar(const QString &displayName, quint32 colorSeed, int side) {
        const QColor background = profileColorForSeed(colorSeed);
        const QColor foreground = profileForegroundFor(background);
        QImage image(side, side, QImage::Format_ARGB32_Premultiplied);
        image.fill(background);
        const QString grapheme = firstGrapheme(displayName);
        if (!grapheme.isEmpty()) {
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setRenderHint(QPainter::TextAntialiasing);
            QFont font;
            font.setFamilies({QStringLiteral("Roboto Flex"), QStringLiteral("Sans Serif")});
            font.setPixelSize(qMax(8, side * 42 / 100));
            font.setWeight(QFont::DemiBold);
            painter.setFont(font);
            painter.setPen(foreground);
            painter.drawText(image.rect(), Qt::AlignCenter, grapheme);
        }
        return image;
    }

    QImage AvatarImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize) {
        const QString profileId = id.section(QLatin1Char('?'), 0, 0);
        if (!ProfileId::isCanonical(profileId)) {
            const QImage fallback = generatedAvatar(QString(), 0, 128);
            if (size) {
                *size = fallback.size();
            }
            return fallback;
        }
        Metadata metadata;
        {
            QMutexLocker locker(&m_mutex);
            metadata = m_profiles.value(profileId);
        }
        const int side = requestedSize.isValid() && requestedSize.width() > 0
                             ? qBound(16, qMax(requestedSize.width(), requestedSize.height()), 512)
                             : 128;
        QImage image;
        if (!metadata.avatarPath.isEmpty() && QFileInfo::exists(metadata.avatarPath)) {
            image = QImage(metadata.avatarPath);
        }
        if (image.isNull()) {
            image = generatedAvatar(metadata.displayName, metadata.colorSeed, side);
        } else if (image.width() != side) {
            image = image.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        if (size) {
            *size = image.size();
        }
        return image;
    }

}

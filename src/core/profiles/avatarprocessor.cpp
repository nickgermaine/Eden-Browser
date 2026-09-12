#include "core/profiles/avatarprocessor.h"

#include "core/profiles/profilepaths.h"

#include <QColorSpace>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QCoreApplication>
#include <QPointer>
#include <QSaveFile>
#include <QThreadPool>

namespace eden::core {

    static QSize boundedDecodeSize(const QSize &sourceSize) {
        if (!sourceSize.isValid() || sourceSize.isEmpty()) {
            return {};
        }
        const int minimumSide = std::min(sourceSize.width(), sourceSize.height());
        if (minimumSide <= AvatarProcessor::normalizedDimension * 2) {
            return {};
        }
        const double scale = static_cast<double>(AvatarProcessor::normalizedDimension * 2) / minimumSide;
        return QSize(
            std::max(1, static_cast<int>(sourceSize.width() * scale)),
            std::max(1, static_cast<int>(sourceSize.height() * scale))
        );
    }

    ProfileError AvatarProcessor::processBlocking(const QUrl &sourceUrl, const QString &targetPath) {
        if (!sourceUrl.isLocalFile()) {
            return ProfileError::AvatarUnreadable;
        }
        const QString sourcePath = sourceUrl.toLocalFile();
        const QFileInfo info(sourcePath);
        if (!info.exists() || info.isSymLink() || !info.isFile()) {
            return ProfileError::AvatarUnreadable;
        }
        if (info.size() > maximumSourceBytes) {
            return ProfileError::AvatarTooLarge;
        }
        QImageReader reader(sourcePath);
        reader.setAutoTransform(true);
        const QSize reportedSize = reader.size();
        if (reportedSize.isValid() &&
            (reportedSize.width() > maximumSourceDimension || reportedSize.height() > maximumSourceDimension)) {
            return ProfileError::AvatarTooLarge;
        }
        if (reader.supportsOption(QImageIOHandler::ScaledSize)) {
            const QSize bounded = boundedDecodeSize(reportedSize);
            if (bounded.isValid()) {
                reader.setScaledSize(bounded);
            }
        }
        QImage decoded = reader.read();
        if (decoded.isNull()) {
            return ProfileError::AvatarDecodeFailed;
        }
        if (decoded.width() > maximumSourceDimension || decoded.height() > maximumSourceDimension) {
            return ProfileError::AvatarTooLarge;
        }
        if (decoded.colorSpace().isValid() && decoded.colorSpace() != QColorSpace(QColorSpace::SRgb)) {
            decoded.convertToColorSpace(QColorSpace::SRgb);
        }
        decoded = decoded.convertToFormat(QImage::Format_RGBA8888);
        if (decoded.isNull()) {
            return ProfileError::AvatarDecodeFailed;
        }
        const int side = std::min(decoded.width(), decoded.height());
        const QRect crop((decoded.width() - side) / 2, (decoded.height() - side) / 2, side, side);
        QImage normalized =
            decoded.copy(crop)
                .scaled(normalizedDimension, normalizedDimension, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (normalized.isNull()) {
            return ProfileError::AvatarDecodeFailed;
        }
        QImage plain(normalized.size(), QImage::Format_RGBA8888);
        plain.setColorSpace(QColorSpace());
        std::memcpy(plain.bits(), normalized.constBits(), static_cast<size_t>(normalized.sizeInBytes()));
        QSaveFile file(targetPath);
        if (!file.open(QIODevice::WriteOnly)) {
            return ProfileError::AvatarWriteFailed;
        }
        if (!plain.save(&file, "PNG") || !file.commit()) {
            return ProfileError::AvatarWriteFailed;
        }
        ProfilePaths::restrictFile(targetPath);
        return ProfileError::None;
    }

    void
    AvatarProcessor::process(const QUrl &sourceUrl, const QString &targetPath, QObject *receiver, Callback callback) {
        QPointer<QObject> guard(receiver);
        QThreadPool::globalInstance()->start([sourceUrl, targetPath, guard, callback = std::move(callback)] {
            const ProfileError error = processBlocking(sourceUrl, targetPath);
            if (!guard) {
                return;
            }
            QMetaObject::invokeMethod(guard.data(), [callback, error] { callback(error); }, Qt::QueuedConnection);
        });
    }

}

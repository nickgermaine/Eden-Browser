#include "engine/cef/osrtexture.h"

#include <QVarLengthArray>
#include <rhi/qrhi.h>

namespace eden::engine::cef {

    QRegion boundedTextureDamage(const QRegion &damage, const QSize &size) {
        const QRect bounds(QPoint(), size);
        const QRegion clipped = damage & bounds;
        return clipped.rectCount() > 64 ? QRegion(clipped.boundingRect()) : clipped;
    }

    OsrTexture::OsrTexture() = default;

    OsrTexture::~OsrTexture() = default;

    void OsrTexture::setFrame(QImage image, const QRegion &damage) {
        if (image.isNull()) {
            return;
        }
        if (image.format() != QImage::Format_ARGB32_Premultiplied && image.format() != QImage::Format_RGB32) {
            image = std::move(image).convertToFormat(QImage::Format_ARGB32_Premultiplied);
            if (image.isNull()) {
                return;
            }
        }
        if (m_size != image.size() || !m_texture) {
            m_pendingDamage = image.rect();
        } else {
            m_pendingDamage = boundedTextureDamage(m_pendingDamage + damage, image.size());
        }
        m_size = image.size();
        m_pendingImage = std::move(image);
    }

    qint64 OsrTexture::comparisonKey() const {
        return static_cast<qint64>(reinterpret_cast<quintptr>(this));
    }

    QRhiTexture *OsrTexture::rhiTexture() const {
        return m_texture.get();
    }

    QSize OsrTexture::textureSize() const {
        return m_size;
    }

    bool OsrTexture::hasAlphaChannel() const {
        return true;
    }

    bool OsrTexture::hasMipmaps() const {
        return false;
    }

    void OsrTexture::commitTextureOperations(QRhi *rhi, QRhiResourceUpdateBatch *updates) {
        if (!rhi || !updates || m_pendingImage.isNull() || m_pendingDamage.isEmpty()) {
            return;
        }
        if (!m_texture || m_texture->pixelSize() != m_size) {
            const QRhiTexture::Format format =
                rhi->isTextureFormatSupported(QRhiTexture::BGRA8) ? QRhiTexture::BGRA8 : QRhiTexture::RGBA8;
            std::unique_ptr<QRhiTexture> replacement(rhi->newTexture(format, m_size));
            if (!replacement->create()) {
                return;
            }
            m_texture = std::move(replacement);
            m_pendingDamage = QRect(QPoint(), m_size);
        }
        QVarLengthArray<QRhiTextureUploadEntry, 16> entries;
        entries.reserve(m_pendingDamage.rectCount());
        for (const QRect &rect : m_pendingDamage) {
            QRhiTextureSubresourceUploadDescription source;
            if (m_texture->format() == QRhiTexture::BGRA8) {
                source.setImage(m_pendingImage);
                source.setSourceTopLeft(rect.topLeft());
            } else {
                QImage image = rect == m_pendingImage.rect() ? m_pendingImage : m_pendingImage.copy(rect);
                image = std::move(image).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
                if (image.isNull()) {
                    return;
                }
                source.setImage(image);
            }
            source.setSourceSize(rect.size());
            source.setDestinationTopLeft(rect.topLeft());
            entries.append(QRhiTextureUploadEntry(0, 0, source));
        }
        QRhiTextureUploadDescription upload;
        upload.setEntries(entries.cbegin(), entries.cend());
        updates->uploadTexture(m_texture.get(), upload);
        m_pendingImage = {};
        m_pendingDamage = {};
    }

}

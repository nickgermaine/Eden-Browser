#pragma once

#include <QImage>
#include <QRegion>
#include <QSGTexture>

#include <memory>

namespace eden::engine::cef {

    QRegion boundedTextureDamage(const QRegion &damage, const QSize &size);

    class OsrTexture final : public QSGTexture {
      public:
        OsrTexture();
        ~OsrTexture() override;

        void setFrame(QImage image, const QRegion &damage);

        qint64 comparisonKey() const override;
        QRhiTexture *rhiTexture() const override;
        QSize textureSize() const override;
        bool hasAlphaChannel() const override;
        bool hasMipmaps() const override;
        void commitTextureOperations(QRhi *rhi, QRhiResourceUpdateBatch *updates) override;

      private:
        std::unique_ptr<QRhiTexture> m_texture;
        QImage m_pendingImage;
        QRegion m_pendingDamage;
        QSize m_size;
    };

}

#include "core/window/thumbnailcache.h"

#include <QMutex>
#include <QMutexLocker>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QUuid>

#include <algorithm>

namespace eden::core {

    class ThumbnailImages final {
      public:
        QMutex mutex;
        QHash<QString, QImage> images;
    };

    class ThumbnailImageProvider final : public QQuickImageProvider {
      public:
        explicit ThumbnailImageProvider(std::shared_ptr<ThumbnailImages> images)
            : QQuickImageProvider(QQuickImageProvider::Image),
              m_images(std::move(images)) {}

        QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override {
            QImage image;
            {
                const QMutexLocker lock(&m_images->mutex);
                image = m_images->images.value(id);
            }
            if (size) {
                *size = image.size();
            }
            if (!image.isNull() && requestedSize.isValid() && requestedSize != image.size()) {
                return image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
            return image;
        }

      private:
        std::shared_ptr<ThumbnailImages> m_images;
    };

    ThumbnailCache::ThumbnailCache(qsizetype maximumBytes)
        : m_images(std::make_shared<ThumbnailImages>()),
          m_providerName("eden-thumbnails-" + QUuid::createUuid().toString(QUuid::WithoutBraces)),
          m_maximumBytes(std::max<qsizetype>(0, maximumBytes)) {}

    ThumbnailCache::~ThumbnailCache() {
        clear();
        if (m_engine) {
            m_engine->removeImageProvider(m_providerName);
        }
    }

    void ThumbnailCache::registerProvider(QQmlEngine *engine) {
        if (m_engine == engine) {
            return;
        }
        if (m_engine) {
            m_engine->removeImageProvider(m_providerName);
        }
        m_engine = engine;
        if (engine) {
            engine->addImageProvider(m_providerName, createImageProvider());
        }
    }

    QQuickImageProvider *ThumbnailCache::createImageProvider() const {
        return new ThumbnailImageProvider(m_images);
    }

    void ThumbnailCache::putMetadata(quint64 tabId, QVariantMap preview) {
        Entry &entry = m_entries[tabId];
        const QVariant thumbnail = entry.preview.value("thumbnail");
        entry.preview = std::move(preview);
        if (thumbnail.isValid()) {
            entry.preview.insert("thumbnail", thumbnail);
        }
        touch(tabId);
        trim();
    }

    bool ThumbnailCache::putImage(quint64 tabId, const QImage &image) {
        auto found = m_entries.find(tabId);
        if (found == m_entries.end() || image.isNull()) {
            return false;
        }
        const QString imageId = QString::number(tabId) + '/' + QString::number(++m_revision);
        const QString thumbnail = "image://" + m_providerName + '/' + imageId;
        {
            const QMutexLocker lock(&m_images->mutex);
            const QString previous = QUrl(found->preview.value("thumbnail").toString()).path().mid(1);
            m_images->images.remove(previous);
            m_images->images.insert(imageId, image);
        }
        m_byteSize -= found->imageBytes;
        found->imageBytes = image.sizeInBytes();
        m_byteSize += found->imageBytes;
        found->preview.insert("thumbnail", thumbnail);
        touch(tabId);
        trim();
        return m_entries.contains(tabId);
    }

    QVariantMap ThumbnailCache::value(quint64 tabId) {
        const auto found = m_entries.constFind(tabId);
        if (found == m_entries.cend()) {
            return {};
        }
        QVariantMap preview = found->preview;
        touch(tabId);
        return preview;
    }

    void ThumbnailCache::remove(quint64 tabId) {
        const auto found = m_entries.find(tabId);
        if (found == m_entries.end()) {
            return;
        }
        {
            const QMutexLocker lock(&m_images->mutex);
            m_images->images.remove(QUrl(found->preview.value("thumbnail").toString()).path().mid(1));
        }
        m_byteSize -= found->imageBytes;
        m_entries.erase(found);
        m_recent.removeAll(tabId);
    }

    void ThumbnailCache::clear() {
        {
            const QMutexLocker lock(&m_images->mutex);
            m_images->images.clear();
        }
        m_entries.clear();
        m_recent.clear();
        m_byteSize = 0;
    }

    qsizetype ThumbnailCache::byteSize() const {
        return m_byteSize;
    }

    qsizetype ThumbnailCache::maximumBytes() const {
        return m_maximumBytes;
    }

    qsizetype ThumbnailCache::count() const {
        return m_entries.size();
    }

    void ThumbnailCache::touch(quint64 tabId) {
        m_recent.removeAll(tabId);
        m_recent.prepend(tabId);
    }

    void ThumbnailCache::trim() {
        while (m_byteSize > m_maximumBytes && !m_recent.isEmpty()) {
            remove(m_recent.constLast());
        }
    }

}

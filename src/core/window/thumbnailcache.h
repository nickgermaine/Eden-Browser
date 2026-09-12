#pragma once

#include <QHash>
#include <QImage>
#include <QList>
#include <QPointer>
#include <QVariantMap>
#include <memory>

class QQmlEngine;
class QQuickImageProvider;

namespace eden::core {

    class ThumbnailImages;

    class ThumbnailCache final {
      public:
        explicit ThumbnailCache(qsizetype maximumBytes = 32 * 1024 * 1024);
        ~ThumbnailCache();
        Q_DISABLE_COPY_MOVE(ThumbnailCache)

        void registerProvider(QQmlEngine *engine);
        QQuickImageProvider *createImageProvider() const;

        void putMetadata(quint64 tabId, QVariantMap preview);
        bool putImage(quint64 tabId, const QImage &image);
        QVariantMap value(quint64 tabId);
        void remove(quint64 tabId);
        void clear();
        qsizetype byteSize() const;
        qsizetype maximumBytes() const;
        qsizetype count() const;

      private:
        struct Entry {
            QVariantMap preview;
            qsizetype imageBytes = 0;
        };

        void touch(quint64 tabId);
        void trim();

        std::shared_ptr<ThumbnailImages> m_images;
        QPointer<QQmlEngine> m_engine;
        QString m_providerName;
        quint64 m_revision = 0;
        QHash<quint64, Entry> m_entries;
        QList<quint64> m_recent;
        qsizetype m_byteSize = 0;
        qsizetype m_maximumBytes;
    };

}

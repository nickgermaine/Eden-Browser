#pragma once

#include <QHash>
#include <QImage>
#include <QList>
#include <QVariantMap>

namespace eden::core {

    class ThumbnailCache final {
      public:
        explicit ThumbnailCache(qsizetype maximumBytes = 32 * 1024 * 1024);

        void putMetadata(quint64 tabId, QVariantMap preview);
        void putImage(quint64 tabId, const QImage &image);
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

        QHash<quint64, Entry> m_entries;
        QList<quint64> m_recent;
        qsizetype m_byteSize = 0;
        qsizetype m_maximumBytes;
    };

}

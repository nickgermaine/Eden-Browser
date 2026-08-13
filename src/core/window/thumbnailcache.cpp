#include "core/window/thumbnailcache.h"

#include <QBuffer>

#include <algorithm>

namespace eden::core {

ThumbnailCache::ThumbnailCache(qsizetype maximumBytes)
    : m_maximumBytes(std::max<qsizetype>(0, maximumBytes)) {}

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

void ThumbnailCache::putImage(quint64 tabId, const QImage &image) {
    auto found = m_entries.find(tabId);
    if (found == m_entries.end() || image.isNull()) {
        return;
    }
    QByteArray encoded;
    QBuffer buffer(&encoded);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "JPEG", 82)) {
        return;
    }
    const QString thumbnail = QString("data:image/jpeg;base64,%1").arg(QString::fromLatin1(encoded.toBase64()));
    m_byteSize -= found->imageBytes;
    found->imageBytes = std::max<qsizetype>(image.sizeInBytes(), thumbnail.size() * qsizetype(sizeof(QChar)));
    m_byteSize += found->imageBytes;
    found->preview.insert("thumbnail", thumbnail);
    touch(tabId);
    trim();
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
    m_byteSize -= found->imageBytes;
    m_entries.erase(found);
    m_recent.removeAll(tabId);
}

void ThumbnailCache::clear() {
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

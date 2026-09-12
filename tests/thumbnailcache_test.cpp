#include "core/window/thumbnailcache.h"

#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QtTest>

class ThumbnailCacheTest final : public QObject {
    Q_OBJECT

  private slots:
    void evictsLeastRecentlyUsedImages();
    void preservesImageAcrossMetadataRefresh();
    void preservesPixelsWithoutEncoding();
    void invalidatesReplacedAndRemovedImages();
    void releasesImagesWhenCacheCloses();
    void registersWithTheQmlEngine();
};

void ThumbnailCacheTest::evictsLeastRecentlyUsedImages() {
    eden::core::ThumbnailCache cache(80000);
    const QImage image(100, 100, QImage::Format_ARGB32);
    cache.putMetadata(1, {{"title", "One"}});
    cache.putImage(1, image);
    cache.putMetadata(2, {{"title", "Two"}});
    cache.putImage(2, image);
    QVERIFY(!cache.value(1).isEmpty());
    cache.putMetadata(3, {{"title", "Three"}});
    cache.putImage(3, image);
    QVERIFY(cache.value(2).isEmpty());
    QVERIFY(!cache.value(1).value("thumbnail").toString().isEmpty());
    QVERIFY(!cache.value(3).value("thumbnail").toString().isEmpty());
    QCOMPARE(cache.byteSize(), qsizetype(80000));
}

void ThumbnailCacheTest::preservesImageAcrossMetadataRefresh() {
    eden::core::ThumbnailCache cache;
    QImage image(8, 8, QImage::Format_ARGB32);
    image.fill(Qt::red);
    cache.putMetadata(9, {{"title", "Before"}});
    cache.putImage(9, image);
    const QString thumbnail = cache.value(9).value("thumbnail").toString();
    cache.putMetadata(9, {{"title", "After"}});
    QCOMPARE(cache.value(9).value("title").toString(), QString("After"));
    QCOMPARE(cache.value(9).value("thumbnail").toString(), thumbnail);
}

void ThumbnailCacheTest::preservesPixelsWithoutEncoding() {
    eden::core::ThumbnailCache cache;
    std::unique_ptr<QQuickImageProvider> provider(cache.createImageProvider());
    QImage image(128, 96, QImage::Format_ARGB32);
    image.fill(QColor(12, 98, 241, 83));
    image.setPixelColor(67, 42, QColor(217, 71, 8, 143));
    cache.putMetadata(1, {{"title", "Pixels"}});
    QVERIFY(cache.putImage(1, image));
    const QUrl source(cache.value(1).value("thumbnail").toString());
    QCOMPARE(source.scheme(), QString("image"));
    QSize size;
    const QImage supplied = provider->requestImage(source.path().mid(1), &size, {});
    QCOMPARE(size, image.size());
    QCOMPARE(supplied, image);
    QCOMPARE(supplied.constBits(), image.constBits());
    QCOMPARE(cache.byteSize(), image.sizeInBytes());
}

void ThumbnailCacheTest::invalidatesReplacedAndRemovedImages() {
    eden::core::ThumbnailCache cache;
    std::unique_ptr<QQuickImageProvider> provider(cache.createImageProvider());
    QImage image(8, 8, QImage::Format_ARGB32);
    image.fill(Qt::red);
    cache.putMetadata(1, {});
    QVERIFY(cache.putImage(1, image));
    const QString first = QUrl(cache.value(1).value("thumbnail").toString()).path().mid(1);
    image.fill(Qt::blue);
    QVERIFY(cache.putImage(1, image));
    const QString second = QUrl(cache.value(1).value("thumbnail").toString()).path().mid(1);
    QVERIFY(first != second);
    QVERIFY(provider->requestImage(first, nullptr, {}).isNull());
    QCOMPARE(provider->requestImage(second, nullptr, {}), image);
    cache.remove(1);
    QVERIFY(provider->requestImage(second, nullptr, {}).isNull());
    QCOMPARE(cache.byteSize(), qsizetype(0));
}

void ThumbnailCacheTest::releasesImagesWhenCacheCloses() {
    std::unique_ptr<QQuickImageProvider> provider;
    QString id;
    {
        eden::core::ThumbnailCache cache;
        provider.reset(cache.createImageProvider());
        QImage image(8, 8, QImage::Format_ARGB32);
        image.fill(Qt::green);
        cache.putMetadata(1, {});
        QVERIFY(cache.putImage(1, image));
        id = QUrl(cache.value(1).value("thumbnail").toString()).path().mid(1);
        QVERIFY(!provider->requestImage(id, nullptr, {}).isNull());
    }
    QVERIFY(provider->requestImage(id, nullptr, {}).isNull());
}

void ThumbnailCacheTest::registersWithTheQmlEngine() {
    QQmlEngine engine;
    QString name;
    {
        eden::core::ThumbnailCache cache;
        cache.registerProvider(&engine);
        QImage image(8, 8, QImage::Format_ARGB32);
        image.fill(Qt::green);
        cache.putMetadata(1, {});
        QVERIFY(cache.putImage(1, image));
        const QUrl source(cache.value(1).value("thumbnail").toString());
        name = source.host();
        auto *provider = static_cast<QQuickImageProvider *>(engine.imageProvider(name));
        QVERIFY(provider);
        QCOMPARE(provider->requestImage(source.path().mid(1), nullptr, {}), image);
    }
    QVERIFY(!engine.imageProvider(name));
    eden::core::ThumbnailCache cache;
    {
        QQmlEngine shorterLivedEngine;
        cache.registerProvider(&shorterLivedEngine);
    }
    cache.registerProvider(&engine);
}

QTEST_GUILESS_MAIN(ThumbnailCacheTest)

#include "thumbnailcache_test.moc"

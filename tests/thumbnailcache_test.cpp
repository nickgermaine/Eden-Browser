#include "core/window/thumbnailcache.h"

#include <QtTest>

class ThumbnailCacheTest final : public QObject {
    Q_OBJECT

  private slots:
    void evictsLeastRecentlyUsedImages();
    void preservesImageAcrossMetadataRefresh();
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

QTEST_GUILESS_MAIN(ThumbnailCacheTest)

#include "thumbnailcache_test.moc"

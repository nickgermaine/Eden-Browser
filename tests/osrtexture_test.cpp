#include "engine/cef/osrtexture.h"

#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QtTest>
#include <rhi/qrhi.h>

#include <atomic>

using eden::engine::cef::boundedTextureDamage;
using eden::engine::cef::OsrTexture;

class TextureItem final : public QQuickItem {
  public:
    explicit TextureItem(QQuickWindow *window)
        : QQuickItem(window->contentItem()) {
        setFlag(ItemHasContents);
        setSize(window->size());
        connect(
            window,
            &QQuickWindow::afterRendering,
            this,
            [this] {
                if (m_texture && m_texture->rhiTexture()) {
                    resourceId.store(m_texture->rhiTexture()->globalResourceId());
                }
            },
            Qt::DirectConnection
        );
        connect(
            window,
            &QQuickWindow::sceneGraphInvalidated,
            this,
            [this] { resourceId.store(0); },
            Qt::DirectConnection
        );
    }

    void present(QImage image, const QRegion &damage) {
        m_image = std::move(image);
        m_frames.append({m_image, damage});
        update();
    }

    std::atomic<quint64> resourceId = 0;

  protected:
    QSGNode *updatePaintNode(QSGNode *previous, UpdatePaintNodeData *) override {
        auto *node = static_cast<QSGSimpleTextureNode *>(previous);
        const bool created = !node;
        if (created) {
            node = new QSGSimpleTextureNode;
            m_texture = new OsrTexture;
            m_texture->setFrame(m_image, m_image.rect());
        }
        for (const auto &[image, damage] : m_frames) {
            m_texture->setFrame(image, damage);
        }
        m_frames.clear();
        if (created) {
            node->setTexture(m_texture);
            node->setOwnsTexture(true);
        }
        node->setRect(boundingRect());
        node->markDirty(QSGNode::DirtyMaterial);
        return node;
    }

  private:
    QImage m_image;
    QList<std::pair<QImage, QRegion>> m_frames;
    QPointer<OsrTexture> m_texture;
};

static QColor pixelAt(QQuickWindow &window, const QPoint &position) {
    const QImage image = window.grabWindow();
    return image.isNull() ? QColor() : image.pixelColor((QPointF(position) * window.devicePixelRatio()).toPoint());
}

class OsrTextureTest final : public QObject {
    Q_OBJECT

  private slots:
    void uploadsOnlyDamageAndReusesStorage() {
        QQuickWindow window;
        window.setColor(Qt::black);
        window.resize(64, 48);
        TextureItem item(&window);
        QImage initial(window.size(), QImage::Format_ARGB32_Premultiplied);
        initial.fill(QColor("#c02040"));
        item.present(initial, initial.rect());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_COMPARE(pixelAt(window, QPoint(40, 30)), QColor("#c02040"));
        const quint64 firstResource = item.resourceId.load();
        QVERIFY(firstResource != 0);
        QImage next(window.size(), QImage::Format_ARGB32_Premultiplied);
        next.fill(QColor("#6040a0"));
        const QRect damaged(4, 4, 12, 12);
        for (int y = damaged.top(); y <= damaged.bottom(); ++y) {
            for (int x = damaged.left(); x <= damaged.right(); ++x) {
                next.setPixelColor(x, y, QColor("#208040"));
            }
        }
        item.present(next, damaged);
        QTRY_COMPARE(pixelAt(window, QPoint(8, 8)), QColor("#208040"));
        QCOMPARE(pixelAt(window, QPoint(40, 30)), QColor("#c02040"));
        QCOMPARE(item.resourceId.load(), firstResource);
        item.present(next, {});
        QTest::qWait(40);
        QCOMPARE(pixelAt(window, QPoint(40, 30)), QColor("#c02040"));
        QCOMPARE(item.resourceId.load(), firstResource);
    }

    void combinesDamageBeforeTheNextUpload() {
        QQuickWindow window;
        window.resize(64, 48);
        TextureItem item(&window);
        QImage image(window.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(QColor("#c02040"));
        item.present(image, image.rect());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_COMPARE(pixelAt(window, QPoint(40, 30)), QColor("#c02040"));
        const quint64 firstResource = item.resourceId.load();
        image.setPixelColor(8, 8, QColor("#208040"));
        item.present(image, QRect(8, 8, 1, 1));
        image.setPixelColor(40, 30, QColor("#2040a0"));
        item.present(image, QRect(40, 30, 1, 1));
        QTRY_COMPARE(pixelAt(window, QPoint(8, 8)), QColor("#208040"));
        QTRY_COMPARE(pixelAt(window, QPoint(40, 30)), QColor("#2040a0"));
        QCOMPARE(item.resourceId.load(), firstResource);
    }

    void resizingRecreatesStorageAndUploadsTheFullImage() {
        QQuickWindow window;
        window.resize(64, 48);
        TextureItem item(&window);
        QImage initial(64, 48, QImage::Format_ARGB32_Premultiplied);
        initial.fill(QColor("#c02040"));
        item.present(initial, initial.rect());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_COMPARE(pixelAt(window, QPoint(40, 30)), QColor("#c02040"));
        const quint64 firstResource = item.resourceId.load();
        QImage resized(32, 24, QImage::Format_ARGB32_Premultiplied);
        resized.fill(QColor("#208040"));
        item.present(resized, QRect(0, 0, 1, 1));
        QTRY_COMPARE(pixelAt(window, QPoint(40, 30)), QColor("#208040"));
        QVERIFY(item.resourceId.load() != firstResource);
    }

    void sceneGraphRecreationRestoresTheLatestImage() {
        QQuickWindow window;
        window.setPersistentSceneGraph(false);
        window.setPersistentGraphics(false);
        window.resize(64, 48);
        TextureItem item(&window);
        QImage image(window.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(QColor("#208040"));
        item.present(image, image.rect());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_COMPARE(pixelAt(window, QPoint(40, 30)), QColor("#208040"));
        const quint64 firstResource = item.resourceId.load();
        window.hide();
        window.releaseResources();
        QTRY_COMPARE(item.resourceId.load(), quint64(0));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_COMPARE(pixelAt(window, QPoint(40, 30)), QColor("#208040"));
        QVERIFY(item.resourceId.load() != firstResource);
    }

    void preservesPremultipliedAlpha() {
        QQuickWindow window;
        window.setColor(Qt::black);
        window.resize(64, 48);
        TextureItem item(&window);
        QImage image(window.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(QColor(64, 128, 192, 128));
        item.present(image, image.rect());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_COMPARE(pixelAt(window, QPoint(40, 30)), QColor(32, 64, 96));
    }

    void clipsDamageAndBoundsRegionComplexity() {
        const QSize size(512, 512);
        QRegion damage(QRect(-10, -10, 20, 20));
        QCOMPARE(boundedTextureDamage(damage, size), QRegion(QRect(0, 0, 10, 10)));
        for (int index = 0; index < 100; ++index) {
            damage += QRect(20 + index * 4, 40, 1, 1);
        }
        const QRegion bounded = boundedTextureDamage(damage, size);
        QVERIFY(bounded.rectCount() <= 64);
        QVERIFY((damage & QRect(QPoint(), size)).subtracted(bounded).isEmpty());
        QVERIFY(bounded.subtracted(QRect(QPoint(), size)).isEmpty());
    }
};

QTEST_MAIN(OsrTextureTest)

#include "osrtexture_test.moc"

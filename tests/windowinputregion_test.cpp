#include "core/window/windowinputregion.h"

#include <QMouseEvent>
#include <QQuickWindow>
#include <QtTest>
#include <limits>

#if defined(EDEN_HAS_X11_INPUT_TEST) && QT_CONFIG(xcb)
#include <QtGui/qguiapplication_platform.h>
#include <xcb/shape.h>
#include <xcb/xtest.h>
#endif

class ClickWindow final : public QQuickWindow {
  public:
    int presses = 0;

  protected:
    void mousePressEvent(QMouseEvent *event) override {
        ++presses;
        event->accept();
    }
};

class WindowInputRegionTest final : public QObject {
    Q_OBJECT

  private slots:
    void followsTheVisibleOutline();
    void followsResizingAndScale();
    void rejectsInvalidGeometry();
    void survivesWindowReplacementAndDestruction();
    void nativeShadowPassesClicksThrough();
};

void WindowInputRegionTest::followsTheVisibleOutline() {
    QQuickWindow window;
    window.resize(500, 400);
    eden::core::WindowInputRegion input;
    input.setWindow(&window);
    input.setRect(QRectF(48, 48, 404, 304));
    input.setRadius(16);
    const QRegion region = input.inputRegion();
    QVERIFY(region.contains(QPoint(250, 200)));
    QVERIFY(region.contains(QPoint(48, 200)));
    QVERIFY(region.contains(QPoint(451, 200)));
    QVERIFY(region.contains(QPoint(250, 48)));
    QVERIFY(region.contains(QPoint(250, 351)));
    QVERIFY(!region.contains(QPoint(47, 200)));
    QVERIFY(!region.contains(QPoint(452, 200)));
    QVERIFY(!region.contains(QPoint(250, 47)));
    QVERIFY(!region.contains(QPoint(250, 352)));
    QVERIFY(!region.contains(QPoint(48, 48)));
    QVERIFY(region.contains(QPoint(58, 58)));
    input.setRadius(0);
    input.setRect(QRectF(0, 0, 500, 400));
    QCOMPARE(input.inputRegion(), QRegion(QRect(0, 0, 500, 400)));
}

void WindowInputRegionTest::followsResizingAndScale() {
    QQuickWindow window;
    window.resize(500, 400);
    eden::core::WindowInputRegion input;
    input.setWindow(&window);
    input.setRect(QRectF(24, 24, 452, 352));
    input.setRadius(12);
    QCOMPARE(input.inputRegion(2).boundingRect(), QRect(48, 48, 904, 704));
    QCOMPARE(input.inputRegion(1.5).boundingRect(), QRect(36, 36, 678, 528));
    window.resize(300, 250);
    input.setRect(QRectF(24, 24, 252, 202));
    QCOMPARE(input.inputRegion().boundingRect(), QRect(24, 24, 252, 202));
    input.setRadius(500);
    QVERIFY(!input.inputRegion().contains(QPoint(24, 24)));
    QVERIFY(input.inputRegion().contains(QPoint(150, 125)));
}

void WindowInputRegionTest::rejectsInvalidGeometry() {
    QQuickWindow window;
    window.resize(300, 250);
    eden::core::WindowInputRegion input;
    input.setWindow(&window);
    input.setRect(QRectF(20, 20, 260, 210));
    input.setRadius(12);
    const QRegion original = input.inputRegion();
    input.setRect(QRectF(std::numeric_limits<qreal>::infinity(), 20, 260, 210));
    input.setRadius(std::numeric_limits<qreal>::quiet_NaN());
    QCOMPARE(input.inputRegion(), original);
    QVERIFY(input.inputRegion(0).isEmpty());
    QVERIFY(input.inputRegion(-1).isEmpty());
    input.setRect(QRectF(-20, -20, 400, 400));
    QCOMPARE(input.inputRegion(), QRegion(QRect(0, 0, 300, 250)));
    input.setRect({});
    QVERIFY(input.inputRegion().isEmpty());
}

void WindowInputRegionTest::survivesWindowReplacementAndDestruction() {
    eden::core::WindowInputRegion input;
    auto first = std::make_unique<QQuickWindow>();
    auto second = std::make_unique<QQuickWindow>();
    first->resize(300, 250);
    second->resize(400, 300);
    input.setWindow(first.get());
    input.setRect(QRectF(20, 20, 360, 260));
    input.setWindow(second.get());
    first.reset();
    QCOMPARE(input.inputRegion().boundingRect(), QRect(20, 20, 360, 260));
    second.reset();
    QVERIFY(!input.window());
    QVERIFY(input.inputRegion().isEmpty());
}

void WindowInputRegionTest::nativeShadowPassesClicksThrough() {
#if defined(EDEN_HAS_X11_INPUT_TEST) && QT_CONFIG(xcb)
    if (QGuiApplication::platformName() != "xcb") {
        QSKIP("Native pointer routing requires the isolated X11 test display");
    }
    auto *native = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    QVERIFY(native);
    auto *connection = native->connection();
    ClickWindow behind;
    ClickWindow front;
    behind.setFlags(Qt::Window | Qt::FramelessWindowHint);
    front.setFlags(Qt::Window | Qt::FramelessWindowHint);
    behind.setGeometry(20, 20, 400, 300);
    front.setGeometry(20, 20, 400, 300);
    behind.setColor(Qt::red);
    front.setColor(Qt::blue);
    eden::core::WindowInputRegion input;
    input.setWindow(&front);
    input.setRect(QRectF(40, 40, 320, 220));
    input.setRadius(20);
    behind.show();
    front.show();
    front.raise();
    QVERIFY(QTest::qWaitForWindowExposed(&behind));
    QVERIFY(QTest::qWaitForWindowExposed(&front));
    const auto nativeRegion = [&](int kind) {
        const auto cookie = xcb_shape_get_rectangles(connection, front.winId(), kind);
        auto *reply = xcb_shape_get_rectangles_reply(connection, cookie, nullptr);
        QRegion result;
        if (reply) {
            const auto *rectangles = xcb_shape_get_rectangles_rectangles(reply);
            const int count = xcb_shape_get_rectangles_rectangles_length(reply);
            for (int i = 0; i < count; ++i) {
                const auto &r = rectangles[i];
                result += QRect(r.x, r.y, r.width, r.height);
            }
            free(reply);
        }
        return result;
    };
    const qreal scale = front.devicePixelRatio();
    QTRY_COMPARE(nativeRegion(XCB_SHAPE_SK_INPUT), input.inputRegion(scale));
    QCOMPARE(nativeRegion(XCB_SHAPE_SK_BOUNDING), QRegion(QRect(QPoint(), front.size() * scale)));
    const auto click = [&](QPoint point) {
        const QPoint global = front.mapToGlobal(point) * scale;
        xcb_test_fake_input(connection, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, XCB_NONE, global.x(), global.y(), 0);
        xcb_test_fake_input(connection, XCB_BUTTON_PRESS, 1, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
        xcb_test_fake_input(connection, XCB_BUTTON_RELEASE, 1, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
        xcb_flush(connection);
    };
    click(QPoint(15, 150));
    QTRY_COMPARE(behind.presses, 1);
    QCOMPARE(front.presses, 0);
    click(QPoint(150, 150));
    QTRY_COMPARE(front.presses, 1);
    click(QPoint(41, 41));
    QTRY_COMPARE(behind.presses, 2);
    click(QPoint(160, 40));
    QTRY_COMPARE(front.presses, 2);
    click(QPoint(150, 280));
    QTRY_COMPARE(behind.presses, 3);
    const QImage rendered = front.grabWindow();
    QVERIFY(!rendered.isNull());
    QCOMPARE(rendered.pixelColor(QPoint(15, 150) * scale), QColor(Qt::blue));
    front.hide();
    front.destroy();
    front.show();
    QVERIFY(QTest::qWaitForWindowExposed(&front));
    QTRY_COMPARE(nativeRegion(XCB_SHAPE_SK_INPUT), input.inputRegion(scale));
    input.setRadius(0);
    input.setRect(QRectF(QPointF(), front.size()));
    QTRY_COMPARE(nativeRegion(XCB_SHAPE_SK_INPUT), QRegion(QRect(QPoint(), front.size() * scale)));
    input.setRect(QRectF(24, 24, 352, 252));
    QTRY_COMPARE(nativeRegion(XCB_SHAPE_SK_INPUT), input.inputRegion(scale));
    input.setWindow(nullptr);
    QTRY_COMPARE(nativeRegion(XCB_SHAPE_SK_INPUT), QRegion(QRect(QPoint(), front.size() * scale)));
#else
    QSKIP("Native pointer routing requires X11 Shape and XTest");
#endif
}

QTEST_MAIN(WindowInputRegionTest)

#include "windowinputregion_test.moc"

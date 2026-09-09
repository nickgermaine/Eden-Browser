#include "core/window/tabstripnavigator.h"
#include "core/window/windowframe.h"

#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest>

class FakeTabView final : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(qreal originX MEMBER originX)
    Q_PROPERTY(qreal contentWidth MEMBER contentWidth)
    Q_PROPERTY(qreal contentX MEMBER contentX)
    Q_PROPERTY(int count MEMBER count)

  public:
    Q_INVOKABLE int indexAt(qreal x, qreal) const {
        return static_cast<int>(x / 100);
    }

    Q_INVOKABLE void positionViewAtIndex(int index, int mode) {
        lastIndex = index;
        lastMode = mode;
    }

    Q_INVOKABLE void forceLayout() {
        ++layoutCount;
    }

    qreal originX = 0;
    qreal contentWidth = 1000;
    qreal contentX = 0;
    int count = 10;
    int lastIndex = -1;
    int lastMode = -1;
    int layoutCount = 0;
};

class WindowFrameTest final : public QObject {
    Q_OBJECT

  private slots:
    void outsideClickClearsFocus();
    void childRemovalDuringTeardownIsIgnored();
    void tabNavigatorScrollsAndSnaps();
    void tabNavigatorRelayoutNormalizesGeometry();
    void tabNavigatorCancelsRelayoutAfterDestruction();
};

void WindowFrameTest::outsideClickClearsFocus() {
    QQuickWindow window;
    window.setGeometry(0, 0, 400, 300);
    QQuickItem field(window.contentItem());
    field.setPosition(QPointF(10, 10));
    field.setSize(QSizeF(120, 40));

    eden::core::WindowFrame frame;
    frame.setWindow(&window);
    field.forceActiveFocus(Qt::MouseFocusReason);
    QCOMPARE(window.activeFocusItem(), &field);

    QMouseEvent insidePress(
        QEvent::MouseButtonPress,
        QPointF(30, 30),
        QPointF(30, 30),
        QPointF(30, 30),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier
    );
    QCoreApplication::sendEvent(&window, &insidePress);
    QCOMPARE(window.activeFocusItem(), &field);

    QMouseEvent outsidePress(
        QEvent::MouseButtonPress,
        QPointF(240, 180),
        QPointF(240, 180),
        QPointF(240, 180),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier
    );
    QCoreApplication::sendEvent(&window, &outsidePress);
    QVERIFY(window.activeFocusItem() != &field);
}

void WindowFrameTest::childRemovalDuringTeardownIsIgnored() {
    QQuickWindow window;
    eden::core::WindowFrame frame;
    frame.setWindow(&window);
    QEvent childRemoval(QEvent::ChildRemoved);
    QCoreApplication::sendEvent(&window, &childRemoval);
    QCOMPARE(frame.window(), &window);
}

void WindowFrameTest::tabNavigatorScrollsAndSnaps() {
    FakeTabView view;
    view.setSize(QSizeF(300, 40));
    eden::core::TabStripNavigator navigator;
    navigator.setView(&view);

    navigator.slide(14);
    QCOMPARE(view.contentX, 14);
    navigator.snap(1);
    QCOMPARE(view.lastIndex, 4);
    QCOMPARE(view.lastMode, 2);

    view.contentX = 250;
    navigator.snap(-1);
    QCOMPARE(view.lastIndex, 1);
    QCOMPARE(view.lastMode, 0);

    navigator.reveal(7);
    QCOMPARE(view.lastIndex, 7);
    QCOMPARE(view.lastMode, 4);

    QCOMPARE(navigator.destinationForDrag(2, 450), 4);
    QCOMPARE(navigator.destinationForDrag(2, -20), 0);
    QCOMPARE(navigator.destinationForDrag(2, 1200), 9);
}

void WindowFrameTest::tabNavigatorRelayoutNormalizesGeometry() {
    FakeTabView view;
    view.setSize(QSizeF(300, 40));
    eden::core::TabStripNavigator navigator;
    navigator.setView(&view);

    view.originX = 12;
    view.contentWidth = 1000;
    view.contentX = 920;
    navigator.scheduleRelayout(true, 7);
    QCoreApplication::processEvents();
    QCOMPARE(view.layoutCount, 1);
    QCOMPARE(view.contentX, 712);
    QCOMPARE(view.lastIndex, 7);
    QCOMPARE(view.lastMode, 4);

    view.contentX = 280;
    navigator.scheduleRelayout(true, 4);
    navigator.scheduleRelayout(false, 4);
    QCoreApplication::processEvents();
    QCOMPARE(view.layoutCount, 2);
    QCOMPARE(view.contentX, 12);
}

void WindowFrameTest::tabNavigatorCancelsRelayoutAfterDestruction() {
    FakeTabView view;
    auto *navigator = new eden::core::TabStripNavigator;
    navigator->setView(&view);
    navigator->scheduleRelayout(true, 0);
    delete navigator;
    QCoreApplication::processEvents();
    QCOMPARE(view.layoutCount, 0);
}

QTEST_MAIN(WindowFrameTest)

#include "windowframe_test.moc"

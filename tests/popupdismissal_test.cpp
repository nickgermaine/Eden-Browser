#include "core/window/popupdismissal.h"

#include <QPointingDevice>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QWheelEvent>
#include <QtTest>
#include <memory>

namespace {

    constexpr auto popupScene = R"(
import QtQuick
import QtQuick.Controls

Window {
    width: 640
    height: 480
    visible: true

    Rectangle {
        objectName: "target"
        property int presses: 0
        anchors.fill: parent
        color: "white"

        TapHandler {
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onPressedChanged: {
                if (pressed)
                    parent.presses++;
            }
        }
    }

    Popup {
        objectName: "lower"
        x: 20
        y: 20
        width: 200
        height: 200
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    }

    Popup {
        objectName: "upper"
        x: 160
        y: 60
        width: 160
        height: 100
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    }

    Popup {
        objectName: "sibling"
        x: 400
        y: 20
        width: 160
        height: 100
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    }

    Popup {
        objectName: "entering"
        x: 240
        y: 230
        width: 100
        height: 60
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        enter: Transition {
            NumberAnimation {
                property: "opacity"
                from: 0
                to: 1
                duration: 60000
            }
        }
    }

    Popup {
        objectName: "windowed"
        popupType: Popup.Window
        x: 400
        y: 150
        width: 160
        height: 100
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    }

    Popup {
        objectName: "sticky"
        x: 400
        y: 300
        width: 160
        height: 100
        closePolicy: Popup.CloseOnEscape
    }

    Popup {
        objectName: "modal"
        x: 240
        y: 300
        width: 120
        height: 100
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    }

    Popup {
        objectName: "dialog"
        x: 240
        y: 300
        width: 120
        height: 100
        modal: true
        closePolicy: Popup.CloseOnEscape
    }
}
)";

}

class WindowDestroyer final : public QObject {
    Q_OBJECT

  public:
    std::unique_ptr<QQuickWindow> window;

  public slots:
    void destroyWindow() {
        window.reset();
    }
};

class PopupDismissalTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void init();
    void cleanup();
    void sequentialRightClicksNeverStackMenus();
    void everyButtonDismissesTheWholeStack();
    void pressInsideTheTopPopupKeepsTheStack();
    void pressInsideALowerPopupClosesOnlyThePopupsAboveIt();
    void popupsThatIgnoreOutsidePressesStayOpen();
    void outsidePressReachesTheContentBelow();
    void modalPopupClosesAndSwallowsThePress();
    void modalPopupWithoutOutsidePolicyBlocksTheCascade();
    void touchPressDismisses();
    void wheelOutsideDismissesNonModalPopups();
    void wheelInsideKeepsThePopup();
    void windowDeactivationDismissesNonModalPopups();
    void pressInAnotherWindowDismissesPopups();
    void wheelInAnotherWindowDismissesPopups();
    void enteringPopupShieldsThePopupsBelow();
    void windowPopupsFollowTheSameRules();
    void windowDestroyedDuringDismissalIsSkipped();
    void staticDismissalWorksWithoutTheFilter();

  private:
    std::unique_ptr<QQuickWindow> createWindow();
    QObject *popup(const char *name) const;
    bool isOpen(const char *name) const;
    void open(const char *name) const;
    int targetPresses() const;
    void sendWheel(QQuickWindow *window, QPoint position);

    QQmlEngine m_engine;
    std::unique_ptr<QQuickWindow> m_window;
    std::unique_ptr<eden::core::PopupDismissal> m_dismissal;
};

void PopupDismissalTest::initTestCase() {
    QQuickStyle::setStyle("Basic");
}

std::unique_ptr<QQuickWindow> PopupDismissalTest::createWindow() {
    QQmlComponent component(&m_engine);
    component.setData(popupScene, QUrl("qrc:/popupdismissal_test.qml"));
    std::unique_ptr<QObject> object(component.create());
    if (!object) {
        qWarning() << component.errors();
        return {};
    }
    auto *window = qobject_cast<QQuickWindow *>(object.get());
    if (!window) {
        return {};
    }
    object.release();
    return std::unique_ptr<QQuickWindow>(window);
}

void PopupDismissalTest::init() {
    m_dismissal = std::make_unique<eden::core::PopupDismissal>();
    m_window = createWindow();
    QVERIFY(m_window);
    QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
}

void PopupDismissalTest::cleanup() {
    m_window.reset();
    m_dismissal.reset();
}

QObject *PopupDismissalTest::popup(const char *name) const {
    return m_window->findChild<QObject *>(QString::fromLatin1(name));
}

bool PopupDismissalTest::isOpen(const char *name) const {
    QObject *object = popup(name);
    return object && object->property("opened").toBool();
}

void PopupDismissalTest::open(const char *name) const {
    QObject *object = popup(name);
    QVERIFY(object);
    QVERIFY(QMetaObject::invokeMethod(object, "open"));
    QTRY_VERIFY(object->property("opened").toBool());
}

int PopupDismissalTest::targetPresses() const {
    QObject *target = m_window->findChild<QObject *>("target");
    return target ? target->property("presses").toInt() : -1;
}

void PopupDismissalTest::sendWheel(QQuickWindow *window, QPoint position) {
    QWheelEvent wheel(
        position,
        window->mapToGlobal(position),
        QPoint(),
        QPoint(0, -120),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::NoScrollPhase,
        false
    );
    QCoreApplication::sendEvent(window, &wheel);
}

void PopupDismissalTest::sequentialRightClicksNeverStackMenus() {
    const char *menus[] = {"lower", "sibling", "upper", "sibling", "lower"};
    const QPoint outside(600, 460);
    for (const char *menu : menus) {
        open(menu);
        QTest::mouseClick(m_window.get(), Qt::RightButton, Qt::NoModifier, outside);
        QTRY_VERIFY(!isOpen(menu));
    }
    open("lower");
    QTest::mouseClick(m_window.get(), Qt::RightButton, Qt::NoModifier, outside);
    open("sibling");
    QTest::mouseClick(m_window.get(), Qt::RightButton, Qt::NoModifier, outside);
    QVERIFY(!isOpen("lower"));
    QTRY_VERIFY(!isOpen("sibling"));
}

void PopupDismissalTest::everyButtonDismissesTheWholeStack() {
    const Qt::MouseButton buttons[] = {Qt::LeftButton, Qt::RightButton, Qt::MiddleButton};
    for (const Qt::MouseButton button : buttons) {
        open("lower");
        open("upper");
        open("sibling");
        QTest::mouseClick(m_window.get(), button, Qt::NoModifier, QPoint(600, 460));
        QTRY_VERIFY(!isOpen("lower"));
        QVERIFY(!isOpen("upper"));
        QVERIFY(!isOpen("sibling"));
    }
}

void PopupDismissalTest::pressInsideTheTopPopupKeepsTheStack() {
    open("lower");
    open("upper");
    QTest::mouseClick(m_window.get(), Qt::RightButton, Qt::NoModifier, QPoint(200, 100));
    QTest::mouseClick(m_window.get(), Qt::LeftButton, Qt::NoModifier, QPoint(300, 140));
    QVERIFY(isOpen("lower"));
    QVERIFY(isOpen("upper"));
}

void PopupDismissalTest::pressInsideALowerPopupClosesOnlyThePopupsAboveIt() {
    open("lower");
    open("upper");
    open("sibling");
    QTest::mouseClick(m_window.get(), Qt::RightButton, Qt::NoModifier, QPoint(60, 180));
    QTRY_VERIFY(!isOpen("sibling"));
    QVERIFY(!isOpen("upper"));
    QVERIFY(isOpen("lower"));
}

void PopupDismissalTest::popupsThatIgnoreOutsidePressesStayOpen() {
    open("lower");
    open("sticky");
    QTest::mouseClick(m_window.get(), Qt::RightButton, Qt::NoModifier, QPoint(600, 20));
    QTRY_VERIFY(!isOpen("lower"));
    QVERIFY(isOpen("sticky"));
}

void PopupDismissalTest::outsidePressReachesTheContentBelow() {
    open("lower");
    const int before = targetPresses();
    QTest::mouseClick(m_window.get(), Qt::RightButton, Qt::NoModifier, QPoint(600, 460));
    QTRY_VERIFY(!isOpen("lower"));
    QCOMPARE(targetPresses(), before + 1);
}

void PopupDismissalTest::modalPopupClosesAndSwallowsThePress() {
    open("lower");
    open("modal");
    const int before = targetPresses();
    QTest::mouseClick(m_window.get(), Qt::RightButton, Qt::NoModifier, QPoint(600, 460));
    QTRY_VERIFY(!isOpen("modal"));
    QVERIFY(isOpen("lower"));
    QCOMPARE(targetPresses(), before);
}

void PopupDismissalTest::modalPopupWithoutOutsidePolicyBlocksTheCascade() {
    open("lower");
    open("dialog");
    QTest::mouseClick(m_window.get(), Qt::RightButton, Qt::NoModifier, QPoint(600, 460));
    QVERIFY(isOpen("dialog"));
    QVERIFY(isOpen("lower"));
    QVERIFY(QMetaObject::invokeMethod(popup("dialog"), "close"));
    QTRY_VERIFY(!isOpen("dialog"));
}

void PopupDismissalTest::touchPressDismisses() {
    QPointingDevice *device = QTest::createTouchDevice();
    open("lower");
    open("upper");
    QTest::touchEvent(m_window.get(), device).press(0, QPoint(250, 140));
    QTest::touchEvent(m_window.get(), device).release(0, QPoint(250, 140));
    QVERIFY(isOpen("lower"));
    QVERIFY(isOpen("upper"));
    QTest::touchEvent(m_window.get(), device).press(0, QPoint(600, 460));
    QTest::touchEvent(m_window.get(), device).release(0, QPoint(600, 460));
    QTRY_VERIFY(!isOpen("lower"));
    QVERIFY(!isOpen("upper"));
}

void PopupDismissalTest::wheelOutsideDismissesNonModalPopups() {
    open("lower");
    open("sibling");
    sendWheel(m_window.get(), QPoint(600, 460));
    QTRY_VERIFY(!isOpen("lower"));
    QVERIFY(!isOpen("sibling"));
    open("lower");
    open("modal");
    sendWheel(m_window.get(), QPoint(600, 460));
    QVERIFY(isOpen("modal"));
    QVERIFY(isOpen("lower"));
}

void PopupDismissalTest::wheelInsideKeepsThePopup() {
    open("lower");
    sendWheel(m_window.get(), QPoint(60, 60));
    QVERIFY(isOpen("lower"));
}

void PopupDismissalTest::windowDeactivationDismissesNonModalPopups() {
    open("lower");
    open("sticky");
    open("modal");
    QEvent deactivate(QEvent::WindowDeactivate);
    QCoreApplication::sendEvent(m_window.get(), &deactivate);
    QTRY_VERIFY(!isOpen("lower"));
    QVERIFY(isOpen("sticky"));
    QVERIFY(isOpen("modal"));
}

void PopupDismissalTest::pressInAnotherWindowDismissesPopups() {
    std::unique_ptr<QQuickWindow> other = createWindow();
    QVERIFY(other);
    QVERIFY(QTest::qWaitForWindowExposed(other.get()));
    open("lower");
    open("modal");
    QTest::mouseClick(other.get(), Qt::RightButton, Qt::NoModifier, QPoint(600, 460));
    QTRY_VERIFY(!isOpen("lower"));
    QVERIFY(isOpen("modal"));
}

void PopupDismissalTest::wheelInAnotherWindowDismissesPopups() {
    std::unique_ptr<QQuickWindow> other = createWindow();
    QVERIFY(other);
    QVERIFY(QTest::qWaitForWindowExposed(other.get()));
    open("lower");
    open("modal");
    sendWheel(other.get(), QPoint(600, 460));
    QTRY_VERIFY(!isOpen("lower"));
    QVERIFY(isOpen("modal"));
}

void PopupDismissalTest::enteringPopupShieldsThePopupsBelow() {
    open("lower");
    QObject *entering = popup("entering");
    QVERIFY(entering);
    QVERIFY(QMetaObject::invokeMethod(entering, "open"));
    QTRY_VERIFY(entering->property("visible").toBool());
    QVERIFY(!entering->property("opened").toBool());
    QTest::mouseClick(m_window.get(), Qt::RightButton, Qt::NoModifier, QPoint(290, 260));
    QVERIFY(isOpen("lower"));
    QVERIFY(entering->property("visible").toBool());
    QTest::mouseClick(m_window.get(), Qt::RightButton, Qt::NoModifier, QPoint(600, 460));
    QTRY_VERIFY(!entering->property("visible").toBool());
    QVERIFY(!isOpen("lower"));
}

void PopupDismissalTest::windowPopupsFollowTheSameRules() {
    QObject *windowed = popup("windowed");
    QVERIFY(windowed);
    open("lower");
    open("windowed");
    QTest::mouseClick(m_window.get(), Qt::RightButton, Qt::NoModifier, QPoint(600, 460));
    QTRY_VERIFY(!isOpen("windowed"));
    QVERIFY(!isOpen("lower"));
    open("windowed");
    auto *content = windowed->property("contentItem").value<QQuickItem *>();
    QVERIFY(content);
    QQuickWindow *popupWindow = content->window();
    QVERIFY(popupWindow);
    QVERIFY(popupWindow != m_window.get());
    const QPoint inside = content->mapToScene(QPointF(content->width() / 2, content->height() / 2)).toPoint();
    QTest::mouseClick(popupWindow, Qt::RightButton, Qt::NoModifier, inside);
    QVERIFY(isOpen("windowed"));
    QVERIFY(QMetaObject::invokeMethod(windowed, "close"));
    QTRY_VERIFY(!isOpen("windowed"));
}

void PopupDismissalTest::windowDestroyedDuringDismissalIsSkipped() {
    WindowDestroyer destroyer;
    destroyer.window = createWindow();
    std::unique_ptr<QQuickWindow> pressed = createWindow();
    QVERIFY(destroyer.window);
    QVERIFY(pressed);
    QVERIFY(QTest::qWaitForWindowExposed(destroyer.window.get()));
    QVERIFY(QTest::qWaitForWindowExposed(pressed.get()));
    QObject *doomedPopup = destroyer.window->findChild<QObject *>("lower");
    QVERIFY(doomedPopup);
    QVERIFY(QMetaObject::invokeMethod(doomedPopup, "open"));
    QTRY_VERIFY(doomedPopup->property("opened").toBool());
    open("lower");
    QPointer<QQuickWindow> doomed = destroyer.window.get();
    QVERIFY(
        QObject::connect(popup("lower"), SIGNAL(closed()), &destroyer, SLOT(destroyWindow()), Qt::DirectConnection)
    );
    QTest::mouseClick(pressed.get(), Qt::RightButton, Qt::NoModifier, QPoint(600, 460));
    QVERIFY(!isOpen("lower"));
    QVERIFY(!doomed);
}

void PopupDismissalTest::staticDismissalWorksWithoutTheFilter() {
    m_dismissal.reset();
    open("sibling");
    QVERIFY(!eden::core::PopupDismissal::dismissOutside(
        m_window.get(),
        {QPointF(480, 60)},
        eden::core::PopupDismissal::Interaction::Press
    ));
    QVERIFY(isOpen("sibling"));
    QVERIFY(
        !eden::core::PopupDismissal::dismissOutside(m_window.get(), {}, eden::core::PopupDismissal::Interaction::Press)
    );
    QVERIFY(isOpen("sibling"));
    eden::core::PopupDismissal::dismissOutside(
        m_window.get(),
        {QPointF(600, 460)},
        eden::core::PopupDismissal::Interaction::Wheel
    );
    QTRY_VERIFY(!isOpen("sibling"));
    eden::core::PopupDismissal::dismissOutside(
        nullptr,
        {QPointF(1, 1)},
        eden::core::PopupDismissal::Interaction::Press
    );
    eden::core::PopupDismissal::dismissTransient(nullptr);
}

QTEST_MAIN(PopupDismissalTest)
#include "popupdismissal_test.moc"

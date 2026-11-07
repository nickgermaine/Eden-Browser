#include "core/settings/shortcutregistry.h"

#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>

class ShortcutRegistryTest final : public QObject {
    Q_OBJECT

  private slots:
    void qtQuickWindowDispatchesBrowserShortcuts() {
        eden::core::ShortcutRegistry shortcuts;
        QQuickWindow window;
        shortcuts.attach(&window);
        QSignalSpy triggered(&shortcuts, &eden::core::ShortcutRegistry::commandTriggered);

        const auto expect = [&](Qt::Key key, Qt::KeyboardModifiers modifiers, const QString &command) {
            triggered.clear();
            QTest::keyClick(&window, key, modifiers);
            QCOMPARE(triggered.size(), 1);
            QCOMPARE(triggered.constFirst().constFirst().toString(), command);
        };
        expect(Qt::Key_T, Qt::ControlModifier, "new_tab");
        expect(Qt::Key_N, Qt::ControlModifier, "new_window");
        expect(Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier, "private_window");
        expect(Qt::Key_W, Qt::ControlModifier, "close_tab");
        expect(Qt::Key_W, Qt::ControlModifier | Qt::ShiftModifier, "close_window");
        expect(Qt::Key_R, Qt::ControlModifier, "reload");
        expect(Qt::Key_R, Qt::ControlModifier | Qt::ShiftModifier, "reload_bypass_cache");
        expect(Qt::Key_F5, Qt::NoModifier, "reload");
        expect(Qt::Key_D, Qt::AltModifier, "focus_omnibox");
        expect(Qt::Key_Left, Qt::AltModifier, "back");
        expect(Qt::Key_1, Qt::ControlModifier, "activate_tab_1");
        expect(Qt::Key_PageDown, Qt::ControlModifier, "next_tab");
        expect(Qt::Key_PageUp, Qt::ControlModifier | Qt::ShiftModifier, "move_tab_left");
        expect(Qt::Key_Equal, Qt::ControlModifier, "zoom_in");
        expect(Qt::Key_Minus, Qt::ControlModifier, "zoom_out");
        expect(Qt::Key_0, Qt::ControlModifier, "zoom_reset");
        expect(Qt::Key_F3, Qt::ShiftModifier, "find_previous");
        expect(Qt::Key_I, Qt::ControlModifier | Qt::ShiftModifier, "devtools");
        expect(Qt::Key_F12, Qt::NoModifier, "devtools");
        expect(Qt::Key_Plus, Qt::ControlModifier | Qt::ShiftModifier, "zoom_in");
        expect(Qt::Key_Plus, Qt::ControlModifier | Qt::KeypadModifier, "zoom_in");

        triggered.clear();
        QTest::keyClick(&window, Qt::Key_C, Qt::ControlModifier);
        QVERIFY(triggered.isEmpty());
    }

    void focusedItemAndWindowIsolation() {
        eden::core::ShortcutRegistry first;
        eden::core::ShortcutRegistry second;
        QQuickWindow a;
        QQuickWindow b;
        QQuickItem page(a.contentItem());
        first.attach(&a);
        second.attach(&b);
        QSignalSpy triggered(&first, &eden::core::ShortcutRegistry::commandTriggered);
        QSignalSpy other(&second, &eden::core::ShortcutRegistry::commandTriggered);
        for (const auto key : {Qt::Key_R, Qt::Key_N, Qt::Key_T}) {
            QKeyEvent press(QEvent::KeyPress, key, Qt::ControlModifier);
            QCoreApplication::sendEvent(&page, &press);
        }
        QCOMPARE(triggered.size(), 3);
        QVERIFY(other.isEmpty());
        QCOMPARE(first.shortcutText("new_tab"), QString("Ctrl+T"));
        QCOMPARE(first.shortcutText("copy"), QString("Ctrl+C"));
        QCOMPARE(first.shortcutText("unknown"), QString());
    }
};

QTEST_MAIN(ShortcutRegistryTest)

#include "shortcutregistry_test.moc"

#include "engine/cef/clipboardmirror.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QtTest>

using eden::engine::cef::ClipboardMirror;

class ClipboardMirrorTest final : public QObject {
    Q_OBJECT

  private slots:
    void init() {
        ClipboardMirror::instance().setText("Initial");
    }

    void laterFrameInvalidatesEarlierExtraction() {
        auto &mirror = ClipboardMirror::instance();
        mirror.begin("frame-one:document-one:1");
        mirror.begin("frame-two:document-one:1");
        mirror.commit("frame-one:document-one:1", "Old");
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("Initial"));
        mirror.commit("frame-two:document-one:1", "New");
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("New"));
        mirror.commit("frame-one:document-one:1", "Old again");
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("New"));
    }

    void nativeCopyInvalidatesExtraction() {
        auto &mirror = ClipboardMirror::instance();
        mirror.begin("page:1");
        mirror.setText("Menu copy");
        mirror.commit("page:1", "Old");
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("Menu copy"));
    }

    void desktopClipboardChangeInvalidatesExtraction() {
        auto &mirror = ClipboardMirror::instance();
        mirror.begin("page:1");
        QGuiApplication::clipboard()->setText("External");
        mirror.commit("page:1", "Old");
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("External"));
        QVERIFY(mirror.mirroredText().isEmpty());
    }

    void sameTextFromDesktopStillInvalidatesExtraction() {
        auto &mirror = ClipboardMirror::instance();
        mirror.begin("page:1");
        QGuiApplication::clipboard()->setText("Initial");
        mirror.commit("page:1", "Old");
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("Initial"));
        QVERIFY(mirror.mirroredText().isEmpty());
    }

    void emptyTextClearsClipboardAndConsumesToken() {
        auto &mirror = ClipboardMirror::instance();
        mirror.begin("page:1");
        mirror.commit("page:1", "");
        QCOMPARE(QGuiApplication::clipboard()->text(), QString());
        mirror.commit("page:1", "Replay");
        QCOMPARE(QGuiApplication::clipboard()->text(), QString());
    }

    void selectionChangesPreserveClipboardExtraction() {
        QClipboard *clipboard = QGuiApplication::clipboard();
        if (!clipboard->supportsSelection()) {
            QSKIP("The current platform has no primary selection");
        }
        auto &mirror = ClipboardMirror::instance();
        mirror.begin("page:1");
        clipboard->setText("Selected", QClipboard::Selection);
        mirror.commit("page:1", "Copied");
        QCOMPARE(clipboard->text(), QString("Copied"));
    }
};

QTEST_MAIN(ClipboardMirrorTest)

#include "clipboardmirror_test.moc"

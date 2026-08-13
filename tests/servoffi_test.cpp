#include "engine/servo/eden_servo_ffi.h"

#include <QtTest>

class ServoFfiTest final : public QObject {
    Q_OBJECT

  private slots:
    void runtimeIsStubbedOut();
    void versionIsExposed();
};

void ServoFfiTest::runtimeIsStubbedOut() {
    QVERIFY(!eden_servo_runtime_available());
}

void ServoFfiTest::versionIsExposed() {
    const QString version = QString::fromUtf8(eden_servo_version());
    QVERIFY(version.startsWith("eden-servo "));
}

QTEST_GUILESS_MAIN(ServoFfiTest)

#include "servoffi_test.moc"

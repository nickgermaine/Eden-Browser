#include "core/profiles/passwordhasher.h"

#include <QtTest>

class PasswordHasherTest final : public QObject {
    Q_OBJECT

  private slots:
    void hashAndVerifyRoundTrip();
    void wrongPasswordFailsAndFreshHashNeedsNoRehash();
    void malformedVerifierFailsSafely();
    void asynchronousWorkerDeliversOnCallerThread();
};

void PasswordHasherTest::hashAndVerifyRoundTrip() {
    QString verifier;
    QVERIFY(eden::core::PasswordHasher::hashPasswordBlocking(QStringLiteral("correct horse battery"), verifier));
    QVERIFY(verifier.startsWith(QStringLiteral("$argon2id$")));
    bool needsRehash = true;
    QVERIFY(
        eden::core::PasswordHasher::verifyPasswordBlocking(
            verifier,
            QStringLiteral("correct horse battery"),
            needsRehash
        )
    );
    QVERIFY(!needsRehash);
}

void PasswordHasherTest::wrongPasswordFailsAndFreshHashNeedsNoRehash() {
    QString verifier;
    QVERIFY(eden::core::PasswordHasher::hashPasswordBlocking(QStringLiteral("the right password"), verifier));
    bool needsRehash = false;
    QVERIFY(
        !eden::core::PasswordHasher::verifyPasswordBlocking(verifier, QStringLiteral("the wrong password"), needsRehash)
    );
}

void PasswordHasherTest::malformedVerifierFailsSafely() {
    bool needsRehash = false;
    QVERIFY(
        !eden::core::PasswordHasher::verifyPasswordBlocking(QString(), QStringLiteral("anything at all"), needsRehash)
    );
    QVERIFY(!eden::core::PasswordHasher::verifyPasswordBlocking(
        QStringLiteral("not-a-verifier"),
        QStringLiteral("anything at all"),
        needsRehash
    ));
    QVERIFY(!eden::core::PasswordHasher::verifyPasswordBlocking(
        QString(300, QLatin1Char('x')),
        QStringLiteral("anything at all"),
        needsRehash
    ));
    QString empty;
    QVERIFY(!eden::core::PasswordHasher::hashPasswordBlocking(QString(), empty));
}

void PasswordHasherTest::asynchronousWorkerDeliversOnCallerThread() {
    eden::core::PasswordHasher hasher;
    QString verifier;
    bool hashed = false;
    hasher.hashPassword(QStringLiteral("worker password"), [&](bool success, QString value) {
        QVERIFY(success);
        QCOMPARE(QThread::currentThread(), qApp->thread());
        verifier = value;
        hashed = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(hashed, 30000);
    bool verified = false;
    hasher.verifyPassword(verifier, QStringLiteral("worker password"), [&](bool matches, bool) {
        QVERIFY(matches);
        QCOMPARE(QThread::currentThread(), qApp->thread());
        verified = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(verified, 30000);
}

QTEST_GUILESS_MAIN(PasswordHasherTest)
#include "passwordhasher_test.moc"

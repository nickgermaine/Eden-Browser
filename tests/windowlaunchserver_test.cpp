#include "core/profiles/windowlaunchserver.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

using eden::core::WindowLaunchRequest;
using eden::core::WindowLaunchServer;

class WindowLaunchServerTest final : public QObject {
    Q_OBJECT

  private slots:
    void forwardsFromAnotherProcess_data();
    void forwardsFromAnotherProcess();
    void waitsForStartupListener();
    void reportsRejectedRequest();
    void receivesSplitRequestOnce();
    void rejectsInvalidRequests_data();
    void rejectsInvalidRequests();
    void isolatesStorageRoots();
    void missingInstanceTimesOut();
};

void WindowLaunchServerTest::forwardsFromAnotherProcess_data() {
    QTest::addColumn<bool>("privateWindow");
    QTest::newRow("normal-window") << false;
    QTest::newRow("private-window") << true;
}

void WindowLaunchServerTest::forwardsFromAnotherProcess() {
    QFETCH(bool, privateWindow);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    WindowLaunchServer server(root.path());
    QList<WindowLaunchRequest> received;
    QVERIFY(server.listen([&](const WindowLaunchRequest &request) {
        received.append(request);
        return true;
    }));
    const auto permissions = QFileInfo(server.socketName()).permissions();
    QVERIFY(!(permissions & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther)));
    QProcess child;
    child.start(
        QCoreApplication::applicationFilePath(),
        {"--send-window", root.path(), privateWindow ? "private" : "normal"}
    );
    QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, 5000);
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 0);
    QCOMPARE(received.size(), 1);
    QCOMPARE(received.first().privateWindow, privateWindow);
    QCOMPARE(received.first().engineName, QStringLiteral("cef"));
    QCOMPARE(
        received.first().urls,
        (QList<QUrl>{QUrl("https://example.com/a%2Fb?q=caf%C3%A9"), QUrl("eden://settings")})
    );
}

void WindowLaunchServerTest::waitsForStartupListener() {
    QTemporaryDir root;
    WindowLaunchServer server(root.path());
    int received = 0;
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(), {"--send-window", root.path(), "normal"});
    QVERIFY(child.waitForStarted());
    QTest::qWait(100);
    QVERIFY(server.listen([&](const WindowLaunchRequest &) {
        ++received;
        return true;
    }));
    QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, 5000);
    QCOMPARE(child.exitCode(), 0);
    QCOMPARE(received, 1);
}

void WindowLaunchServerTest::reportsRejectedRequest() {
    QTemporaryDir root;
    WindowLaunchServer server(root.path());
    QVERIFY(server.listen([](const WindowLaunchRequest &) { return false; }));
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(), {"--send-window", root.path(), "normal"});
    QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, 5000);
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 1);
}

void WindowLaunchServerTest::receivesSplitRequestOnce() {
    QTemporaryDir root;
    WindowLaunchServer server(root.path());
    int received = 0;
    QVERIFY(server.listen([&](const WindowLaunchRequest &) {
        ++received;
        return true;
    }));
    QLocalSocket socket;
    socket.connectToServer(server.socketName());
    QTRY_COMPARE(socket.state(), QLocalSocket::ConnectedState);
    socket.write(R"({"version":1,"private":false,)");
    socket.flush();
    QTest::qWait(20);
    QCOMPARE(received, 0);
    socket.write("\"engine\":\"\",\"urls\":[]}\n");
    QTRY_COMPARE(socket.state(), QLocalSocket::UnconnectedState);
    QCOMPARE(received, 1);
    QCOMPARE(socket.readAll(), QByteArray("accepted\n"));
}

void WindowLaunchServerTest::rejectsInvalidRequests_data() {
    QTest::addColumn<QByteArray>("payload");
    QTest::newRow("invalid-json") << QByteArray("invalid\n");
    QTest::newRow("wrong-version") << QByteArray("{\"version\":2,\"private\":false,\"engine\":\"\",\"urls\":[]}\n");
    QTest::newRow("missing-mode") << QByteArray("{\"version\":1,\"engine\":\"\",\"urls\":[]}\n");
    QTest::newRow("relative-url") << QByteArray(
        "{\"version\":1,\"private\":false,\"engine\":\"\",\"urls\":[\"relative\"]}\n"
    );
    QTest::newRow("empty-url") << QByteArray("{\"version\":1,\"private\":false,\"engine\":\"\",\"urls\":[\"\"]}\n");
    QTest::newRow("oversized-message") << QByteArray(256 * 1024 + 1, 'x');
    QTest::newRow("extra-message") << QByteArray("{\"version\":1,\"private\":false,\"engine\":\"\",\"urls\":[]}\n{}\n");
    QJsonArray urls;
    for (int i = 0; i < 129; ++i) {
        urls.append(QStringLiteral("https://example.com/"));
    }
    QTest::newRow("too-many-urls")
        << QJsonDocument(
               QJsonObject{{"version", 1}, {"private", false}, {"engine", ""}, {"urls", urls}}
           ).toJson(QJsonDocument::Compact) +
               '\n';
}

void WindowLaunchServerTest::rejectsInvalidRequests() {
    QFETCH(QByteArray, payload);
    QTemporaryDir root;
    WindowLaunchServer server(root.path());
    int received = 0;
    QVERIFY(server.listen([&](const WindowLaunchRequest &) {
        ++received;
        return true;
    }));
    QLocalSocket socket;
    socket.connectToServer(server.socketName());
    QTRY_COMPARE(socket.state(), QLocalSocket::ConnectedState);
    socket.write(payload);
    QTRY_COMPARE(socket.state(), QLocalSocket::UnconnectedState);
    QCOMPARE(received, 0);
    QVERIFY(socket.readAll() != "accepted\n");
}

void WindowLaunchServerTest::isolatesStorageRoots() {
    QTemporaryDir first;
    QTemporaryDir second;
    WindowLaunchServer one(first.path());
    WindowLaunchServer same(first.path());
    WindowLaunchServer two(second.path());
    QCOMPARE(one.socketName(), same.socketName());
    QVERIFY(one.socketName() != two.socketName());
    QVERIFY(one.listen([](const WindowLaunchRequest &) { return true; }));
    QVERIFY(two.listen([](const WindowLaunchRequest &) { return true; }));
}

void WindowLaunchServerTest::missingInstanceTimesOut() {
    QTemporaryDir root;
    WindowLaunchServer server(root.path());
    QElapsedTimer timer;
    timer.start();
    QVERIFY(!server.forward({}, 100));
    QVERIFY(timer.elapsed() < 1000);
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    const auto arguments = application.arguments();
    if (arguments.size() == 4 && arguments.at(1) == "--send-window") {
        WindowLaunchServer server(arguments.at(2));
        return server.forward(
                   {arguments.at(3) == "private",
                    "cef",
                    {QUrl("https://example.com/a%2Fb?q=caf%C3%A9"), QUrl("eden://settings")}},
                   3000
               )
                   ? 0
                   : 1;
    }
    WindowLaunchServerTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "windowlaunchserver_test.moc"

#include "engine/cef/devtoolssocketserver.h"

#include <QSignalSpy>
#include <QTest>
#include <QWebSocket>

using eden::engine::cef::DevToolsSocketServer;

class DevToolsSocketServerTest final : public QObject {
    Q_OBJECT

  private slots:
    void startsAndStopsWithSessions();
    void rejectsMissingWrongAndReusedTokens();
    void forwardsMessagesWithoutChangingPayload();
};

void DevToolsSocketServerTest::startsAndStopsWithSessions() {
    DevToolsSocketServer server;
    QVERIFY(!server.isListening());
    const DevToolsSocketServer::Session first = server.openSession([](const QByteArray &) {});
    QVERIFY(first.isValid());
    QCOMPARE(first.token.size(), 32);
    QVERIFY(server.isListening());
    QCOMPARE(server.sessionCount(), 1);
    const DevToolsSocketServer::Session second = server.openSession([](const QByteArray &) {});
    QVERIFY(second.isValid());
    QCOMPARE(server.port(), QUrl("ws://" + first.endpoint).port());
    QCOMPARE(server.sessionCount(), 2);
    server.closeSession(first.token);
    QVERIFY(server.isListening());
    server.closeSession(second.token);
    QVERIFY(!server.isListening());
    QCOMPARE(server.sessionCount(), 0);
    QCOMPARE(server.port(), quint16(0));
}

void DevToolsSocketServerTest::rejectsMissingWrongAndReusedTokens() {
    DevToolsSocketServer server;
    int delivered = 0;
    const DevToolsSocketServer::Session session = server.openSession([&delivered](const QByteArray &) { ++delivered; });
    QVERIFY(session.isValid());

    const auto rejected = [&server](const QUrl &url) {
        QWebSocket socket;
        QSignalSpy disconnected(&socket, &QWebSocket::disconnected);
        socket.open(url);
        QTRY_VERIFY_WITH_TIMEOUT(socket.state() == QAbstractSocket::UnconnectedState || disconnected.count() > 0, 2000);
        QCOMPARE(socket.state(), QAbstractSocket::UnconnectedState);
    };

    rejected(QUrl(QString("ws://127.0.0.1:%1/").arg(server.port())));
    rejected(QUrl(QString("ws://127.0.0.1:%1/not-the-token").arg(server.port())));

    QWebSocket accepted;
    QSignalSpy connected(&accepted, &QWebSocket::connected);
    accepted.open(QUrl("ws://" + session.endpoint));
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 2000);
    accepted.close();
    QTRY_COMPARE_WITH_TIMEOUT(accepted.state(), QAbstractSocket::UnconnectedState, 2000);

    rejected(QUrl("ws://" + session.endpoint));
    QCOMPARE(delivered, 0);
    server.closeSession(session.token);
}

void DevToolsSocketServerTest::forwardsMessagesWithoutChangingPayload() {
    DevToolsSocketServer server;
    QByteArray received;
    const DevToolsSocketServer::Session session =
        server.openSession([&received](const QByteArray &message) { received = message; });
    QVERIFY(session.isValid());

    QWebSocket socket;
    QSignalSpy connected(&socket, &QWebSocket::connected);
    QSignalSpy textReceived(&socket, &QWebSocket::textMessageReceived);
    socket.open(QUrl("ws://" + session.endpoint));
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 2000);

    const QByteArray request = R"({"id":17,"method":"Runtime.enable","params":{}})";
    socket.sendTextMessage(QString::fromUtf8(request));
    QTRY_COMPARE_WITH_TIMEOUT(received, request, 2000);

    const QByteArray response = R"({"id":17,"result":{"value":"garden"}})";
    server.sendMessage(session.token, response);
    QTRY_COMPARE_WITH_TIMEOUT(textReceived.count(), 1, 2000);
    QCOMPARE(textReceived.takeFirst().at(0).toString().toUtf8(), response);

    server.closeSession(session.token);
}

QTEST_MAIN(DevToolsSocketServerTest)

#include "devtoolssocketserver_test.moc"

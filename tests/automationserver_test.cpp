#include "core/automation/automationserver.h"
#include "core/automation/performancemetrics.h"
#include "core/profiles/windowregistry.h"
#include "core/window/tabmodel.h"
#include "core/window/windowcontroller.h"
#include "profiletesthelpers.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

class AutomationServerTest final : public QObject {
    Q_OBJECT

  private slots:
    void init();
    void cleanup();
    void disabledCreatesNoServer();
    void commandsAndTeardown();
    void rejectsUnsafePaths();
    void rejectsUnsafeTargets();
    void disconnectsInvalidPayloads();

  private:
    QJsonObject request(QLocalSocket &socket, int id, const QString &method, const QJsonObject &parameters = {});
    QByteArray m_previousRuntime;
    QByteArray m_previousAutomation;
    QByteArray m_previousSocket;
};

void AutomationServerTest::init() {
    m_previousRuntime = qgetenv("XDG_RUNTIME_DIR");
    m_previousAutomation = qgetenv("EDEN_AUTOMATION");
    m_previousSocket = qgetenv("EDEN_AUTOMATION_SOCKET");
    eden::core::PerformanceMetrics::clear();
}

void AutomationServerTest::cleanup() {
    qputenv("XDG_RUNTIME_DIR", m_previousRuntime);
    qputenv("EDEN_AUTOMATION", m_previousAutomation);
    qputenv("EDEN_AUTOMATION_SOCKET", m_previousSocket);
    eden::core::PerformanceMetrics::clear();
}

void AutomationServerTest::disabledCreatesNoServer() {
    qunsetenv("EDEN_AUTOMATION");
    QVERIFY(!eden::core::AutomationServer::createIfEnabled());
}

void AutomationServerTest::commandsAndTeardown() {
    QTemporaryDir runtime("/tmp/ea-XXXXXX");
    QVERIFY(runtime.isValid());
    QFile::setPermissions(runtime.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtime.path()));
    const QString socketPath = runtime.filePath("automation.sock");

    QQuickWindow window;
    QQmlEngine qml;
    QQmlComponent component(&qml);
    component.setData("import QtQuick\nTextInput { objectName: \"omniboxField\" }", QUrl());
    std::unique_ptr<QObject> fieldObject(component.create());
    QVERIFY2(fieldObject, qPrintable(component.errorString()));
    auto *field = qobject_cast<QQuickItem *>(fieldObject.get());
    QVERIFY(field);
    field->setParent(&window);
    field->setParentItem(window.contentItem());

    eden::core::WindowRegistry windowRegistry;
    eden::test::ProfileHarness harness;
    QVERIFY(harness.create(&qml));
    eden::core::WindowController controller(&window);
    controller.initialize(harness.context, false, "qtwebengine", false);
    bool quitCalled = false;
    {
        eden::core::AutomationServer server(socketPath, nullptr, [&quitCalled] { quitCalled = true; });
        server.attach(&controller, &window);
        QVERIFY(server.start());
        QVERIFY(QFileInfo::exists(socketPath));
        QLocalSocket socket;
        socket.connectToServer(socketPath);
        QVERIFY(socket.waitForConnected(1000));

        QCOMPARE(request(socket, 1, "openTab", {{"url", "eden://settings"}}).value("result").toInt(), 0);
        QCOMPARE(request(socket, 2, "openTab", {{"url", "eden://theme-editor"}}).value("result").toInt(), 1);
        QVERIFY(request(socket, 3, "activateTab", {{"index", 0}}).value("result").toBool());
        QVERIFY(request(socket, 4, "reorderTab", {{"from", 0}, {"to", 1}}).value("result").toBool());
        QVERIFY(request(socket, 5, "typeInOmnibox", {{"text", "z"}}).value("result").toBool());
        QCOMPARE(field->property("text").toString(), QString("z"));
        window.show();
        window.update();
        QTest::qWait(100);
        const QJsonObject metrics =
            request(socket, 6, "dumpMetrics", {{"afterSequence", 0}}).value("result").toObject();
        QVERIFY(metrics.value("sequence").toInteger() >= 1);
        const QJsonArray samples = metrics.value("samples").toArray();
        QCOMPARE(
            std::count_if(
                samples.begin(),
                samples.end(),
                [](const QJsonValue &sample) {
                    return sample.toObject().value("name").toString() == "input.key_to_frame_ms";
                }
            ),
            1
        );
        const QString capturePath = runtime.filePath("window.png");
        QVERIFY(request(socket, 7, "captureWindow", {{"path", capturePath}}).value("result").toBool());
        QVERIFY(!QImage(capturePath).isNull());
        QVERIFY(request(socket, 8, "captureWindow", {{"path", "/home/window.png"}}).contains("error"));
        QVERIFY(request(socket, 9, "quit").value("result").toBool());
        QTRY_VERIFY(quitCalled);
    }
    QVERIFY(!QFileInfo::exists(socketPath));
}

void AutomationServerTest::rejectsUnsafePaths() {
    QTemporaryDir runtime("/tmp/ea-XXXXXX");
    QVERIFY(runtime.isValid());
    QFile::setPermissions(runtime.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtime.path()));
    QTemporaryDir outside("/tmp/eo-XXXXXX");
    QVERIFY(outside.isValid());
    QFile::setPermissions(outside.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    eden::core::AutomationServer server(outside.filePath("automation.sock"));
    QVERIFY(!server.start());
}

void AutomationServerTest::rejectsUnsafeTargets() {
    QTemporaryDir runtime("/tmp/ea-XXXXXX");
    QVERIFY(runtime.isValid());
    QFile::setPermissions(runtime.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtime.path()));
    const QString regularPath = runtime.filePath("regular");
    QFile regular(regularPath);
    QVERIFY(regular.open(QIODevice::WriteOnly));
    QCOMPARE(regular.write("kept"), 4);
    regular.close();
    eden::core::AutomationServer regularServer(regularPath);
    QVERIFY(!regularServer.start());
    QVERIFY(regular.open(QIODevice::ReadOnly));
    QCOMPARE(regular.readAll(), QByteArray("kept"));

    const QString targetPath = runtime.filePath("target");
    QFile target(targetPath);
    QVERIFY(target.open(QIODevice::WriteOnly));
    target.close();
    const QString socketLink = runtime.filePath("socket-link");
    QVERIFY(QFile::link(targetPath, socketLink));
    eden::core::AutomationServer linkServer(socketLink);
    QVERIFY(!linkServer.start());
    QVERIFY(QFileInfo::exists(targetPath));
    QVERIFY(QFileInfo(socketLink).isSymLink());

    const QString insecureParent = runtime.filePath("insecure");
    QVERIFY(QDir().mkdir(insecureParent));
    QFile::setPermissions(
        insecureParent,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner | QFileDevice::ReadGroup |
            QFileDevice::ExeGroup
    );
    eden::core::AutomationServer modeServer(insecureParent + "/automation.sock");
    QVERIFY(!modeServer.start());

    const QString realParent = runtime.filePath("real-parent");
    QVERIFY(QDir().mkdir(realParent));
    QFile::setPermissions(realParent, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const QString parentLink = runtime.filePath("parent-link");
    QVERIFY(QFile::link(realParent, parentLink));
    eden::core::AutomationServer parentLinkServer(parentLink + "/automation.sock");
    QVERIFY(!parentLinkServer.start());
}

void AutomationServerTest::disconnectsInvalidPayloads() {
    QTemporaryDir runtime("/tmp/ea-XXXXXX");
    QVERIFY(runtime.isValid());
    QFile::setPermissions(runtime.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtime.path()));
    eden::core::AutomationServer server(runtime.filePath("automation.sock"));
    QVERIFY(server.start());

    QLocalSocket oversized;
    oversized.connectToServer(server.socketPath());
    QVERIFY(oversized.waitForConnected(1000));
    oversized.write(QByteArray(64 * 1024 + 1, 'x'));
    oversized.flush();
    QTRY_COMPARE(oversized.state(), QLocalSocket::UnconnectedState);

    QLocalSocket nul;
    nul.connectToServer(server.socketPath());
    QVERIFY(nul.waitForConnected(1000));
    nul.write(QByteArray("{\"jsonrpc\":\"2.0\"") + QByteArray(1, '\0') + "}\n");
    nul.flush();
    QTRY_COMPARE(nul.state(), QLocalSocket::UnconnectedState);
}

QJsonObject
AutomationServerTest::request(QLocalSocket &socket, int id, const QString &method, const QJsonObject &parameters) {
    QJsonObject command;
    command.insert("jsonrpc", "2.0");
    command.insert("id", id);
    command.insert("method", method);
    command.insert("params", parameters);
    socket.write(QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n');
    socket.flush();
    QElapsedTimer timer;
    timer.start();
    while (!socket.canReadLine() && timer.elapsed() < 1000) {
        QCoreApplication::processEvents();
        socket.waitForReadyRead(10);
    }
    return QJsonDocument::fromJson(socket.readLine()).object();
}

QTEST_MAIN(AutomationServerTest)

#include "automationserver_test.moc"

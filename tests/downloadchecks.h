#pragma once

#include "engine/engineprofile.h"
#include "engine/engineview.h"

#include <QFile>
#include <QSet>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

inline void verifyConcurrentDownloads(
    eden::engine::EngineProfile &profile,
    eden::engine::EngineView &first,
    eden::engine::EngineView &second
) {
    QTcpServer server;
    QList<QTcpSocket *> pending;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                if (socket->property("responded").toBool()) {
                    socket->readAll();
                    return;
                }
                QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n")) {
                    return;
                }
                socket->setProperty("responded", true);
                socket->write(
                    "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                    "Content-Disposition: attachment; filename=eden-parallel.txt\r\n"
                    "Content-Length: 65537\r\nConnection: close\r\n\r\n"
                );
                socket->write(QByteArray(65536, request.startsWith("GET /first ") ? '1' : '2'));
                pending.append(socket);
            });
        }
    });
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QSignalSpy started(&profile, &eden::engine::EngineProfile::downloadStarted);
    QSignalSpy updated(&profile, &eden::engine::EngineProfile::downloadUpdated);
    first.load(QUrl(QString("http://127.0.0.1:%1/first").arg(server.serverPort())));
    second.load(QUrl(QString("http://127.0.0.1:%1/second").arg(server.serverPort())));
    QTRY_COMPARE_WITH_TIMEOUT(started.size(), 2, 10000);
    const QString firstPath = started.at(0).at(3).toString();
    const QString secondPath = started.at(1).at(3).toString();
    QVERIFY2(firstPath != secondPath, "Two tabs reserved the same download destination");
    for (QTcpSocket *socket : pending) {
        socket->write("x");
        socket->disconnectFromHost();
    }
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&] {
            int completed = 0;
            for (const auto &update : updated) {
                completed += (update.at(3).toString() == "complete" || update.at(3).toString() == "completed");
            }
            return completed >= 2;
        })(),
        10000
    );
    QSet<QByteArray> contents;
    for (const QString &path : {firstPath, secondPath}) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        contents.insert(file.readAll());
    }
    QCOMPARE(contents, (QSet<QByteArray>{QByteArray(65536, '1') + 'x', QByteArray(65536, '2') + 'x'}));
}

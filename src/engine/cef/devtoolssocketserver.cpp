#include "engine/cef/devtoolssocketserver.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QThread>
#include <QWebSocket>
#include <QWebSocketProtocol>
#include <QWebSocketServer>

namespace eden::engine::cef {

    DevToolsSocketServer::DevToolsSocketServer(QObject *parent)
        : QObject(parent) {}

    DevToolsSocketServer::~DevToolsSocketServer() {
        const QStringList tokens = m_sessions.keys();
        for (const QString &token : tokens) {
            closeSession(token);
        }
    }

    DevToolsSocketServer::Session DevToolsSocketServer::openSession(MessageHandler handler) {
        Q_ASSERT(QThread::currentThread() == thread());
        if (!handler) {
            return {};
        }
        if (!m_server) {
            m_server = std::make_unique<QWebSocketServer>(QString(), QWebSocketServer::NonSecureMode);
            connect(
                m_server.get(),
                &QWebSocketServer::newConnection,
                this,
                &DevToolsSocketServer::acceptPendingConnection
            );
        }
        if (!m_server->isListening() && !m_server->listen(QHostAddress::LocalHost, 0)) {
            m_server.reset();
            return {};
        }
        QString token;
        do {
            token = createToken();
        } while (m_sessions.contains(token));
        m_sessions.insert(token, SessionState{std::move(handler), nullptr, false});
        return {token, QString("127.0.0.1:%1/%2").arg(m_server->serverPort()).arg(token)};
    }

    void DevToolsSocketServer::closeSession(const QString &token) {
        Q_ASSERT(QThread::currentThread() == thread());
        const auto found = m_sessions.find(token);
        if (found == m_sessions.end()) {
            return;
        }
        QPointer<QWebSocket> socket = found->socket;
        m_sessions.erase(found);
        if (socket) {
            socket->close(QWebSocketProtocol::CloseCodeGoingAway, QString());
            socket->deleteLater();
        }
        if (m_sessions.isEmpty()) {
            m_server.reset();
        }
    }

    void DevToolsSocketServer::sendMessage(const QString &token, const QByteArray &message) {
        Q_ASSERT(QThread::currentThread() == thread());
        const auto found = m_sessions.constFind(token);
        if (found == m_sessions.cend() || !found->socket || found->socket->state() != QAbstractSocket::ConnectedState) {
            return;
        }
        found->socket->sendTextMessage(QString::fromUtf8(message));
    }

    bool DevToolsSocketServer::isListening() const {
        return m_server && m_server->isListening();
    }

    int DevToolsSocketServer::sessionCount() const {
        return m_sessions.size();
    }

    int DevToolsSocketServer::connectedSessionCount() const {
        int count = 0;
        for (const SessionState &session : m_sessions) {
            if (session.socket && session.socket->state() == QAbstractSocket::ConnectedState) {
                ++count;
            }
        }
        return count;
    }

    quint16 DevToolsSocketServer::port() const {
        return m_server ? m_server->serverPort() : 0;
    }

    void DevToolsSocketServer::acceptPendingConnection() {
        Q_ASSERT(QThread::currentThread() == thread());
        while (m_server->hasPendingConnections()) {
            QWebSocket *socket = m_server->nextPendingConnection();
            if (!socket) {
                continue;
            }
            socket->setMaxAllowedIncomingMessageSize(64 * 1024 * 1024);
            const QString path = socket->requestUrl().path();
            const QString token = path.startsWith('/') ? path.sliced(1) : QString();
            auto found = m_sessions.find(token);
            if (found == m_sessions.end() || found->consumed || found->socket) {
                rejectConnection(socket);
                continue;
            }
            found->consumed = true;
            found->socket = socket;
            connect(socket, &QWebSocket::textMessageReceived, this, [this, token](const QString &message) {
                const auto current = m_sessions.constFind(token);
                if (current != m_sessions.cend() && current->socket && current->handler) {
                    current->handler(message.toUtf8());
                }
            });
            connect(socket, &QWebSocket::binaryMessageReceived, this, [this, token](const QByteArray &message) {
                const auto current = m_sessions.constFind(token);
                if (current != m_sessions.cend() && current->socket && current->handler) {
                    current->handler(message);
                }
            });
            connect(socket, &QWebSocket::disconnected, this, [this, token, socket] {
                const auto current = m_sessions.find(token);
                if (current != m_sessions.end() && current->socket == socket) {
                    current->socket.clear();
                }
                socket->deleteLater();
            });
        }
    }

    void DevToolsSocketServer::rejectConnection(QWebSocket *socket) {
        connect(socket, &QWebSocket::disconnected, socket, &QObject::deleteLater);
        socket->close(QWebSocketProtocol::CloseCodePolicyViolated, QString());
    }

    QString DevToolsSocketServer::createToken() const {
        const quint64 high = QRandomGenerator::system()->generate64();
        const quint64 low = QRandomGenerator::system()->generate64();
        return QString("%1%2").arg(high, 16, 16, QLatin1Char('0')).arg(low, 16, 16, QLatin1Char('0'));
    }

    DevToolsSocketServer *sharedDevToolsSocketServer() {
        static QPointer<DevToolsSocketServer> server;
        Q_ASSERT(QCoreApplication::instance());
        Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
        if (!server) {
            server = new DevToolsSocketServer(QCoreApplication::instance());
        }
        return server;
    }

}

#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <memory>

QT_BEGIN_NAMESPACE
class QWebSocket;
class QWebSocketServer;
QT_END_NAMESPACE

namespace eden::engine::cef {

class DevToolsSocketServer final : public QObject {
    Q_OBJECT

  public:
    struct Session {
        QString token;
        QString endpoint;

        bool isValid() const {
            return !token.isEmpty() && !endpoint.isEmpty();
        }
    };

    using MessageHandler = std::function<void(const QByteArray &message)>;

    explicit DevToolsSocketServer(QObject *parent = nullptr);
    ~DevToolsSocketServer() override;

    Session openSession(MessageHandler handler);
    void closeSession(const QString &token);
    void sendMessage(const QString &token, const QByteArray &message);
    bool isListening() const;
    int sessionCount() const;
    int connectedSessionCount() const;
    quint16 port() const;

  private:
    struct SessionState {
        MessageHandler handler;
        QPointer<QWebSocket> socket;
        bool consumed = false;
    };

    void acceptPendingConnection();
    void rejectConnection(QWebSocket *socket);
    QString createToken() const;

    QHash<QString, SessionState> m_sessions;
    std::unique_ptr<QWebSocketServer> m_server;
};

DevToolsSocketServer *sharedDevToolsSocketServer();

}

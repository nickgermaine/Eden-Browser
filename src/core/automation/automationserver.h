#pragma once

#include <QJsonValue>
#include <QObject>
#include <QPointer>

#include <chrono>
#include <functional>
#include <memory>

class QLocalServer;
class QLocalSocket;
class QQuickWindow;

namespace eden::core {

    class WindowController;

    class AutomationServer final : public QObject {
        Q_OBJECT

      public:
        static std::unique_ptr<AutomationServer> createIfEnabled(QObject *parent = nullptr);
        AutomationServer(QString socketPath, QObject *parent = nullptr, std::function<void()> quitHandler = {});
        ~AutomationServer() override;

        bool start();
        QString socketPath() const;
        void attach(WindowController *controller, QQuickWindow *window);
        bool isAttached() const;

      private:
        bool validateSocketParent() const;
        void acceptConnection();
        void readSocket(QLocalSocket *socket);
        void handleRequest(QLocalSocket *socket, const QByteArray &line);
        void sendResult(QLocalSocket *socket, const QJsonValue &id, const QJsonValue &result);
        void sendError(QLocalSocket *socket, const QJsonValue &id, int code, const QString &message);
        bool typeInOmnibox(const QString &text);
        void recordNextFrame(
            const QString &name,
            std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now()
        );

        QPointer<WindowController> m_controller;
        QPointer<QQuickWindow> m_window;
        QString m_socketPath;
        std::unique_ptr<QLocalServer> m_server;
        std::function<void()> m_quitHandler;
        std::chrono::steady_clock::time_point m_previousCollectedFrame;
        quint64 m_idleDiagnosticFrameSwaps = 0;
        QMetaObject::Connection m_frameConnection;
    };

}

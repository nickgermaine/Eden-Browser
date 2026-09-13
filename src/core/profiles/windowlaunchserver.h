#pragma once

#include <QLocalServer>
#include <QObject>
#include <QUrl>

#include <functional>

namespace eden::core {

    struct WindowLaunchRequest {
        bool privateWindow = false;
        QString engineName;
        QList<QUrl> urls;
    };

    class WindowLaunchServer final : public QObject {
      public:
        using Handler = std::function<bool(const WindowLaunchRequest &)>;

        explicit WindowLaunchServer(const QString &dataRoot, QObject *parent = nullptr);
        ~WindowLaunchServer() override;
        bool listen(Handler handler);
        bool forward(const WindowLaunchRequest &request, int timeoutMilliseconds = 10000) const;
        QString socketName() const;

      private:
        void acceptConnections();

        QString m_socketName;
        QLocalServer m_server;
        Handler m_handler;
        int m_connections = 0;
    };

}

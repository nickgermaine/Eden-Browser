#include "core/profiles/windowlaunchserver.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <memory>
#include <optional>

namespace eden::core {

    namespace {

        constexpr qsizetype maximumRequestBytes = 256 * 1024;
        constexpr qsizetype maximumUrls = 128;

        std::optional<WindowLaunchRequest> decodeRequest(const QByteArray &bytes) {
            const QJsonDocument document = QJsonDocument::fromJson(bytes);
            if (!document.isObject()) {
                return {};
            }
            const QJsonObject object = document.object();
            if (object.value("version").toInt() != 1 || !object.value("private").isBool() ||
                !object.value("engine").isString() || !object.value("urls").isArray()) {
                return {};
            }
            WindowLaunchRequest request;
            request.privateWindow = object.value("private").toBool();
            request.engineName = object.value("engine").toString();
            const QJsonArray urls = object.value("urls").toArray();
            if (request.engineName.size() > 64 || urls.size() > maximumUrls) {
                return {};
            }
            for (const QJsonValue &value : urls) {
                if (!value.isString() || value.toString().size() > 65536) {
                    return {};
                }
                const QUrl url(value.toString(), QUrl::StrictMode);
                if (!url.isValid() || url.isEmpty() || url.isRelative()) {
                    return {};
                }
                request.urls.append(url);
            }
            return request;
        }

        QByteArray encodeRequest(const WindowLaunchRequest &request) {
            QJsonArray urls;
            for (const QUrl &url : request.urls) {
                urls.append(url.toString(QUrl::FullyEncoded));
            }
            return QJsonDocument(
                       QJsonObject{
                           {"version", 1},
                           {"private", request.privateWindow},
                           {"engine", request.engineName},
                           {"urls", urls}
                       }
                   ).toJson(QJsonDocument::Compact) +
                   '\n';
        }

    }

    WindowLaunchServer::WindowLaunchServer(const QString &dataRoot, QObject *parent)
        : QObject(parent) {
        const QFileInfo directory(dataRoot);
        const QString identity = directory.exists() ? directory.canonicalFilePath() : directory.absoluteFilePath();
        const QByteArray digest =
            QCryptographicHash::hash(QDir::cleanPath(identity).toUtf8(), QCryptographicHash::Sha256);
        m_socketName = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) +
                       QStringLiteral("/eden-window-") + QString::fromLatin1(digest.toHex().first(32));
        m_server.setSocketOptions(QLocalServer::UserAccessOption);
        m_server.setMaxPendingConnections(16);
        connect(&m_server, &QLocalServer::newConnection, this, &WindowLaunchServer::acceptConnections);
    }

    bool WindowLaunchServer::listen(Handler handler) {
        if (m_server.isListening() || !handler) {
            return false;
        }
        m_handler = std::move(handler);
        QLocalServer::removeServer(m_socketName);
        return m_server.listen(m_socketName);
    }

    WindowLaunchServer::~WindowLaunchServer() {
        m_server.close();
        const auto sockets = m_server.findChildren<QLocalSocket *>();
        qDeleteAll(sockets);
    }

    QString WindowLaunchServer::socketName() const {
        return m_socketName;
    }

    bool WindowLaunchServer::forward(const WindowLaunchRequest &request, int timeoutMilliseconds) const {
        const QByteArray bytes = encodeRequest(request);
        if (bytes.size() > maximumRequestBytes || !decodeRequest(bytes) || timeoutMilliseconds <= 0) {
            return false;
        }
        QElapsedTimer elapsed;
        elapsed.start();
        const auto remaining = [&] {
            return qMax(0, timeoutMilliseconds - static_cast<int>(elapsed.elapsed()));
        };
        QLocalSocket socket;
        socket.setReadBufferSize(32);
        while (remaining() > 0) {
            socket.connectToServer(m_socketName);
            if (socket.waitForConnected(qMin(remaining(), 250))) {
                break;
            }
            socket.abort();
            QThread::msleep(static_cast<unsigned long>(qMin(remaining(), 50)));
        }
        if (socket.state() != QLocalSocket::ConnectedState || socket.write(bytes) != bytes.size()) {
            return false;
        }
        while (socket.bytesToWrite() > 0) {
            if (remaining() == 0 || !socket.waitForBytesWritten(remaining())) {
                return false;
            }
        }
        QByteArray response;
        while (remaining() > 0 && response.size() < 32) {
            response.append(socket.readAll());
            if (response.contains('\n')) {
                return response == "accepted\n";
            }
            if (!socket.waitForReadyRead(remaining())) {
                response.append(socket.readAll());
                return response == "accepted\n";
            }
        }
        return false;
    }

    void WindowLaunchServer::acceptConnections() {
        while (m_server.hasPendingConnections()) {
            QLocalSocket *socket = m_server.nextPendingConnection();
            if (m_connections >= 16) {
                socket->abort();
                socket->deleteLater();
                continue;
            }
            ++m_connections;
            socket->setReadBufferSize(maximumRequestBytes + 1);
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QObject::destroyed, this, [this] { --m_connections; });
            QTimer::singleShot(5000, socket, [socket] { socket->abort(); });
            auto buffer = std::make_shared<QByteArray>();
            auto finished = std::make_shared<bool>(false);
            const auto receive = [this, socket, buffer, finished] {
                if (*finished) {
                    return;
                }
                buffer->append(socket->readAll());
                if (buffer->size() > maximumRequestBytes) {
                    *finished = true;
                    socket->abort();
                    return;
                }
                const qsizetype end = buffer->indexOf('\n');
                if (end < 0) {
                    return;
                }
                *finished = true;
                const auto request = end == buffer->size() - 1 ? decodeRequest(*buffer) : std::nullopt;
                const bool accepted = request && m_handler(*request);
                socket->write(accepted ? "accepted\n" : "rejected\n");
                socket->disconnectFromServer();
            };
            connect(socket, &QLocalSocket::readyRead, socket, receive);
            receive();
        }
    }

}

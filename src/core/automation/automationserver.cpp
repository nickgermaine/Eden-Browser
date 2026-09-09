#include "core/automation/automationserver.h"
#include "core/automation/performancemetrics.h"
#include "core/profiles/profilelistmodel.h"
#include "core/profiles/profilemanager.h"
#include "core/window/tabmodel.h"
#include "core/window/windowcontroller.h"
#include "engine/engineview.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSaveFile>
#include <QTimer>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <utility>

namespace eden::core {

    static constexpr qsizetype kMaximumRequestBytes = 64 * 1024;

    std::unique_ptr<AutomationServer> AutomationServer::createIfEnabled(QObject *parent) {
        if (qEnvironmentVariable("EDEN_AUTOMATION") != "1") {
            return {};
        }
        auto server = std::make_unique<AutomationServer>(qEnvironmentVariable("EDEN_AUTOMATION_SOCKET"), parent);
        if (!server->start()) {
            return {};
        }
        return server;
    }

    AutomationServer::AutomationServer(QString socketPath, QObject *parent, std::function<void()> quitHandler)
        : QObject(parent),
          m_socketPath(std::move(socketPath)),
          m_server(std::make_unique<QLocalServer>()),
          m_quitHandler(std::move(quitHandler)) {
        if (!m_quitHandler) {
            m_quitHandler = [] {
                QCoreApplication::quit();
            };
        }
        connect(m_server.get(), &QLocalServer::newConnection, this, &AutomationServer::acceptConnection);
    }

    void AutomationServer::attach(WindowController *controller, QQuickWindow *window) {
        if (m_frameConnection) {
            disconnect(m_frameConnection);
        }
        m_controller = controller;
        m_window = window;
        if (!window) {
            return;
        }
        m_frameConnection = connect(window, &QQuickWindow::frameSwapped, this, [this] {
            if (qEnvironmentVariableIsSet("EDEN_IDLE_DIAGNOSTICS")) {
                ++m_idleDiagnosticFrameSwaps;
            }
            const auto now = std::chrono::steady_clock::now();
            if (PerformanceMetrics::frameCollectionEnabled()) {
                if (m_previousCollectedFrame.time_since_epoch().count() != 0) {
                    PerformanceMetrics::record(
                        "frame.interval_ms",
                        std::chrono::duration<double, std::milli>(now - m_previousCollectedFrame).count()
                    );
                }
                m_previousCollectedFrame = now;
            } else {
                m_previousCollectedFrame = {};
            }
            const std::optional<double> milliseconds = PerformanceMetrics::completeIfReady("scroll.input_to_frame_ms");
            if (milliseconds && qEnvironmentVariableIsSet("EDEN_PERF")) {
                qInfo("EDEN_PERF scroll.input_to_frame_ms=%.3f", *milliseconds);
            }
        });
    }

    bool AutomationServer::isAttached() const {
        return m_controller && m_window;
    }

    AutomationServer::~AutomationServer() {
        if (qEnvironmentVariableIsSet("EDEN_IDLE_DIAGNOSTICS")) {
            qInfo(
                "EDEN_PERF diagnostic.shell_frame_swaps=%llu",
                static_cast<unsigned long long>(m_idleDiagnosticFrameSwaps)
            );
        }
        if (m_server->isListening()) {
            m_server->close();
            if (validateSocketParent()) {
                const QByteArray socketPath = QFile::encodeName(m_socketPath);
                struct stat attributes{};
                if (lstat(socketPath.constData(), &attributes) == 0 && S_ISSOCK(attributes.st_mode) &&
                    attributes.st_uid == geteuid()) {
                    QLocalServer::removeServer(m_socketPath);
                }
            }
        }
    }

    bool AutomationServer::start() {
        if (!validateSocketParent()) {
            qWarning("The automation socket path failed security validation");
            return false;
        }
        const QByteArray socketPath = QFile::encodeName(m_socketPath);
        struct stat attributes{};
        if (lstat(socketPath.constData(), &attributes) == 0) {
            if (!S_ISSOCK(attributes.st_mode) || attributes.st_uid != geteuid() ||
                !QLocalServer::removeServer(m_socketPath)) {
                return false;
            }
        } else if (errno != ENOENT) {
            return false;
        }
        if (!m_server->listen(m_socketPath)) {
            qWarning().noquote() << "The automation socket could not listen:" << m_server->errorString();
            return false;
        }
        return true;
    }

    QString AutomationServer::socketPath() const {
        return m_socketPath;
    }

    bool AutomationServer::validateSocketParent() const {
        const QFileInfo socketInfo(m_socketPath);
        if (!socketInfo.isAbsolute() || socketInfo.fileName().isEmpty()) {
            return false;
        }
        const QFileInfo runtimeInfo(qEnvironmentVariable("XDG_RUNTIME_DIR"));
        const QString canonicalRuntime = runtimeInfo.canonicalFilePath();
        const QString canonicalParent = QFileInfo(socketInfo.absolutePath()).canonicalFilePath();
        if (!runtimeInfo.isAbsolute() || canonicalRuntime.isEmpty() || canonicalParent.isEmpty() ||
            (canonicalParent != canonicalRuntime && !canonicalParent.startsWith(canonicalRuntime + '/'))) {
            return false;
        }
        const QByteArray parentPath = QFile::encodeName(socketInfo.absolutePath());
        struct stat attributes{};
        if (lstat(parentPath.constData(), &attributes) != 0 || !S_ISDIR(attributes.st_mode) ||
            S_ISLNK(attributes.st_mode)) {
            return false;
        }
        return attributes.st_uid == geteuid() && (attributes.st_mode & 0777) == 0700;
    }

    void AutomationServer::acceptConnection() {
        while (QLocalSocket *socket = m_server->nextPendingConnection()) {
            socket->setParent(this);
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] { readSocket(socket); });
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        }
    }

    void AutomationServer::readSocket(QLocalSocket *socket) {
        if (socket->bytesAvailable() > kMaximumRequestBytes || socket->peek(socket->bytesAvailable()).contains('\0')) {
            socket->abort();
            return;
        }
        while (socket->canReadLine()) {
            const QByteArray line = socket->readLine();
            if (line.size() > kMaximumRequestBytes || line.contains('\0')) {
                socket->abort();
                return;
            }
            handleRequest(socket, line.trimmed());
        }
    }

    void AutomationServer::handleRequest(QLocalSocket *socket, const QByteArray &line) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            sendError(socket, {}, -32700, "Invalid JSON");
            return;
        }
        const QJsonObject request = document.object();
        const QJsonValue id = request.value("id");
        if (request.value("jsonrpc").toString() != "2.0" || !request.value("method").isString()) {
            sendError(socket, id, -32600, "Invalid request");
            return;
        }
        const QString method = request.value("method").toString();
        const QJsonObject parameters = request.value("params").toObject();
        if (method == "dumpMetrics") {
            const qint64 after = parameters.value("afterSequence").toInteger(0);
            QJsonObject result;
            result.insert("sequence", static_cast<qint64>(PerformanceMetrics::sequence()));
            result.insert(
                "samples",
                PerformanceMetrics::samplesAfter(static_cast<quint64>(std::max<qint64>(0, after)))
            );
            sendResult(socket, id, result);
            return;
        }
        if (method == "beginFrameCollection") {
            PerformanceMetrics::setFrameCollectionEnabled(true);
            sendResult(socket, id, true);
            return;
        }
        if (method == "endFrameCollection") {
            PerformanceMetrics::setFrameCollectionEnabled(false);
            sendResult(socket, id, true);
            return;
        }
        if (method == "quit") {
            sendResult(socket, id, true);
            socket->flush();
            QTimer::singleShot(0, this, [this] { m_quitHandler(); });
            return;
        }
        if (method == "listProfiles") {
            ProfileManager *profiles = ProfileManager::instance();
            if (!profiles) {
                sendError(socket, id, -32000, "Profiles are unavailable");
                return;
            }
            QJsonArray result;
            ProfileListModel *model = profiles->profileModel();
            for (int row = 0; row < model->rowCount(); ++row) {
                const QModelIndex modelIndex = model->index(row);
                QJsonObject entry;
                entry.insert("profileId", model->data(modelIndex, ProfileListModel::ProfileIdRole).toString());
                entry.insert("displayName", model->data(modelIndex, ProfileListModel::DisplayNameRole).toString());
                entry.insert("protected", model->data(modelIndex, ProfileListModel::ProtectedProfileRole).toBool());
                entry.insert("sessionState", model->data(modelIndex, ProfileListModel::SessionStateRole).toString());
                entry.insert("current", model->data(modelIndex, ProfileListModel::CurrentProfileRole).toBool());
                result.append(entry);
            }
            sendResult(socket, id, result);
            return;
        }
        if (method == "currentProfile") {
            sendResult(socket, id, m_controller ? m_controller->profileId() : QString());
            return;
        }
        if (method == "createProfile") {
            ProfileManager *profiles = ProfileManager::instance();
            const QString name = parameters.value("name").toString();
            if (!profiles || name.isEmpty()) {
                sendError(socket, id, -32602, "Invalid profile name");
                return;
            }
            const QString password = parameters.value("password").toString();
            profiles->createProfile(name, !password.isEmpty(), password, password, QUrl());
            sendResult(socket, id, true);
            return;
        }
        if (method == "activateProfile") {
            ProfileManager *profiles = ProfileManager::instance();
            if (!profiles) {
                sendError(socket, id, -32000, "Profiles are unavailable");
                return;
            }
            profiles->activateProfile(parameters.value("profileId").toString());
            sendResult(socket, id, true);
            return;
        }
        if (method == "submitProfilePassword") {
            ProfileManager *profiles = ProfileManager::instance();
            if (!profiles) {
                sendError(socket, id, -32000, "Profiles are unavailable");
                return;
            }
            profiles->submitPassword(parameters.value("profileId").toString(), parameters.value("password").toString());
            sendResult(socket, id, true);
            return;
        }
        if (method == "profileCooldownSeconds") {
            ProfileManager *profiles = ProfileManager::instance();
            if (!profiles) {
                sendError(socket, id, -32000, "Profiles are unavailable");
                return;
            }
            sendResult(socket, id, profiles->passwordCooldownSeconds(parameters.value("profileId").toString()));
            return;
        }
        if (method == "signOutProfile") {
            ProfileManager *profiles = ProfileManager::instance();
            if (!profiles) {
                sendError(socket, id, -32000, "Profiles are unavailable");
                return;
            }
            const QString profileId = parameters.value("profileId").toString();
            profiles->signOut(profileId);
            profiles->confirmSignOut(profileId, true);
            sendResult(socket, id, true);
            return;
        }
        if (!m_controller || !m_window) {
            sendError(socket, id, -32001, "The browser window is not ready");
            return;
        }
        if (method == "openTab") {
            const QUrl url(parameters.value("url").toString());
            if (!url.isValid() || url.isEmpty()) {
                sendError(socket, id, -32602, "Invalid URL");
                return;
            }
            const QString backendId = parameters.value("backend").toString();
            sendResult(socket, id, m_controller->newTab(url, false, backendId));
        } else if (method == "activateTab") {
            if (!parameters.value("index").isDouble()) {
                sendError(socket, id, -32602, "Invalid tab index");
                return;
            }
            const int index = parameters.value("index").toInt(-1);
            if (!m_controller->tabs() || index < 0 || index >= m_controller->tabs()->rowCount()) {
                sendError(socket, id, -32602, "Invalid tab index");
                return;
            }
            m_controller->setActiveIndex(index);
            m_window->update();
            sendResult(socket, id, true);
        } else if (method == "typeInOmnibox") {
            if (!parameters.value("text").isString() || parameters.value("text").toString().isEmpty() ||
                !typeInOmnibox(parameters.value("text").toString())) {
                sendError(socket, id, -32602, "Omnibox is unavailable");
                return;
            }
            sendResult(socket, id, true);
        } else if (method == "reorderTab") {
            if (!parameters.value("from").isDouble() || !parameters.value("to").isDouble() || !m_controller->tabs()) {
                sendError(socket, id, -32602, "Invalid tab move");
                return;
            }
            const int from = parameters.value("from").toInt(-1);
            const int to = parameters.value("to").toInt(-1);
            if (from < 0 || to < 0 || from >= m_controller->tabs()->rowCount() ||
                to >= m_controller->tabs()->rowCount() || from == to) {
                sendError(socket, id, -32602, "Invalid tab move");
                return;
            }
            const auto started = std::chrono::steady_clock::now();
            if (!m_controller->tabs()->moveTab(from, to)) {
                sendError(socket, id, -32602, "Invalid tab move");
                return;
            }
            m_window->update();
            recordNextFrame("reorder.input_to_frame_ms", started);
            sendResult(socket, id, true);
        } else if (method == "wheel") {
            auto *view = qobject_cast<engine::EngineView *>(m_controller->currentEngine());
            PerformanceMetrics::begin("scroll.input_to_frame_ms");
            PerformanceMetrics::begin("scroll.input_to_page_event_ms");
            if (!view || !view->automationWheel(parameters.value("delta").toInt(-120))) {
                PerformanceMetrics::cancel("scroll.input_to_frame_ms");
                PerformanceMetrics::cancel("scroll.input_to_page_event_ms");
                sendError(socket, id, -32602, "Active engine cannot receive wheel input");
                return;
            }
            sendResult(socket, id, true);
        } else if (method == "captureWindow") {
            const QFileInfo target(parameters.value("path").toString());
            const QString temporaryRoot = QFileInfo(QDir::tempPath()).canonicalFilePath();
            const QString targetParent = target.absoluteDir().canonicalPath();
            if (!parameters.value("path").isString() || !target.isAbsolute() || target.exists() ||
                target.suffix().compare("png", Qt::CaseInsensitive) != 0 || temporaryRoot.isEmpty() ||
                targetParent.isEmpty() ||
                (targetParent != temporaryRoot && !targetParent.startsWith(temporaryRoot + '/'))) {
                sendError(socket, id, -32602, "Invalid capture path");
                return;
            }
            QSaveFile output(target.absoluteFilePath());
            const QImage capture = m_window->grabWindow();
            if (capture.isNull() || !output.open(QIODevice::WriteOnly) || !capture.save(&output, "PNG") ||
                !output.commit()) {
                sendError(socket, id, -32603, "Window capture failed");
                return;
            }
            sendResult(socket, id, true);
        } else if (method == "pointerMove" || method == "pointerClick") {
            if (!parameters.value("x").isDouble() || !parameters.value("y").isDouble()) {
                sendError(socket, id, -32602, "Invalid pointer position");
                return;
            }
            const QPointF position(parameters.value("x").toDouble(), parameters.value("y").toDouble());
            const QPointF global = m_window->mapToGlobal(position.toPoint());
            if (method == "pointerMove") {
                QMouseEvent
                    event(QEvent::MouseMove, position, position, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(m_window, &event);
            } else {
                const Qt::MouseButton button =
                    parameters.value("button").toString() == "right" ? Qt::RightButton : Qt::LeftButton;
                QMouseEvent press(QEvent::MouseButtonPress, position, position, global, button, button, Qt::NoModifier);
                QMouseEvent release(
                    QEvent::MouseButtonRelease,
                    position,
                    position,
                    global,
                    button,
                    Qt::NoButton,
                    Qt::NoModifier
                );
                QCoreApplication::sendEvent(m_window, &press);
                QCoreApplication::sendEvent(m_window, &release);
            }
            sendResult(socket, id, true);
        } else if (method == "reload") {
            m_controller->reload();
            sendResult(socket, id, true);
        } else if (method == "resolveFileDialog") {
            QList<QUrl> files;
            const QJsonArray fileValues = parameters.value("files").toArray();
            for (const QJsonValue &value : fileValues) {
                files.append(QUrl::fromLocalFile(value.toString()));
            }
            m_controller->resolveFileDialog(parameters.value("accepted").toBool(), files);
            sendResult(socket, id, true);
        } else if (method == "resolvePermission") {
            m_controller->resolvePermissionRequest(parameters.value("allowed").toBool());
            sendResult(socket, id, true);
        } else if (method == "navigationHistory") {
            sendResult(
                socket,
                id,
                QJsonArray::fromVariantList(m_controller->navigationHistory(parameters.value("direction").toInt(-1)))
            );
        } else if (method == "duplicateTab") {
            const int index = parameters.value("index").toInt(-1);
            if (!m_controller->tabs() || index < 0 || index >= m_controller->tabs()->rowCount()) {
                sendError(socket, id, -32602, "Invalid tab index");
                return;
            }
            sendResult(socket, id, m_controller->tabs()->duplicateTab(index));
        } else if (method == "tabPreview") {
            const int index = parameters.value("index").toInt(-1);
            sendResult(socket, id, QJsonObject::fromVariantMap(m_controller->tabPreview(index)));
        } else if (method == "tabContextMenuActions") {
            const int index = parameters.value("index").toInt(-1);
            sendResult(socket, id, QJsonArray::fromVariantList(m_controller->tabContextMenuActions(index)));
        } else if (method == "switchEngine") {
            sendResult(socket, id, m_controller->switchEngine(parameters.value("backendId").toString()));
        } else {
            sendError(socket, id, -32601, "Method not found");
        }
    }

    void AutomationServer::sendResult(QLocalSocket *socket, const QJsonValue &id, const QJsonValue &result) {
        QJsonObject response;
        response.insert("jsonrpc", "2.0");
        response.insert("id", id);
        response.insert("result", result);
        socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
        socket->flush();
    }

    void AutomationServer::sendError(QLocalSocket *socket, const QJsonValue &id, int code, const QString &message) {
        QJsonObject error;
        error.insert("code", code);
        error.insert("message", message);
        QJsonObject response;
        response.insert("jsonrpc", "2.0");
        response.insert("id", id);
        response.insert("error", error);
        socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
        socket->flush();
    }

    bool AutomationServer::typeInOmnibox(const QString &text) {
        QQuickItem *field = m_window->findChild<QQuickItem *>("omniboxField");
        if (!field) {
            return false;
        }
        field->forceActiveFocus(Qt::OtherFocusReason);
        recordNextFrame("input.key_to_frame_ms");
        for (const QChar character : text) {
            const QString value(character);
            QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, value);
            QCoreApplication::sendEvent(field, &press);
            QKeyEvent release(QEvent::KeyRelease, 0, Qt::NoModifier, value);
            QCoreApplication::sendEvent(field, &release);
        }
        m_window->update();
        return true;
    }

    void AutomationServer::recordNextFrame(const QString &name, std::chrono::steady_clock::time_point started) {
        auto connection = std::make_shared<QMetaObject::Connection>();
        *connection = connect(m_window, &QQuickWindow::frameSwapped, m_window, [connection, name, started] {
            disconnect(*connection);
            const double milliseconds =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            PerformanceMetrics::record(name, milliseconds);
            if (qEnvironmentVariableIsSet("EDEN_PERF")) {
                qInfo().noquote() << "EDEN_PERF" << name + '=' + QString::number(milliseconds, 'f', 3);
            }
        });
    }

}

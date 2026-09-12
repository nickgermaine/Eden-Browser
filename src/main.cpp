#include "core/profiles/enginestorage.h"
#if EDEN_ENABLE_AUTOMATION
#include "core/automation/automationserver.h"
#include "core/automation/performancemetrics.h"
#endif
#include "core/profiles/applicationcontext.h"
#include "core/profiles/profileeditorcontroller.h"
#include "core/profiles/profilelistmodel.h"
#include "core/profiles/profilemanager.h"
#include "core/profiles/profilesettings.h"
#include "core/profiles/windowregistry.h"
#include "core/settings/settingsstore.h"
#include "core/settings/theme/thememanager.h"
#include "core/window/tabstripnavigator.h"
#include "core/window/windowcontroller.h"
#include "core/window/windowframe.h"
#include "engine/enginefactory.h"
#include "engine/engineregistry.h"
#include "engine/engineview.h"
#include "platform/waylandglibeventdispatcher.h"

#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>

#include <chrono>
#include <memory>

#include <sys/resource.h>
#include <unistd.h>

class PerformanceInputProbe final : public QObject {
  public:
    explicit PerformanceInputProbe(QQuickWindow *window)
        : QObject(window),
          m_window(window) {
        m_window->installEventFilter(this);
        connect(m_window, &QQuickWindow::frameSwapped, this, [this] {
            const auto now = std::chrono::steady_clock::now();
            if (m_inputPending) {
                const double milliseconds = std::chrono::duration<double, std::milli>(now - m_inputStarted).count();
#if EDEN_ENABLE_AUTOMATION
                eden::core::PerformanceMetrics::record("input.key_to_frame_ms", milliseconds);
#endif
                qInfo("EDEN_PERF input.key_to_frame_ms=%.3f", milliseconds);
                m_inputPending = false;
            }
            if (
                qEnvironmentVariableIsSet("EDEN_BENCH_FRAMES")
#if EDEN_ENABLE_AUTOMATION
                && eden::core::PerformanceMetrics::frameCollectionEnabled()
#endif
            ) {
                if (m_previousFrame.time_since_epoch().count() != 0) {
                    const double milliseconds =
                        std::chrono::duration<double, std::milli>(now - m_previousFrame).count();
                    qInfo("EDEN_PERF frame.interval_ms=%.3f", milliseconds);
                }
                m_previousFrame = now;
            }
        });
    }

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == m_window && event->type() == QEvent::KeyPress && QGuiApplication::focusObject() &&
            QGuiApplication::focusObject()->objectName() == "omniboxField") {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (!keyEvent->isAutoRepeat() && !keyEvent->text().isEmpty()) {
                m_inputStarted = std::chrono::steady_clock::now();
                m_inputPending = true;
            }
        }
        return QObject::eventFilter(watched, event);
    }

  private:
    QQuickWindow *m_window;
    std::chrono::steady_clock::time_point m_inputStarted;
    std::chrono::steady_clock::time_point m_previousFrame;
    bool m_inputPending = false;
};

static double residentMemoryMb() {
    QFile status("/proc/self/status");
    if (status.open(QIODevice::ReadOnly)) {
        while (!status.atEnd()) {
            const QByteArray line = status.readLine();
            if (!line.startsWith("VmRSS:")) {
                continue;
            }
            const QList<QByteArray> fields = line.simplified().split(' ');
            if (fields.size() >= 2) {
                const double value = fields.at(1).toDouble() / 1024.0;
                if (value > 0) {
                    return value;
                }
            }
        }
    }
    QFile rollup("/proc/self/smaps_rollup");
    if (rollup.open(QIODevice::ReadOnly)) {
        while (!rollup.atEnd()) {
            const QByteArray line = rollup.readLine();
            if (!line.startsWith("Rss:")) {
                continue;
            }
            const QList<QByteArray> fields = line.simplified().split(' ');
            if (fields.size() >= 2) {
                const double value = fields.at(1).toDouble() / 1024.0;
                if (value > 0) {
                    return value;
                }
            }
        }
    }
    QFile statm("/proc/self/statm");
    if (statm.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> fields = statm.readAll().simplified().split(' ');
        const long pageSize = sysconf(_SC_PAGESIZE);
        if (fields.size() >= 2 && pageSize > 0) {
            const double value = fields.at(1).toDouble() * static_cast<double>(pageSize) / (1024.0 * 1024.0);
            if (value > 0) {
                return value;
            }
        }
    }
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss > 0) {
        return static_cast<double>(usage.ru_maxrss) / 1024.0;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    const qint64 startupNanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    qputenv("EDEN_START_TIME_NS", QByteArray::number(startupNanoseconds));
    QElapsedTimer startupTimer;
    startupTimer.start();
    const auto logStartupStage = [&startupTimer](const char *stage) {
        if (qEnvironmentVariableIsSet("EDEN_PERF")) {
#if EDEN_ENABLE_AUTOMATION
            eden::core::PerformanceMetrics::record(
                QStringLiteral("startup.stage.") + QString::fromLatin1(stage),
                startupTimer.elapsed()
            );
#endif
            qInfo("EDEN_PERF startup.stage.%s=%lld", stage, startupTimer.elapsed());
        }
    };
    bool windowedCefRequested = false;
    for (int index = 1; index < argc; ++index) {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        if (argument == "--engine-compositing=windowed") {
            windowedCefRequested = true;
        }
    }
    if (windowedCefRequested) {
        qputenv("QT_QPA_PLATFORM", "xcb");
        qunsetenv("WAYLAND_DISPLAY");
        qputenv("EGL_PLATFORM", "x11");
    } else {
        if (qEnvironmentVariableIsSet("WAYLAND_DISPLAY")) {
            qputenv("QT_QPA_PLATFORM", "wayland");
            qputenv("EGL_PLATFORM", "wayland");
            qputenv("QSG_RHI_BACKEND", "opengl");
            QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
        } else {
            qputenv("QT_QPA_PLATFORM", "xcb");
            qputenv("EGL_PLATFORM", "x11");
            QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        }
    }
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORMTHEME")) {
        qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
    }
    if (!qEnvironmentVariableIsSet("QT_NO_GLIB")) {
        qputenv("QT_NO_GLIB", "1");
    }
    eden::engine::EngineFactory::configureApplicationArguments(argc, argv);
    QCoreApplication::setApplicationName("Eden");
    QCoreApplication::setApplicationVersion(EDEN_VERSION);
    QCoreApplication::setOrganizationName("Eden");
    QCoreApplication::setOrganizationDomain("eden.browser");
    QGuiApplication::setDesktopFileName("eden-browser");
#if EDEN_ENGINE_QTWEBENGINE
    eden::engine::EngineFactory::prepareApplication(eden::engine::Backend::QtWebEngine);
#endif
    QGuiApplication application(argc, argv);
    logStartupStage("qgui_application_constructed_ms");
    Q_INIT_RESOURCE(eden_ui_raw_res_0);
    QQuickStyle::setStyle("Basic");
    QFontDatabase::addApplicationFont(":/qt/qml/Eden/Ui/resources/fonts/RobotoFlex.ttf");

    std::unique_ptr<eden::platform::WaylandGlibEventDispatcher> waylandGlibDispatcher;
    if (QGuiApplication::platformName() == "wayland" && qEnvironmentVariableIntValue("QT_NO_GLIB") != 0) {
        waylandGlibDispatcher = std::make_unique<eden::platform::WaylandGlibEventDispatcher>();
    }

    eden::core::ApplicationContext applicationContext;
    if (!applicationContext.acquireSingleInstanceLock()) {
        qCritical("Another Eden window already manages this profile data");
        return 0;
    }

    qmlRegisterType<eden::core::WindowController>("Eden.Ui", 1, 0, "WindowController");
    qmlRegisterType<eden::core::WindowFrame>("Eden.Ui", 1, 0, "WindowFrame");
    qmlRegisterType<eden::core::TabStripNavigator>("Eden.Ui", 1, 0, "TabStripNavigator");
    qmlRegisterUncreatableType<eden::engine::EngineView>(
        "Eden.Ui",
        1,
        0,
        "EngineView",
        "Engine views are created by WindowController"
    );
    qmlRegisterUncreatableType<eden::core::ProfileListModel>(
        "Eden.Ui",
        1,
        0,
        "ProfileListModel",
        "The profile list is owned by ProfileManager"
    );
    qmlRegisterUncreatableType<eden::core::ProfileEditorController>(
        "Eden.Ui",
        1,
        0,
        "ProfileEditorController",
        "Profile editors are created by ProfileManager"
    );
    qmlRegisterUncreatableType<eden::core::ProfileSettings>(
        "Eden.Ui",
        1,
        0,
        "ProfileSettings",
        "Profile settings are owned by ProfileContext"
    );
    qmlRegisterSingletonInstance("Eden.Ui", 1, 0, "Engines", eden::engine::EngineRegistry::instance());
    qmlRegisterSingletonInstance("Eden.Ui", 1, 0, "Settings", eden::core::SettingsStore::instance());
    qmlRegisterSingletonInstance("Eden.Ui", 1, 0, "Themes", eden::core::ThemeManager::instance());
    qmlRegisterSingletonInstance("Eden.Ui", 1, 0, "Profiles", applicationContext.profiles());

    QString engineName;
    bool privateWindow = false;
    QList<QUrl> launchUrls;
    const QStringList arguments = application.arguments();
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        const QString &argument = arguments.at(index);
        if (argument.startsWith("--engine=")) {
            engineName = argument.sliced(9);
        } else if (argument == "--private") {
            privateWindow = true;
        } else if (!argument.startsWith("--")) {
            const QUrl url = QUrl::fromUserInput(argument);
            if (url.isValid() && !url.isEmpty()) {
                launchUrls.append(url);
            }
        }
    }
    applicationContext.profiles()->configureLaunch(privateWindow, engineName, launchUrls);
    bool holdAtShellStage = false;
#if EDEN_ENABLE_AUTOMATION
    if (qEnvironmentVariable("EDEN_AUTOMATION") == "1") {
        holdAtShellStage = qEnvironmentVariable("EDEN_AUTOMATION_ENGINE_STAGE") == "shell";
    }
#endif

    QQmlApplicationEngine engine;
    logStartupStage("qml_engine_constructed_ms");
    applicationContext.profiles()->setQmlEngine(&engine);
    engine.loadFromModule("Eden.Ui", "LaunchWindow");
    logStartupStage("qml_module_loaded_ms");
    if (engine.rootObjects().isEmpty()) {
        eden::engine::EngineFactory::shutdown();
        return 1;
    }

    QQuickWindow *launchWindow = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
#if EDEN_ENABLE_AUTOMATION
    std::unique_ptr<eden::core::AutomationServer> automationServer =
        eden::core::AutomationServer::createIfEnabled(&application);
    if (qEnvironmentVariable("EDEN_AUTOMATION") == "1" && !automationServer) {
        qCritical("The local automation socket could not be initialized");
        eden::engine::EngineFactory::shutdown();
        return 1;
    }
#endif
    if (launchWindow) {
        applicationContext.profiles()->setLaunchWindow(launchWindow);
        auto startupConnection = std::make_shared<QMetaObject::Connection>();
        *startupConnection = QObject::connect(
            launchWindow,
            &QQuickWindow::frameSwapped,
            launchWindow,
            [startupConnection, startupTimer, holdAtShellStage, profiles = applicationContext.profiles()]() mutable {
                if (!*startupConnection) {
                    return;
                }
                QObject::disconnect(*startupConnection);
                *startupConnection = {};
                if (qEnvironmentVariableIsSet("EDEN_PERF")) {
#if EDEN_ENABLE_AUTOMATION
                    eden::core::PerformanceMetrics::record("memory.shell_first_frame_rss_mb", residentMemoryMb());
                    eden::core::PerformanceMetrics::record("startup.shell_first_frame_ms", startupTimer.elapsed());
#endif
                    qInfo("EDEN_PERF memory.shell_first_frame_rss_mb=%.3f", residentMemoryMb());
                    qInfo("EDEN_PERF startup.shell_first_frame_ms=%lld", startupTimer.elapsed());
                }
                eden::core::SettingsStore::instance()->initialize();
                eden::core::ThemeManager::instance()->refresh();
                if (holdAtShellStage) {
                    return;
                }
                profiles->beginStartup();
            }
        );
    }
#if EDEN_ENABLE_AUTOMATION
    if (automationServer) {
        QObject::connect(
            applicationContext.profiles(),
            &eden::core::ProfileManager::profileOpened,
            automationServer.get(),
            [server = automationServer.get(), windows = applicationContext.windows()](const QString &profileId) {
                if (server->isAttached()) {
                    return;
                }
                eden::core::WindowController *controller = windows->mostRecentProfileController(profileId, false);
                if (controller && controller->window()) {
                    server->attach(controller, controller->window());
                    if (qEnvironmentVariableIsSet("EDEN_PERF")) {
                        new PerformanceInputProbe(controller->window());
                    }
                }
            }
        );
    } else
#endif
        if (qEnvironmentVariableIsSet("EDEN_PERF")) {
        QObject::connect(
            applicationContext.profiles(),
            &eden::core::ProfileManager::profileOpened,
            &application,
            [windows = applicationContext.windows()](const QString &profileId) {
                static bool probeInstalled = false;
                if (probeInstalled) {
                    return;
                }
                eden::core::WindowController *controller = windows->mostRecentProfileController(profileId, false);
                if (controller && controller->window()) {
                    probeInstalled = true;
                    new PerformanceInputProbe(controller->window());
                }
            }
        );
    }
    QObject::connect(
        &application,
        &QGuiApplication::aboutToQuit,
        &application,
        [profiles = applicationContext.profiles()] { profiles->handleAboutToQuit(); }
    );

    const int exitCode = application.exec();
    applicationContext.profiles()->shutdownContexts();
    const QList<QObject *> rootObjects = engine.rootObjects();
    for (QObject *rootObject : rootObjects) {
        delete rootObject;
    }
    engine.collectGarbage();
    engine.clearComponentCache();
    eden::engine::EngineFactory::shutdown();
    eden::core::EngineStorage::instance()->shutdown();
    return exitCode;
}

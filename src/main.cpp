#include "core/settings/settingsstore.h"
#include "core/settings/theme/thememanager.h"
#include "core/window/tabstripnavigator.h"
#include "core/window/windowcontroller.h"
#include "core/window/windowframe.h"
#include "engine/enginefactory.h"
#include "engine/engineview.h"

#include <QElapsedTimer>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>

#include <memory>

int main(int argc, char *argv[]) {
    QElapsedTimer startupTimer;
    startupTimer.start();
    eden::engine::EngineFactory::initialize();
    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName("Eden");
    QCoreApplication::setApplicationVersion(EDEN_VERSION);
    QCoreApplication::setOrganizationName("Eden");
    QCoreApplication::setOrganizationDomain("eden.browser");
    QQuickStyle::setStyle("Basic");
    QFontDatabase::addApplicationFont(":/qt/qml/Eden/Ui/resources/fonts/RobotoFlex.ttf");

    qmlRegisterType<eden::core::WindowController>("Eden.Ui", 1, 0, "WindowController");
    qmlRegisterType<eden::core::WindowFrame>("Eden.Ui", 1, 0, "WindowFrame");
    qmlRegisterType<eden::core::TabStripNavigator>("Eden.Ui", 1, 0, "TabStripNavigator");
    qmlRegisterUncreatableType<eden::engine::EngineView>("Eden.Ui", 1, 0, "EngineView", "Engine views are created by WindowController");
    qmlRegisterSingletonInstance("Eden.Ui", 1, 0, "Settings", eden::core::SettingsStore::instance());
    qmlRegisterSingletonInstance("Eden.Ui", 1, 0, "Themes", eden::core::ThemeManager::instance());

    QString engineName = "qtwebengine";
    bool privateWindow = false;
    for (const QString &argument : application.arguments()) {
        if (argument.startsWith("--engine=")) {
            engineName = argument.sliced(9);
        } else if (argument == "--private") {
            privateWindow = true;
        }
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("startupPrivateWindow", privateWindow);
    engine.rootContext()->setContextProperty("startupEngineName", engineName);
    engine.loadFromModule("Eden.Ui", "BrowserWindow");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    QQuickWindow *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    eden::core::WindowController *controller = window ? window->findChild<eden::core::WindowController *>("windowController") : nullptr;
    if (window && controller) {
        auto startupConnection = std::make_shared<QMetaObject::Connection>();
        *startupConnection = QObject::connect(window, &QQuickWindow::frameSwapped, window,
                                              [startupConnection, controller, privateWindow, engineName, startupTimer]() mutable {
                                                  QObject::disconnect(*startupConnection);
                                                  if (qEnvironmentVariableIsSet("EDEN_PERF")) {
                                                      qInfo("EDEN_PERF startup.shell_first_frame_ms=%lld", startupTimer.elapsed());
                                                  }
                                                  eden::core::SettingsStore::instance()->initialize();
                                                  eden::core::ThemeManager::instance()->refresh();
                                                  controller->initialize(privateWindow, engineName);
                                              });
    } else if (controller) {
        controller->initialize(privateWindow, engineName);
    }

    return application.exec();
}

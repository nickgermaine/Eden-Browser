#include "engine/engineplugin.h"
#include "engine/wpe/wpeengineview.h"
#include "engine/wpe/wpeprofile.h"
#include "engine/wpe/wperuntime.h"

#include <QCoreApplication>
#include <QDir>
#include <QQuickWindow>

namespace eden::engine {

    static bool prepareWpe(int, char *[]) {
        if (!QCoreApplication::instance()) {
            if (!qEnvironmentVariableIsSet("QSG_RHI_BACKEND")) {
                QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
            }
            if (!qEnvironmentVariableIsSet("QT_XCB_GL_INTEGRATION")) {
                qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");
            }
        }
        return true;
    }

    static bool initializeWpe(int, char *[], const EngineIdentity *identity) {
        if (!identity || !identity->product || !identity->version) {
            return false;
        }
        return WpeRuntime::initialize(QString::fromUtf8(identity->product), QString::fromUtf8(identity->version));
    }

    static EngineView *createWpeView(EngineProfile *profile) {
        auto *native = qobject_cast<WpeProfile *>(profile);
        return native && native->storageReady() && WpeRuntime::display() ? new WpeEngineView(native) : nullptr;
    }

    static EngineProfile *createWpeProfile(const EngineProfileParameters *parameters, QQmlEngine *, QObject *parent) {
        if (!parameters || parameters->backend != Backend::Wpe || !WpeRuntime::display()) {
            return nullptr;
        }
        if (!parameters->privateProfile &&
            (!QDir::isAbsolutePath(parameters->dataPath) || !QDir::isAbsolutePath(parameters->cachePath))) {
            return nullptr;
        }
        auto *profile = new WpeProfile(*parameters, parent);
        if (!profile->storageReady()) {
            delete profile;
            return nullptr;
        }
        return profile;
    }

}

extern "C" Q_DECL_EXPORT const eden::engine::EnginePluginApi *eden_engine_plugin() {
    static const eden::engine::EnginePluginApi api{
        eden::engine::enginePluginAbiVersion,
        eden::engine::prepareWpe,
        eden::engine::initializeWpe,
        eden::engine::createWpeView,
        eden::engine::createWpeProfile,
        eden::engine::WpeRuntime::shutdown
    };
    return &api;
}

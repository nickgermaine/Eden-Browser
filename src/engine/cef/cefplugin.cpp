#include "engine/cef/cefengineview.h"
#include "engine/cef/cefprofile.h"
#include "engine/cef/cefruntime.h"
#include "engine/engineplugin.h"

namespace eden::engine::cef {

    static bool initializeCef(int argc, char *argv[], const EngineIdentity *identity) {
        if (!identity || !identity->product || !identity->version) {
            return false;
        }
        return CefRuntime::instance().initialize(argc, argv, identity->product, identity->version);
    }

    static EngineView *createCefView(EngineProfile *profile) {
        return new CefEngineView(profile);
    }

    static EngineProfile *createCefProfile(bool privateProfile, QQmlEngine *, QObject *parent) {
        return new CefProfile(privateProfile, parent);
    }

    static void shutdownCef() {
        CefRuntime::instance().shutdown();
    }

}

extern "C" const eden::engine::EnginePluginApi *eden_engine_plugin() {
    static const eden::engine::EnginePluginApi api{
        eden::engine::enginePluginAbiVersion,
        eden::engine::cef::initializeCef,
        eden::engine::cef::createCefView,
        eden::engine::cef::createCefProfile,
        eden::engine::cef::shutdownCef
    };
    return &api;
}

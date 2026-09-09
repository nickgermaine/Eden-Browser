#include "engine/cef/cefengineview.h"
#include "engine/cef/cefprofile.h"
#include "engine/cef/cefruntime.h"
#include "engine/engineplugin.h"

#include <filesystem>

namespace eden::engine::cef {

    static bool initializeCef(int argc, char *argv[], const EngineIdentity *identity) {
        if (!identity || !identity->product || !identity->version || !identity->engineDataRoot) {
            return false;
        }
        const std::filesystem::path profilesRoot = std::filesystem::path(identity->engineDataRoot) / "profiles";
        return CefRuntime::instance().initialize(argc, argv, identity->product, identity->version, profilesRoot);
    }

    static EngineView *createCefView(EngineProfile *profile) {
        return new CefEngineView(profile);
    }

    static EngineProfile *createCefProfile(const EngineProfileParameters *parameters, QQmlEngine *, QObject *parent) {
        if (!parameters) {
            return nullptr;
        }
        return new CefProfile(*parameters, parent);
    }

    static void shutdownCef() {
        CefRuntime::instance().shutdown();
    }

}

extern "C" const eden::engine::EnginePluginApi *eden_engine_plugin() {
    static const eden::engine::EnginePluginApi api{
        eden::engine::enginePluginAbiVersion,
        nullptr,
        eden::engine::cef::initializeCef,
        eden::engine::cef::createCefView,
        eden::engine::cef::createCefProfile,
        eden::engine::cef::shutdownCef
    };
    return &api;
}

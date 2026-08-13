#include "engine/engineplugin.h"
#include "engine/qtwebengine/qtwebengineprofile.h"
#include "engine/qtwebengine/qtwebengineview.h"

#include <QtWebEngineQuick/qtwebenginequickglobal.h>

namespace eden::engine {

static bool initializeQtWebEngine(int, char *[]) {
    QtWebEngineQuick::initialize();
    return true;
}

static EngineView *createQtWebEngineView(EngineProfile *profile) {
    return new QtWebEngineView(profile);
}

static EngineProfile *createQtWebEngineProfile(bool privateProfile, QQmlEngine *qmlEngine, QObject *parent) {
    return new QtWebEngineProfile(privateProfile, qmlEngine, parent);
}

static void shutdownQtWebEngine() {}

}

extern "C" const eden::engine::EnginePluginApi *eden_engine_plugin() {
    static const eden::engine::EnginePluginApi api{eden::engine::enginePluginAbiVersion, eden::engine::initializeQtWebEngine,
                                                   eden::engine::createQtWebEngineView, eden::engine::createQtWebEngineProfile,
                                                   eden::engine::shutdownQtWebEngine};
    return &api;
}

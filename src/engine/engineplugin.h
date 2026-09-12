#pragma once

#include "engine/engineprofileparameters.h"

class QObject;
class QQmlEngine;

namespace eden::engine {

    class EngineProfile;
    class EngineView;
    struct EngineIdentity {
        const char *product;
        const char *version;
        const char *engineDataRoot;
    };

    struct EnginePluginApi {
        int abiVersion;
        bool (*prepareApplication)(int argc, char *argv[]);
        bool (*initialize)(int argc, char *argv[], const EngineIdentity *identity);
        EngineView *(*createView)(EngineProfile *profile);
        EngineProfile
            *(*createProfile)(const EngineProfileParameters *parameters, QQmlEngine *qmlEngine, QObject *parent);
        void (*shutdown)();
    };

    using ResolveEnginePlugin = const EnginePluginApi *(*)();

    inline constexpr int enginePluginAbiVersion = 7;

}

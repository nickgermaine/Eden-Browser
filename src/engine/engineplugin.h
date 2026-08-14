#pragma once

class QObject;
class QQmlEngine;

namespace eden::engine {

    class EngineProfile;
    class EngineView;
    struct EngineIdentity {
        const char *product;
        const char *version;
    };

    struct EnginePluginApi {
        int abiVersion;
        bool (*initialize)(int argc, char *argv[], const EngineIdentity *identity);
        EngineView *(*createView)(EngineProfile *profile);
        EngineProfile *(*createProfile)(bool privateProfile, QQmlEngine *qmlEngine, QObject *parent);
        void (*shutdown)();
    };

    using ResolveEnginePlugin = const EnginePluginApi *(*)();

    inline constexpr int enginePluginAbiVersion = 2;

}

#pragma once

#include "engine/enginebackend.h"

#include <memory>

class QQmlEngine;

namespace eden::engine {

class EngineProfile;
class EngineView;

class EngineFactory {
  public:
    using Backend = engine::Backend;

    static void configureApplicationArguments(int argc, char *argv[]);
    static bool initialize(Backend backend);
    static bool initializeCef(int argc, char *argv[]);
    static bool isBackendLoaded(Backend backend);
    static void shutdown();
    static std::unique_ptr<EngineView> create(Backend backend, EngineProfile *profile);
    static std::shared_ptr<EngineProfile> create(Backend backend, bool privateProfile, QQmlEngine *engine);
};

}

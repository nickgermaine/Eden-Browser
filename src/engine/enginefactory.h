#pragma once

#include <memory>

namespace eden::engine {

class EngineProfile;
class EngineView;

class EngineFactory {
  public:
    enum class Backend { QtWebEngine, Cef, Servo };

    static void initialize();
    static std::unique_ptr<EngineView> create(Backend backend, EngineProfile *profile);
};

}

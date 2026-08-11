#include "engine/enginefactory.h"
#include "engine/engineview.h"
#include "engine/qtwebengine/qtwebengineview.h"

#include <stdexcept>

namespace eden::engine {

std::unique_ptr<EngineView> EngineFactory::create(Backend backend, EngineProfile *profile) {
    if (backend == Backend::QtWebEngine) {
        return std::make_unique<QtWebEngineView>(profile);
    }
    throw std::invalid_argument("The selected engine is not available in this build");
}

}

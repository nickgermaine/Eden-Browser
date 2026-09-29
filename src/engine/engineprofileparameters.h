#pragma once

#include "engine/enginebackend.h"

#include <QString>

namespace eden::engine {

    struct EngineProfileParameters {
        QString profileId;
        Backend backend = Backend::Cef;
        bool privateProfile = false;
        QString dataPath;
        QString cachePath;
        bool controlledByAutomation = false;
    };

}

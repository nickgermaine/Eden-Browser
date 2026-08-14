#pragma once

#include "include/internal/cef_types_wrappers.h"

#include <filesystem>
#include <span>
#include <string_view>

namespace eden::engine::cef {

    struct CefProcessSettingsResult {
        CefSettings settings;
        bool valid;
    };

    CefProcessSettingsResult createCefProcessSettings(
        const std::filesystem::path &browserExecutable,
        const char *product,
        const char *version,
        std::span<const std::string_view> arguments,
        const std::filesystem::path &rootCachePath = {}
    );

}

#include "engine/cef/cefprocesssettings.h"

#include "include/internal/cef_string_wrappers.h"

namespace eden::engine::cef {

CefProcessSettingsResult createCefProcessSettings(const std::filesystem::path &browserExecutable,
                                                  std::span<const std::string_view> arguments, const std::filesystem::path &rootCachePath) {
    CefProcessSettingsResult result{};
    if (!rootCachePath.empty() && !rootCachePath.is_absolute()) {
        return result;
    }
    const std::filesystem::path executableDirectory = browserExecutable.parent_path();
    std::error_code error;
    const std::filesystem::path helperPath = std::filesystem::absolute(executableDirectory / "eden-helper", error);
    result.valid = !error && std::filesystem::is_regular_file(helperPath, error) && !error;
    if (!result.valid) {
        return result;
    }

    CefString(&result.settings.browser_subprocess_path) = helperPath.string();
    CefString(&result.settings.resources_dir_path) = executableDirectory.string();
    CefString(&result.settings.locales_dir_path) = (executableDirectory / "locales").string();
    result.settings.multi_threaded_message_loop = true;
    if (!rootCachePath.empty()) {
        CefString(&result.settings.root_cache_path) = rootCachePath.string();
    }
    result.settings.windowless_rendering_enabled = true;
    for (const std::string_view argument : arguments) {
        if (argument == "--engine-compositing=windowed") {
            result.settings.windowless_rendering_enabled = false;
        }
        if (argument == "--no-sandbox") {
            result.settings.no_sandbox = true;
        }
    }
    return result;
}

}

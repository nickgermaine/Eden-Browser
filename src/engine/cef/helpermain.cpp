#include "engine/cef/cefrendererapp.h"

#include "include/cef_app.h"

#include <cstdlib>
#include <string_view>

int main(int argc, char *argv[]) {
    bool x11Ozone = false;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--ozone-platform=x11") {
            x11Ozone = true;
            break;
        }
    }
    if (x11Ozone) {
        setenv("EGL_PLATFORM", "x11", 1);
        unsetenv("WAYLAND_DISPLAY");
    }
    const CefMainArgs mainArguments(argc, argv);
    return CefExecuteProcess(mainArguments, new eden::engine::cef::CefRendererApp(), nullptr);
}

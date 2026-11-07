#pragma once

#include "include/internal/cef_linux.h"

namespace eden::engine::cef {

    void configureAlloyRuntime(CefWindowInfo &windowInfo);
    void applyFontPolicy(CefBrowserSettings &settings);
    double compositedDeviceScale(bool waylandOzone, double windowScale);

}

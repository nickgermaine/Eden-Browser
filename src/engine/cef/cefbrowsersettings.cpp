#include "engine/cef/cefbrowsersettings.h"

#include "engine/fontpolicy.h"

#include "include/internal/cef_string_wrappers.h"

namespace eden::engine::cef {

    void configureAlloyRuntime(CefWindowInfo &windowInfo) {
        windowInfo.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
    }

    void applyFontPolicy(CefBrowserSettings &settings) {
        CefString(&settings.standard_font_family) = FontPolicy::standardFamily().toStdString();
        CefString(&settings.sans_serif_font_family) = FontPolicy::sansSerifFamily().toStdString();
        CefString(&settings.serif_font_family) = FontPolicy::serifFamily().toStdString();
        CefString(&settings.fixed_font_family) = FontPolicy::monospaceFamily().toStdString();
        CefString(&settings.cursive_font_family) = FontPolicy::cursiveFamily().toStdString();
        CefString(&settings.fantasy_font_family) = FontPolicy::fantasyFamily().toStdString();
    }

    double compositedDeviceScale(bool waylandOzone, double windowScale) {
        return waylandOzone || !(windowScale > 0) ? 1.0 : windowScale;
    }

}

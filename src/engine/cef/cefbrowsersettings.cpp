#include "engine/cef/cefbrowsersettings.h"

namespace eden::engine::cef {

void configureAlloyRuntime(CefWindowInfo &windowInfo) {
    windowInfo.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
}

}

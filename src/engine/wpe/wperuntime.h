#pragma once

#include "engine/wpe/wpeapi.h"
#include <QString>
#include <functional>

namespace eden::engine {

    class WpeSurface;

    class WpeRuntime {
      public:
        static bool initialize(const QString &product, const QString &version);
        static void shutdown();
        static void initializeSpellChecking(WebKitWebContext *context);
        static WPEDisplay *display();
        static const QString &userAgentSuffix();
        static WPEView *captureInspector(const std::function<void()> &open);
    };

    void bindWpeSurface(WPEView *view, WpeSurface *surface);

}

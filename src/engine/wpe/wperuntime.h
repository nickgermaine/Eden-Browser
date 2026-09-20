#pragma once

#include "engine/wpe/wpeapi.h"
#include <QString>
#include <QVariantMap>
#include <functional>

class QQuickWindow;

namespace eden::engine {

    class WpeSurface;

    class WpeRuntime {
      public:
        static bool initialize(const QString &product, const QString &version);
        static QVariantMap diagnostics();
        static void shutdown();
        static void initializeSpellChecking(WebKitWebContext *context);
        static WPEDisplay *display();
        static guint64 displayPulseCount();
        static void synchronizeScreen(WPEView *view, QQuickWindow *window, bool visible);
        static const QString &userAgentSuffix();
        static WPEView *captureInspector(const std::function<void()> &open);
    };

    void bindWpeSurface(WPEView *view, WpeSurface *surface);

}

#pragma once

#include "engine/wpe/wpeapi.h"
#include <memory>

class QQuickWindow;

namespace eden::engine {
    class WpeDisplaySyncState;

    class WpeDisplaySync {
      public:
        explicit WpeDisplaySync(WPEDisplay *display);
        ~WpeDisplaySync();
        guint screenCount() const;
        guint64 pulseCount() const;
        WPEScreen *screen(guint index) const;
        void bind(WPEView *view, QQuickWindow *window, bool visible);

      private:
        std::unique_ptr<WpeDisplaySyncState> m_state;
    };
}

#pragma once

#include "include/cef_app.h"
#include "include/cef_render_process_handler.h"

#include <map>

namespace eden::engine::cef {

    class CefRendererApp final : public CefApp, public CefRenderProcessHandler {
      public:
        CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override;
        void OnContextCreated(
            CefRefPtr<CefBrowser> browser,
            CefRefPtr<CefFrame> frame,
            CefRefPtr<CefV8Context> context
        ) override;
        void OnContextReleased(
            CefRefPtr<CefBrowser> browser,
            CefRefPtr<CefFrame> frame,
            CefRefPtr<CefV8Context> context
        ) override;
        bool OnProcessMessageReceived(
            CefRefPtr<CefBrowser> browser,
            CefRefPtr<CefFrame> frame,
            CefProcessId sourceProcess,
            CefRefPtr<CefProcessMessage> message
        ) override;

      private:
        friend class DisplayCaptureRequestHandler;

        bool requestDisplayCapture(
            CefRefPtr<CefV8Context> context,
            bool audioRequested,
            CefRefPtr<CefV8Value> &returnValue,
            CefString &exception
        );

        struct DisplayCapturePromise {
            CefRefPtr<CefV8Context> context;
            CefRefPtr<CefV8Value> promise;
        };

        int m_nextDisplayCaptureRequestId = 1;
        std::map<std::pair<int, int>, DisplayCapturePromise> m_displayCapturePromises;

        IMPLEMENT_REFCOUNTING(CefRendererApp);
    };

}

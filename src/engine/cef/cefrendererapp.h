#pragma once

#include "include/cef_app.h"
#include "include/cef_render_process_handler.h"

namespace eden::engine::cef {

class CefRendererApp final : public CefApp, public CefRenderProcessHandler {
  public:
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override;
    void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override;

  private:
    IMPLEMENT_REFCOUNTING(CefRendererApp);
};

}

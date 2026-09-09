#pragma once

#include "include/cef_app.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace eden::engine::cef {

    class CefApplication final : public CefApp, public CefBrowserProcessHandler {
      public:
        CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override;
        void
        OnBeforeCommandLineProcessing(const CefString &processType, CefRefPtr<CefCommandLine> commandLine) override;
        void OnContextInitialized() override;
        bool waitForContextInitialization(std::chrono::milliseconds timeout);

      private:
        std::mutex m_contextMutex;
        std::condition_variable m_contextCondition;
        bool m_contextInitialized = false;

        IMPLEMENT_REFCOUNTING(CefApplication);
    };

}

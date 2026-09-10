#pragma once

#include "include/cef_request_context.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace eden::engine::cef {

    class CefApplication;

    class CefRuntime final {
      public:
        static CefRuntime &instance();

        int executeProcess(int argc, char *argv[]);
        bool initialize(
            int argc,
            char *argv[],
            const char *product,
            const char *version,
            const std::filesystem::path &rootCachePath = {}
        );
        void shutdown();
        bool registerBrowserClient(const void *client, std::function<void()> requestClose);
        void releaseBrowserClient(const void *client);
        std::size_t browserClientCount() const;
        bool isInitialized() const;
        int exitCode() const;
        const std::filesystem::path &rootCachePath() const;
        CefRefPtr<CefRequestContext>
        createRequestContext(bool privateProfile, const std::filesystem::path &profileDataPath) const;

      private:
        CefRuntime() = default;

        CefRefPtr<CefApplication> m_application;
        std::filesystem::path m_rootCachePath;
        bool m_initialized = false;
        bool m_shuttingDown = false;
        int m_exitCode = 0;
        mutable std::mutex m_browserClientsMutex;
        std::unordered_map<const void *, std::function<void()>> m_browserClients;
        std::function<void()> m_browsersClosed;
        bool m_acceptingBrowserClients = false;
    };

    CefRequestContextSettings
    createCefRequestContextSettings(bool privateProfile, const std::filesystem::path &profileDataPath);
    bool isPathWithinRoot(const std::filesystem::path &candidate, const std::filesystem::path &root);

}

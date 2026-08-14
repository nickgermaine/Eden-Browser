#pragma once

#include "include/cef_request_context.h"

#include <filesystem>

namespace eden::engine::cef {

class CefApplication;

class CefRuntime final {
  public:
    static CefRuntime &instance();

    int executeProcess(int argc, char *argv[]);
    bool initialize(int argc, char *argv[], const char *product, const char *version, const std::filesystem::path &rootCachePath = {});
    void shutdown();
    bool isInitialized() const;
    int exitCode() const;
    const std::filesystem::path &rootCachePath() const;
    CefRefPtr<CefRequestContext> createRequestContext(bool privateProfile) const;

  private:
    CefRuntime() = default;

    CefRefPtr<CefApplication> m_application;
    std::filesystem::path m_rootCachePath;
    bool m_initialized = false;
    int m_exitCode = 0;
};

CefRequestContextSettings createCefRequestContextSettings(bool privateProfile, const std::filesystem::path &rootCachePath);

}

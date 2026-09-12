#include "engine/cef/cefruntime.h"
#include "engine/cef/cefapplication.h"
#include "engine/cef/cefprocesssettings.h"
#include "engine/cef/cefuibridge.h"

#include "include/cef_app.h"
#include "include/internal/cef_string_wrappers.h"

#include <QDir>
#include <QEventLoop>

#include <chrono>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

namespace eden::engine::cef {

    static std::filesystem::path defaultRootCachePath() {
        const QString configuredDataHome = qEnvironmentVariable("XDG_DATA_HOME");
        const std::filesystem::path dataHome =
            configuredDataHome.isEmpty() ? std::filesystem::path(QDir::homePath().toStdString()) / ".local" / "share"
                                         : std::filesystem::path(configuredDataHome.toStdString());
        return dataHome / "eden" / "cef";
    }

    static bool prepareRootCachePath(const std::filesystem::path &rootCachePath) {
        std::error_code error;
        std::filesystem::create_directories(rootCachePath, error);
        if (error) {
            return false;
        }
        std::filesystem::permissions(
            rootCachePath,
            std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace,
            error
        );
        return !error;
    }

    CefRuntime &CefRuntime::instance() {
        static CefRuntime runtime;
        return runtime;
    }

    int CefRuntime::executeProcess(int argc, char *argv[]) {
        if (!m_application) {
            m_application = new CefApplication;
        }
        const CefMainArgs mainArguments(argc, argv);
        return CefExecuteProcess(mainArguments, m_application, nullptr);
    }

    bool CefRuntime::initialize(
        int argc,
        char *argv[],
        const char *product,
        const char *version,
        const std::filesystem::path &rootCachePath
    ) {
        if (m_shuttingDown) {
            return false;
        }
        if (m_initialized) {
            return true;
        }
        m_exitCode = 0;
        CefUiBridge::resumeTasks();
        if (argc < 1 || !argv || !argv[0]) {
            m_exitCode = 1;
            return false;
        }
        m_rootCachePath = rootCachePath.empty() ? defaultRootCachePath() : std::filesystem::absolute(rootCachePath);
        if (!prepareRootCachePath(m_rootCachePath)) {
            m_exitCode = 1;
            return false;
        }
        std::vector<std::string_view> arguments;
        arguments.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
        for (int index = 1; index < argc; ++index) {
            if (argv[index]) {
                arguments.emplace_back(argv[index]);
            }
        }
        std::error_code executableError;
        const std::filesystem::path executablePath = std::filesystem::read_symlink("/proc/self/exe", executableError);
        if (executableError || !executablePath.is_absolute()) {
            m_exitCode = 1;
            return false;
        }
        const CefProcessSettingsResult processSettings = createCefProcessSettings(
            executablePath,
            product,
            version,
            std::span<const std::string_view>(arguments),
            m_rootCachePath
        );
        if (!processSettings.valid) {
            m_exitCode = 1;
            return false;
        }
        if (!m_application) {
            m_application = new CefApplication;
        }
        const CefMainArgs mainArguments(argc, argv);
        if (!CefInitialize(mainArguments, processSettings.settings, m_application, nullptr)) {
            m_exitCode = CefGetExitCode();
            m_application = nullptr;
            return false;
        }
        m_initialized = true;
        {
            const std::lock_guard lock(m_browserClientsMutex);
            m_acceptingBrowserClients = true;
        }
        if (!m_application->waitForContextInitialization(std::chrono::seconds(10))) {
            shutdown();
            m_exitCode = 1;
            return false;
        }
        return true;
    }

    void CefRuntime::shutdown() {
        if (!m_initialized || m_shuttingDown) {
            return;
        }
        m_shuttingDown = true;
        CefUiBridge::beginShutdown();
        QEventLoop closingBrowsers;
        std::vector<std::function<void()>> clients;
        {
            const std::lock_guard lock(m_browserClientsMutex);
            m_acceptingBrowserClients = false;
            clients.reserve(m_browserClients.size());
            for (const auto &[client, requestClose] : m_browserClients) {
                clients.push_back(requestClose);
            }
            if (!clients.empty()) {
                m_browsersClosed = [&closingBrowsers] {
                    QMetaObject::invokeMethod(&closingBrowsers, &QEventLoop::quit, Qt::QueuedConnection);
                };
            }
        }
        if (!clients.empty()) {
            for (const auto &requestClose : clients) {
                requestClose();
            }
            clients.clear();
            closingBrowsers.exec(QEventLoop::ExcludeUserInputEvents);
        }
        CefShutdown();
        CefUiBridge::cancelPendingTasks();
        m_initialized = false;
        m_shuttingDown = false;
        m_application = nullptr;
        m_rootCachePath.clear();
    }

    bool CefRuntime::registerBrowserClient(const void *client, std::function<void()> requestClose) {
        const std::lock_guard lock(m_browserClientsMutex);
        return m_acceptingBrowserClients && m_browserClients.emplace(client, std::move(requestClose)).second;
    }

    void CefRuntime::releaseBrowserClient(const void *client) {
        std::function<void()> releasedClient;
        std::function<void()> browsersClosed;
        {
            const std::lock_guard lock(m_browserClientsMutex);
            const auto found = m_browserClients.find(client);
            if (found == m_browserClients.end()) {
                return;
            }
            releasedClient = std::move(found->second);
            m_browserClients.erase(found);
            if (m_browserClients.empty()) {
                browsersClosed = std::move(m_browsersClosed);
            }
        }
        releasedClient = {};
        if (browsersClosed) {
            browsersClosed();
        }
    }

    std::size_t CefRuntime::browserClientCount() const {
        const std::lock_guard lock(m_browserClientsMutex);
        return m_browserClients.size();
    }

    bool CefRuntime::isInitialized() const {
        return m_initialized && !m_shuttingDown;
    }

    int CefRuntime::exitCode() const {
        return m_exitCode;
    }

    const std::filesystem::path &CefRuntime::rootCachePath() const {
        return m_rootCachePath;
    }

    bool isPathWithinRoot(const std::filesystem::path &candidate, const std::filesystem::path &root) {
        const std::filesystem::path normalizedCandidate = candidate.lexically_normal();
        const std::filesystem::path normalizedRoot = root.lexically_normal();
        auto rootIterator = normalizedRoot.begin();
        for (auto candidateIterator = normalizedCandidate.begin();
             rootIterator != normalizedRoot.end() && candidateIterator != normalizedCandidate.end();
             ++rootIterator, ++candidateIterator) {
            if (*rootIterator != *candidateIterator) {
                return false;
            }
        }
        return rootIterator == normalizedRoot.end() && normalizedCandidate != normalizedRoot;
    }

    CefRefPtr<CefRequestContext>
    CefRuntime::createRequestContext(bool privateProfile, const std::filesystem::path &profileDataPath) const {
        if (!isInitialized()) {
            throw std::logic_error("CEF is not initialized");
        }
        if (!privateProfile) {
            if (profileDataPath.empty() || !isPathWithinRoot(profileDataPath, m_rootCachePath)) {
                throw std::invalid_argument("The CEF profile path is outside the engine root");
            }
            if (!prepareRootCachePath(profileDataPath)) {
                throw std::runtime_error("CEF profile directory creation failed");
            }
        }
        const CefRequestContextSettings settings = createCefRequestContextSettings(privateProfile, profileDataPath);
        CefRefPtr<CefRequestContext> context = CefRequestContext::CreateContext(settings, nullptr);
        if (!context) {
            throw std::runtime_error("CEF request context creation failed");
        }
        return context;
    }

    CefRequestContextSettings
    createCefRequestContextSettings(bool privateProfile, const std::filesystem::path &profileDataPath) {
        CefRequestContextSettings settings;
        if (!privateProfile) {
            CefString(&settings.cache_path) = profileDataPath.string();
            settings.persist_session_cookies = true;
        }
        return settings;
    }

}

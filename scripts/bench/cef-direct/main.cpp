#include <charconv>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_browser_view_delegate.h"
#include "include/views/cef_fill_layout.h"
#include "include/views/cef_window.h"
#include "include/views/cef_window_delegate.h"
#include "include/wrapper/cef_helpers.h"

namespace {

    constexpr std::string_view defaultUrl = "https://browserbench.org/Speedometer3.1/";
    constexpr int defaultWindowWidth = 1280;
    constexpr int defaultWindowHeight = 900;

    struct DirectOptions {
        std::string url{defaultUrl};
        std::filesystem::path cachePath;
        int windowWidth{defaultWindowWidth};
        int windowHeight{defaultWindowHeight};
    };

    std::optional<std::string> switchValue(int argumentCount, char *argumentValues[], std::string_view switchName) {
        const std::string prefix = "--" + std::string(switchName) + "=";
        const std::string separate = "--" + std::string(switchName);
        for (int index = 1; index < argumentCount; ++index) {
            const std::string_view argument{argumentValues[index]};
            if (argument.starts_with(prefix)) {
                return std::string(argument.substr(prefix.size()));
            }
            if (argument == separate && index + 1 < argumentCount) {
                return std::string(argumentValues[index + 1]);
            }
        }
        return std::nullopt;
    }

    bool parsePositiveInteger(std::string_view text, int &value) {
        int parsed = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (error != std::errc{} || end != text.data() + text.size() || parsed <= 0) {
            return false;
        }
        value = parsed;
        return true;
    }

    bool parseWindowSize(std::string_view text, int &width, int &height) {
        const std::size_t separator = text.find('x');
        if (separator == std::string_view::npos) {
            return false;
        }
        int parsedWidth = 0;
        int parsedHeight = 0;
        if (!parsePositiveInteger(text.substr(0, separator), parsedWidth) ||
            !parsePositiveInteger(text.substr(separator + 1), parsedHeight)) {
            return false;
        }
        width = parsedWidth;
        height = parsedHeight;
        return true;
    }

    std::filesystem::path executablePath(char *argumentValues[]) {
        std::error_code error;
        std::filesystem::path path = std::filesystem::canonical("/proc/self/exe", error);
        if (!error) {
            return path;
        }
        path = std::filesystem::absolute(argumentValues[0], error);
        if (!error) {
            return path;
        }
        return argumentValues[0];
    }

    std::optional<DirectOptions>
    parseOptions(int argumentCount, char *argumentValues[], const std::filesystem::path &executable) {
        DirectOptions options;
        if (const auto url = switchValue(argumentCount, argumentValues, "url")) {
            options.url = *url;
        }

        std::error_code error;
        if (const auto cachePath = switchValue(argumentCount, argumentValues, "cache-path")) {
            options.cachePath = std::filesystem::absolute(*cachePath, error);
        } else {
            options.cachePath = std::filesystem::temp_directory_path(error) / "eden-cef-direct-profile";
        }
        if (error || options.cachePath.empty() || !options.cachePath.is_absolute()) {
            std::cerr << "A valid absolute cache path is required\n";
            return std::nullopt;
        }

        if (const auto windowSize = switchValue(argumentCount, argumentValues, "window-size")) {
            if (!parseWindowSize(*windowSize, options.windowWidth, options.windowHeight)) {
                std::cerr << "Window size must use WIDTHxHEIGHT with positive integers\n";
                return std::nullopt;
            }
        }

        if (!std::filesystem::exists(executable.parent_path() / "libcef.so")) {
            std::cerr << "libcef.so must be beside the executable\n";
            return std::nullopt;
        }
        return options;
    }

    void
    appendListSwitch(CefRefPtr<CefCommandLine> commandLine, const std::string &switchName, const std::string &value) {
        std::string values = commandLine->GetSwitchValue(switchName);
        std::size_t offset = 0;
        while (offset <= values.size()) {
            const std::size_t separator = values.find(',', offset);
            const std::size_t length = separator == std::string::npos ? values.size() - offset : separator - offset;
            if (values.compare(offset, length, value) == 0) {
                return;
            }
            if (separator == std::string::npos) {
                break;
            }
            offset = separator + 1;
        }
        if (!values.empty()) {
            values += ',';
        }
        values += value;
        commandLine->AppendSwitchWithValue(switchName, values);
    }

    class DirectClient final : public CefClient, public CefLifeSpanHandler {
      public:
        CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
            return this;
        }

        void OnAfterCreated(CefRefPtr<CefBrowser>) override {
            CEF_REQUIRE_UI_THREAD();
            ++browserCount_;
        }

        void OnBeforeClose(CefRefPtr<CefBrowser>) override {
            CEF_REQUIRE_UI_THREAD();
            --browserCount_;
            if (browserCount_ == 0) {
                CefQuitMessageLoop();
            }
        }

      private:
        int browserCount_{0};

        IMPLEMENT_REFCOUNTING(DirectClient);
    };

    class DirectBrowserViewDelegate final : public CefBrowserViewDelegate {
      public:
        cef_runtime_style_t GetBrowserRuntimeStyle() override {
            return CEF_RUNTIME_STYLE_ALLOY;
        }

      private:
        IMPLEMENT_REFCOUNTING(DirectBrowserViewDelegate);
    };

    class DirectWindowDelegate final : public CefWindowDelegate {
      public:
        DirectWindowDelegate(CefRefPtr<CefBrowserView> browserView, int width, int height)
            : browserView_(browserView),
              width_(width),
              height_(height) {}

        void OnWindowCreated(CefRefPtr<CefWindow> window) override {
            CEF_REQUIRE_UI_THREAD();
            window->SetToFillLayout();
            window->AddChildView(browserView_);
            window->SetTitle("Eden CEF Direct");
            window->Show();
            browserView_->RequestFocus();
        }

        void OnWindowDestroyed(CefRefPtr<CefWindow>) override {
            CEF_REQUIRE_UI_THREAD();
            browserView_ = nullptr;
        }

        CefRect GetInitialBounds(CefRefPtr<CefWindow>) override {
            return CefRect(100, 100, width_, height_);
        }

        bool CanClose(CefRefPtr<CefWindow>) override {
            CEF_REQUIRE_UI_THREAD();
            if (!browserView_) {
                return true;
            }
            const CefRefPtr<CefBrowser> browser = browserView_->GetBrowser();
            return !browser || browser->GetHost()->TryCloseBrowser();
        }

        cef_runtime_style_t GetWindowRuntimeStyle() override {
            return CEF_RUNTIME_STYLE_ALLOY;
        }

      private:
        CefRefPtr<CefBrowserView> browserView_;
        const int width_;
        const int height_;

        IMPLEMENT_REFCOUNTING(DirectWindowDelegate);
    };

    class DirectApplication final : public CefApp, public CefBrowserProcessHandler {
      public:
        explicit DirectApplication(DirectOptions options)
            : options_(std::move(options)) {}

        CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
            return this;
        }

        void OnBeforeCommandLineProcessing(const CefString &, CefRefPtr<CefCommandLine> commandLine) override {
            if (!commandLine->HasSwitch("ozone-platform")) {
                commandLine->AppendSwitchWithValue("ozone-platform", "x11");
            }
            appendListSwitch(commandLine, "disable-features", "ImmersiveReadAnything");
        }

        void OnContextInitialized() override {
            CEF_REQUIRE_UI_THREAD();
            CefRefPtr<DirectClient> client = new DirectClient();
            CefRefPtr<DirectBrowserViewDelegate> browserViewDelegate = new DirectBrowserViewDelegate();
            CefBrowserSettings settings;
            CefRefPtr<CefBrowserView> browserView = CefBrowserView::CreateBrowserView(
                client,
                options_.url,
                settings,
                nullptr,
                nullptr,
                browserViewDelegate
            );
            CefWindow::CreateTopLevelWindow(
                new DirectWindowDelegate(browserView, options_.windowWidth, options_.windowHeight)
            );
        }

      private:
        const DirectOptions options_;

        IMPLEMENT_REFCOUNTING(DirectApplication);
    };

}

int main(int argumentCount, char *argumentValues[]) {
    const std::filesystem::path executable = executablePath(argumentValues);
    const std::optional<DirectOptions> options = parseOptions(argumentCount, argumentValues, executable);
    if (!options) {
        return 2;
    }

    CefMainArgs mainArguments(argumentCount, argumentValues);
    CefRefPtr<DirectApplication> application = new DirectApplication(*options);
    const int subprocessExitCode = CefExecuteProcess(mainArguments, application, nullptr);
    if (subprocessExitCode >= 0) {
        return subprocessExitCode;
    }

    std::error_code error;
    std::filesystem::create_directories(options->cachePath, error);
    if (error) {
        std::cerr << "Failed to create cache directory: " << error.message() << '\n';
        return 2;
    }

    const std::filesystem::path executableDirectory = executable.parent_path();
    CefSettings settings;
    CefString(&settings.browser_subprocess_path) = executable.string();
    CefString(&settings.resources_dir_path) = executableDirectory.string();
    CefString(&settings.locales_dir_path) = (executableDirectory / "locales").string();
    CefString(&settings.root_cache_path) = options->cachePath.string();
    CefString(&settings.cache_path) = options->cachePath.string();
    settings.windowless_rendering_enabled = false;
    settings.log_severity = LOGSEVERITY_WARNING;

    if (!CefInitialize(mainArguments, settings, application, nullptr)) {
        std::cerr << "CEF initialization failed\n";
        return 3;
    }

    CefRunMessageLoop();
    application = nullptr;
    CefShutdown();
    return 0;
}

#include "servo_capi_minimal.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {
    using Clock = std::chrono::steady_clock;

    constexpr std::int32_t loadStatusComplete = 2;
    constexpr std::uint32_t viewportWidth = 1280;
    constexpr std::uint32_t viewportHeight = 900;
    constexpr std::string_view defaultUrl = "https://browserbench.org/Speedometer3.1/";
    constexpr std::string_view benchmarkScript =
        R"JS(javascript:(()=>{const b=document.querySelector('.start-tests-button');if(!b)return;const p=()=>{if(window.benchmarkClient&&benchmarkClient._hasResults){const r=benchmarkClient._computeResults(benchmarkClient._measuredValuesList,'score');const m=r.formattedMean||String(r.mean);document.documentElement.innerHTML='<head><style>html,body{margin:0;width:100%;height:100%;background:rgb(1,2,3);color:white;font:700 96px sans-serif}body{display:grid;place-items:center}small{display:block;font-size:24px;text-align:center}</style></head><body><main>'+m+'<small>Servo 0.4.0 Speedometer 3.1</small></main></body>';return}setTimeout(p,250)};b.click();p()})())JS";

    std::atomic_bool eventLoopWoken = true;
    std::condition_variable eventLoopCondition;
    std::mutex eventLoopMutex;

    struct ApplicationState {
        bool scriptInjected = false;
        bool screenshotPending = false;
        bool completed = false;
        std::string outputPath = "servo-speedometer-result.ppm";
        Clock::time_point nextScreenshot = Clock::now() + std::chrono::seconds(20);
    };

    bool hasCompletionSentinel(const std::uint8_t *data, std::uint32_t width, std::uint32_t height) {
        if (!data || width == 0 || height == 0) {
            return false;
        }
        const auto matches = [data](std::size_t pixel) {
            const std::size_t offset = pixel * 4;
            return data[offset] == 1 && data[offset + 1] == 2 && data[offset + 2] == 3;
        };
        return matches(0) && matches(width - 1) && matches(static_cast<std::size_t>(width) * (height - 1));
    }

    bool writePpm(const std::string &path, const std::uint8_t *data, std::uint32_t width, std::uint32_t height) {
        std::ofstream output(path, std::ios::binary);
        if (!output) {
            return false;
        }
        output << "P6\n" << width << ' ' << height << "\n255\n";
        std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
        for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(width) * height; ++pixel) {
            rgb[pixel * 3] = data[pixel * 4];
            rgb[pixel * 3 + 1] = data[pixel * 4 + 1];
            rgb[pixel * 3 + 2] = data[pixel * 4 + 2];
        }
        output.write(reinterpret_cast<const char *>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
        return output.good();
    }

    extern "C" void wakeEventLoop() {
        eventLoopWoken.store(true, std::memory_order_release);
        eventLoopCondition.notify_one();
    }

    extern "C" void screenshotReady(
        const std::uint8_t *data,
        std::uint32_t width,
        std::uint32_t height,
        std::int32_t error,
        void *userData
    ) {
        auto &state = *static_cast<ApplicationState *>(userData);
        state.screenshotPending = false;
        state.nextScreenshot = Clock::now() + std::chrono::seconds(3);
        if (error != 0 || !hasCompletionSentinel(data, width, height)) {
            return;
        }
        if (!writePpm(state.outputPath, data, width, height)) {
            std::cerr << "Unable to write " << state.outputPath << '\n';
            return;
        }
        state.completed = true;
        std::cout << "Saved completed Speedometer result to " << state.outputPath << '\n';
    }

    extern "C" void loadStatusChanged(WebView *webview, std::int32_t status, void *userData) {
        auto &state = *static_cast<ApplicationState *>(userData);
        if (status != loadStatusComplete || state.scriptInjected) {
            return;
        }
        state.scriptInjected = true;
        const std::string script(benchmarkScript);
        if (servo_webview_load(webview, script.c_str()) != 0) {
            std::cerr << "Servo rejected the Speedometer start script\n";
        }
    }

    extern "C" void newFrameReady(WebView *webview, void *userData) {
        auto &state = *static_cast<ApplicationState *>(userData);
        servo_webview_paint(webview);
        if (!state.scriptInjected || state.screenshotPending || Clock::now() < state.nextScreenshot) {
            return;
        }
        state.screenshotPending = true;
        servo_webview_take_screenshot(webview, screenshotReady, &state);
    }

    struct Options {
        std::string url = std::string(defaultUrl);
        std::string outputPath = "servo-speedometer-result.ppm";
        int timeoutSeconds = 600;
    };

    Options parseOptions(int argc, char *argv[]) {
        Options options;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            if (argument.starts_with("--url=")) {
                options.url = argument.substr(6);
            } else if (argument.starts_with("--output=")) {
                options.outputPath = argument.substr(9);
            } else if (argument.starts_with("--timeout-seconds=")) {
                options.timeoutSeconds = std::stoi(std::string(argument.substr(18)));
            }
        }
        return options;
    }
}

int main(int argc, char *argv[]) {
    const Options options = parseOptions(argc, argv);
    ServoBuilder *servoBuilder = servo_builder_create();
    servo_builder_set_event_loop_waker(servoBuilder, ServoEventLoopWaker{wakeEventLoop});
    Servo *servo = servo_builder_build(servoBuilder);
    if (!servo) {
        std::cerr << "Unable to initialize Servo\n";
        return 1;
    }
    servo_setup_logging(servo);

    RenderingContext *renderingContext = servo_rendering_context_create_software(viewportWidth, viewportHeight);
    if (!renderingContext) {
        std::cerr << "Unable to create Servo software rendering context\n";
        servo_free(servo);
        return 1;
    }

    ApplicationState state;
    state.outputPath = options.outputPath;
    ServoWebViewBuilder *webviewBuilder = servo_webview_builder_create(servo, renderingContext);
    servo_webview_builder_set_delegate(webviewBuilder, ServoWebViewDelegate{&state, loadStatusChanged, newFrameReady});
    if (servo_webview_builder_set_url(webviewBuilder, options.url.c_str()) != 0) {
        std::cerr << "Servo rejected URL " << options.url << '\n';
        servo_free(servo);
        return 1;
    }
    WebView *webview = servo_webview_builder_build(webviewBuilder);
    if (!webview) {
        std::cerr << "Unable to create Servo WebView\n";
        servo_free(servo);
        return 1;
    }

    const auto deadline = Clock::now() + std::chrono::seconds(options.timeoutSeconds);
    while (!state.completed && Clock::now() < deadline) {
        servo_spin_event_loop(servo);
        if (!eventLoopWoken.exchange(false, std::memory_order_acq_rel)) {
            std::unique_lock lock(eventLoopMutex);
            eventLoopCondition.wait_for(lock, std::chrono::milliseconds(10));
        }
    }

    servo_webview_free(webview);
    servo_free(servo);
    if (!state.completed) {
        std::cerr << "Servo Speedometer run timed out\n";
        return 2;
    }
    return 0;
}

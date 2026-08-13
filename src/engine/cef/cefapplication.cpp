#include "engine/cef/cefapplication.h"

#include <QGuiApplication>
#include <QScreen>

#include <string>

namespace {

void appendListSwitch(CefRefPtr<CefCommandLine> commandLine, const std::string &switchName, const std::string &feature) {
    std::string disabled = commandLine->GetSwitchValue(switchName);
    std::size_t offset = 0;
    bool present = false;
    while (offset <= disabled.size()) {
        const std::size_t separator = disabled.find(',', offset);
        const std::size_t length = separator == std::string::npos ? disabled.size() - offset : separator - offset;
        if (disabled.compare(offset, length, feature) == 0) {
            present = true;
            break;
        }
        if (separator == std::string::npos) {
            break;
        }
        offset = separator + 1;
    }
    if (!present) {
        if (!disabled.empty()) {
            disabled += ',';
        }
        disabled += feature;
        commandLine->AppendSwitchWithValue(switchName, disabled);
    }
}

void disableFeature(CefRefPtr<CefCommandLine> commandLine, const std::string &feature) {
    appendListSwitch(commandLine, "disable-features", feature);
}

}

namespace eden::engine::cef {

CefRefPtr<CefBrowserProcessHandler> CefApplication::GetBrowserProcessHandler() {
    return this;
}

void CefApplication::OnBeforeCommandLineProcessing(const CefString &processType, CefRefPtr<CefCommandLine> commandLine) {
    const bool osr = commandLine->GetSwitchValue("engine-compositing") != "windowed";
    const bool sharedTextureProbe = qEnvironmentVariableIntValue("EDEN_CEF_SHARED_TEXTURE_PROBE") == 1;
    const bool wayland = qEnvironmentVariable("QT_QPA_PLATFORM") == "wayland";
    commandLine->AppendSwitchWithValue("ozone-platform", osr && wayland && !sharedTextureProbe ? "wayland" : "x11");
    if (osr && sharedTextureProbe) {
        commandLine->AppendSwitchWithValue("use-angle", "gl-egl");
    }
    disableFeature(commandLine, "ImmersiveReadAnything");
    appendListSwitch(commandLine, "disable-blink-features", "FileSystemAccessLocal");
    if (processType.empty() && !commandLine->HasSwitch("force-device-scale-factor") && QGuiApplication::instance()) {
        QScreen *screen = QGuiApplication::primaryScreen();
        if (!screen) {
            return;
        }
        commandLine->AppendSwitchWithValue("force-device-scale-factor", QString::number(screen->devicePixelRatio()).toStdString());
    }
}

void CefApplication::OnContextInitialized() {
    {
        const std::lock_guard lock(m_contextMutex);
        m_contextInitialized = true;
    }
    m_contextCondition.notify_all();
}

bool CefApplication::waitForContextInitialization(std::chrono::milliseconds timeout) {
    std::unique_lock lock(m_contextMutex);
    return m_contextCondition.wait_for(lock, timeout, [this] { return m_contextInitialized; });
}

}

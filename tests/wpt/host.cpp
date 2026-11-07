#include "engine/wpe/wpeengineview.h"
#include "engine/wpe/wpeprofile.h"
#include "engine/wpe/wperuntime.h"
#include "engine/wpe/wpesurface.h"
#include "platform/waylandglibeventdispatcher.h"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <memory>
#include <vector>
#include <wpe/webkit.h>

using namespace eden::engine;

namespace {

    struct HostedView {
        std::unique_ptr<QQuickWindow> window;
        std::unique_ptr<QQuickItem> viewport;
        std::unique_ptr<WpeEngineView> view;
    };

    class Host final : public QObject {
      public:
        Host(WpeProfile &profile, QSize viewport, bool automation, QByteArray browserName)
            : m_profile(profile),
              m_viewport(viewport),
              m_automation(automation),
              m_browserName(std::move(browserName)) {
            if (automation) {
                g_signal_connect(
                    profile.context(),
                    "automation-started",
                    G_CALLBACK(+[](WebKitWebContext *, WebKitAutomationSession *session, Host *self) {
                        self->startAutomation(session);
                    }),
                    this
                );
            }
        }

        ~Host() override {
            g_signal_handlers_disconnect_by_data(m_profile.context(), this);
            for (auto *session : m_sessions) {
                g_signal_handlers_disconnect_by_data(session, this);
                g_object_unref(session);
            }
            while (!m_views.empty()) {
                release(m_views.back()->view.get());
            }
        }

        void open(const QUrl &url) {
            auto &hosted = add();
            hosted.view->load(url);
        }

      private:
        void startAutomation(WebKitAutomationSession *session) {
            auto *info = webkit_application_info_new();
            webkit_application_info_set_name(info, m_browserName.constData());
            webkit_application_info_set_version(info, WEBKIT_MAJOR_VERSION, WEBKIT_MINOR_VERSION, WEBKIT_MICRO_VERSION);
            webkit_automation_session_set_application_info(session, info);
            webkit_application_info_unref(info);
            m_sessions.push_back(WEBKIT_AUTOMATION_SESSION(g_object_ref(session)));
            g_signal_connect(
                session,
                "create-web-view",
                G_CALLBACK(+[](WebKitAutomationSession *, Host *self) -> WebKitWebView * {
                    auto &hosted = self->add();
                    hosted.view->load(QUrl(QStringLiteral("about:blank")));
                    return hosted.view->nativeView();
                }),
                this
            );
        }

        HostedView &add() {
            auto hosted = std::make_unique<HostedView>();
            hosted->view = std::make_unique<WpeEngineView>(&m_profile);
            auto *view = hosted->view.get();
            configureNative(view);
            connect(view, &EngineView::permissionRequested, view, [view](const PermissionRequestInfo &info) {
                view->resolvePermissionRequest(info.id, true);
            });
            connect(view, &EngineView::displayCaptureRequested, view, [view](const DisplayCaptureRequestInfo &info) {
                view->resolveDisplayCaptureRequest(info.id, QStringLiteral("screen"));
            });
            connect(view, &EngineView::javaScriptDialogRequested, view, [view](const JavaScriptDialogInfo &info) {
                const bool leave = info.kind == QStringLiteral("alert") || info.kind == QStringLiteral("beforeunload");
                const QString message = info.message.simplified();
                std::fprintf(stderr, "eden-wpt-host dialog %s: %s\n", qPrintable(info.kind), qPrintable(message));
                std::fflush(stderr);
                view->resolveJavaScriptDialog(info.id, leave, {});
            });
            connect(view, &EngineView::fileDialogRequested, view, [view](const FileDialogInfo &info) {
                view->resolveFileDialog(info.id, false, {});
            });
            connect(view, &EngineView::newViewRequested, this, [this](EngineNewViewRequest *request) {
                adopt(request);
            });
            connect(view, &EngineView::closeRequested, this, [this, view] { release(view); });
            hosted->window = std::make_unique<QQuickWindow>();
            hosted->window->resize(m_viewport);
            hosted->viewport = std::make_unique<QQuickItem>(hosted->window->contentItem());
            hosted->viewport->setSize(QSizeF(m_viewport));
            view->attach(hosted->viewport.get());
            hosted->window->show();
            hosted->window->requestActivate();
            if (view->surface()) {
                view->surface()->forceActiveFocus(Qt::OtherFocusReason);
            }
            m_views.push_back(std::move(hosted));
            return *m_views.back();
        }

        void adopt(EngineNewViewRequest *request) {
            auto &hosted = add();
            if (!request->openIn(hosted.view.get())) {
                qWarning("A popup could not be adopted");
                release(hosted.view.get());
                return;
            }
            configureNative(hosted.view.get());
        }

        void configureNative(WpeEngineView *view) {
            auto *settings = webkit_web_view_get_settings(view->nativeView());
            webkit_settings_set_enable_mock_capture_devices(settings, TRUE);
            webkit_settings_set_media_playback_requires_user_gesture(settings, FALSE);
            webkit_settings_set_javascript_can_open_windows_automatically(settings, m_automation);
            g_signal_connect(
                view->nativeView(),
                "web-process-terminated",
                G_CALLBACK(+[](WebKitWebView *, WebKitWebProcessTerminationReason reason, gpointer) {
                    qWarning("A web process terminated: %d", int(reason));
                }),
                nullptr
            );
            g_signal_connect(
                view->nativeView(),
                "close",
                G_CALLBACK(+[](WebKitWebView *native, Host *self) { self->releaseNative(native); }),
                this
            );
        }

        void releaseNative(WebKitWebView *native) {
            const auto found = std::find_if(m_views.begin(), m_views.end(), [native](const auto &hosted) {
                return hosted->view->nativeView() == native;
            });
            if (found != m_views.end()) {
                release((*found)->view.get());
            }
        }

        void release(WpeEngineView *view) {
            const auto found = std::find_if(m_views.begin(), m_views.end(), [view](const auto &hosted) {
                return hosted->view.get() == view;
            });
            if (found == m_views.end()) {
                return;
            }
            auto hosted = std::move(*found);
            m_views.erase(found);
            if (auto *native = hosted->view->nativeView()) {
                g_signal_handlers_disconnect_by_data(native, this);
            }
            hosted->view->attach(nullptr);
            hosted->view.reset();
            hosted->viewport.reset();
            hosted->window.reset();
            if (m_views.empty() && !m_automation) {
                QCoreApplication::quit();
            }
        }

        WpeProfile &m_profile;
        const QSize m_viewport;
        const bool m_automation;
        const QByteArray m_browserName;
        std::vector<std::unique_ptr<HostedView>> m_views;
        std::vector<WebKitAutomationSession *> m_sessions;
    };

}

int main(int argc, char *argv[]) {
    qputenv("QT_NO_GLIB", "1");
    qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QCoreApplication::setApplicationName("EdenWebPlatformTests");
    QGuiApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"automation", "Accept WebDriver sessions and create their windows on demand"});
    parser.addOption({"url", "First page when not driven by WebDriver", "url", "about:blank"});
    parser.addOption({"browser-name", "Browser name reported to WebDriver", "name", "Eden"});
    parser.addOption({"viewport-width", "Page viewport width", "pixels", "800"});
    parser.addOption({"viewport-height", "Page viewport height", "pixels", "600"});
    parser.process(application);
    const int viewportWidth = parser.value("viewport-width").toInt();
    const int viewportHeight = parser.value("viewport-height").toInt();
    if (viewportWidth < 1 || viewportWidth > 8192 || viewportHeight < 1 || viewportHeight > 8192) {
        parser.showHelp(2);
    }
    const bool automation = parser.isSet("automation");
    if (automation && parser.isSet("url")) {
        qCritical("--url is not used when WebDriver drives the host");
        return 2;
    }
    if (parser.value("browser-name").trimmed().isEmpty()) {
        qCritical("--browser-name needs a name");
        return 2;
    }
    std::signal(SIGTERM, [](int) { std::_Exit(0); });
    eden::platform::WaylandGlibEventDispatcher dispatcher;
    if (!WpeRuntime::initialize("Eden", "web-platform-tests")) {
        return 2;
    }
    int result = 0;
    {
        EngineProfileParameters parameters;
        parameters.backend = Backend::Wpe;
        parameters.profileId = "web-platform-tests";
        parameters.privateProfile = true;
        parameters.controlledByAutomation = automation;
        WpeProfile profile(parameters);
        if (automation && !profile.controlledByAutomation()) {
            qCritical("WebKit automation is unavailable in this runtime");
            return 2;
        }
        Host host(profile, QSize(viewportWidth, viewportHeight), automation, parser.value("browser-name").toUtf8());
        if (!automation) {
            host.open(QUrl(parser.value("url")));
        }
        result = application.exec();
    }
    WpeRuntime::shutdown();
    return result;
}

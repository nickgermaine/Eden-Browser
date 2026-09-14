#include "engine/wpe/wpeengineview.h"
#include "engine/wpe/wpeprofile.h"
#include "engine/wpe/wperuntime.h"
#include "engine/wpe/wpesurface.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSet>
#include <QSslCertificate>
#include <QTimer>
#include <QUuid>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <utility>

namespace eden::engine {

    class WpeNewViewRequest final : public EngineNewViewRequest {
      public:
        WpeNewViewRequest(WebKitWebView *source, WpeProfile *profile, WebKitNavigationAction *action)
            : EngineNewViewRequest(
                  QUrl(QString::fromUtf8(webkit_uri_request_get_uri(webkit_navigation_action_get_request(action)))),
                  dispositionFor(action),
                  webkit_navigation_action_is_user_gesture(action)
              ),
              m_source(source),
              m_profile(profile) {}
        bool openIn(EngineView *target) override {
            auto *view = qobject_cast<WpeEngineView *>(target);
            if (m_target || !view || !view->adoptPopup(m_source, m_profile)) {
                return false;
            }
            m_target = view;
            return true;
        }
        WebKitWebView *adoptedView() const {
            return m_target ? m_target->nativeView() : nullptr;
        }

      private:
        static Disposition dispositionFor(WebKitNavigationAction *action) {
            const unsigned modifiers = webkit_navigation_action_get_modifiers(action);
            const bool tab = webkit_navigation_action_get_mouse_button(action) == 2 ||
                             (modifiers & (WPE_MODIFIER_KEYBOARD_CONTROL | WPE_MODIFIER_KEYBOARD_META));
            if (modifiers & WPE_MODIFIER_KEYBOARD_SHIFT) {
                return tab ? Disposition::NewForegroundTab : Disposition::NewWindow;
            }
            return tab ? Disposition::NewBackgroundTab : Disposition::NewForegroundTab;
        }

        WebKitWebView *m_source;
        WpeProfile *m_profile;
        QPointer<WpeEngineView> m_target;
    };

    WpeEngineView::WpeEngineView(WpeProfile *profile, QObject *parent)
        : EngineView(parent),
          m_profile(profile) {
        createNative();
    }

    WpeEngineView::~WpeEngineView() {
        destroyNative();
    }

    void WpeEngineView::createNative(WebKitWebView *related) {
        m_cancellable = g_cancellable_new();
        m_content = webkit_user_content_manager_new();
        webkit_user_content_manager_register_script_message_handler(m_content, "forms", "eden.forms");
        g_signal_connect(
            m_content,
            "script-message-received::forms",
            G_CALLBACK(+[](WebKitUserContentManager *, JSCValue *message, WpeEngineView *self) {
                self->handleFormMessage(message);
            }),
            this
        );
        installFormHooks();
        auto *settings = webkit_settings_new();
        webkit_settings_set_enable_developer_extras(settings, TRUE);
        webkit_settings_set_enable_fullscreen(settings, TRUE);
        webkit_settings_set_enable_page_cache(settings, TRUE);
        webkit_settings_set_enable_smooth_scrolling(settings, TRUE);
        webkit_settings_set_user_agent_with_application_details(
            settings,
            "Eden",
            WpeRuntime::userAgentSuffix().section('/', 1).toUtf8().constData()
        );
        if (related) {
            m_view = WEBKIT_WEB_VIEW(g_object_new(
                WEBKIT_TYPE_WEB_VIEW,
                "related-view",
                related,
                "user-content-manager",
                m_content,
                "settings",
                settings,
                nullptr
            ));
        } else {
            m_view = WEBKIT_WEB_VIEW(g_object_new(
                WEBKIT_TYPE_WEB_VIEW,
                "display",
                WpeRuntime::display(),
                "network-session",
                m_profile->session(),
                "web-context",
                m_profile->context(),
                "user-content-manager",
                m_content,
                "settings",
                settings,
                nullptr
            ));
        }
        g_object_unref(settings);
        if (g_signal_lookup("eden-favicon-v1", WEBKIT_TYPE_WEB_VIEW)) {
            g_signal_connect(
                m_view,
                "eden-favicon-v1",
                G_CALLBACK(+[](WebKitWebView *, GBytes *bytes, WpeEngineView *self) { self->updateFavicon(bytes); }),
                this
            );
        }
        auto *nativeSurface = webkit_web_view_get_wpe_view(m_view);
        if (!wpe_view_get_toplevel(nativeSurface)) {
            auto *toplevel = wpe_display_create_toplevel(WpeRuntime::display(), 1);
            wpe_view_set_toplevel(nativeSurface, toplevel);
            g_object_unref(toplevel);
        }
        m_surface = new WpeSurface(nativeSurface);
        m_surface->setParent(this);
        if (g_signal_lookup("eden-focus-v1", WEBKIT_TYPE_WEB_VIEW)) {
            g_signal_connect(
                m_view,
                "eden-focus-v1",
                G_CALLBACK(+[](WebKitWebView *, int direction, WpeEngineView *self) {
                    if (!self->m_surface || !self->m_surface->isVisible()) {
                        return;
                    }
                    if (direction) {
                        emit self->focusTraversalRequested(direction > 0);
                    } else {
                        self->m_surface->forceActiveFocus(Qt::OtherFocusReason);
                    }
                }),
                this
            );
        }
        connect(m_surface, &WpeSurface::shortcutRequested, this, &EngineView::shortcutRequested);
        connect(m_surface, &WpeSurface::renderingFailed, this, [this](const QString &message) {
            qWarning().noquote() << message;
            if (m_surface && !m_surface->accelerated()) {
                m_surface->setProperty("renderingDiagnostic", message);
            }
        });
        g_signal_connect(
            m_view,
            "notify",
            G_CALLBACK(+[](GObject *, GParamSpec *, WpeEngineView *self) { self->synchronizeState(); }),
            this
        );
        g_signal_connect(
            webkit_web_view_get_back_forward_list(m_view),
            "changed",
            G_CALLBACK(+[](WebKitBackForwardList *, WebKitBackForwardListItem *, GList *, WpeEngineView *self) {
                self->synchronizeState();
            }),
            this
        );
        g_signal_connect(
            m_view,
            "load-changed",
            G_CALLBACK(+[](WebKitWebView *, WebKitLoadEvent event, WpeEngineView *self) {
                self->navigationChanged(event);
            }),
            this
        );
        g_signal_connect(
            m_view,
            "load-failed",
            G_CALLBACK(
                +[](WebKitWebView *, WebKitLoadEvent, const char *uri, GError *error, WpeEngineView *self) -> gboolean {
                    if (!g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED) &&
                        !g_error_matches(
                            error,
                            WEBKIT_POLICY_ERROR,
                            WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE
                        )) {
                        self->showError(QUrl(QString::fromUtf8(uri)), QString::fromUtf8(error->message));
                    }
                    return TRUE;
                }
            ),
            this
        );
        g_signal_connect(
            m_view,
            "load-failed-with-tls-errors",
            G_CALLBACK(
                +[](WebKitWebView *, const char *uri, GTlsCertificate *, GTlsCertificateFlags, WpeEngineView *self)
                    -> gboolean {
                    self->showError(
                        QUrl(QString::fromUtf8(uri)),
                        "The site's security certificate could not be verified."
                    );
                    return TRUE;
                }
            ),
            this
        );
        g_signal_connect(
            m_view,
            "web-process-terminated",
            G_CALLBACK(+[](WebKitWebView *, WebKitWebProcessTerminationReason, WpeEngineView *self) {
                self->showError(self->url(), "This page stopped responding. Reload to try again.");
            }),
            this
        );
        g_signal_connect(
            m_view,
            "insecure-content-detected",
            G_CALLBACK(+[](WebKitWebView *, WebKitInsecureContentEvent, WpeEngineView *self) {
                self->m_insecureContent = true;
                emit self->securityStateChanged();
            }),
            this
        );
        g_signal_connect(
            m_view,
            "create",
            G_CALLBACK(
                +[](WebKitWebView *source, WebKitNavigationAction *action, WpeEngineView *self) -> WebKitWebView * {
                    WpeNewViewRequest request(source, self->m_profile, action);
                    emit self->newViewRequested(&request);
                    return request.adoptedView();
                }
            ),
            this
        );
        g_signal_connect(
            m_view,
            "close",
            G_CALLBACK(+[](WebKitWebView *, WpeEngineView *self) {
                QMetaObject::invokeMethod(self, [self] { emit self->closeRequested(); }, Qt::QueuedConnection);
            }),
            this
        );
        g_signal_connect(
            m_view,
            "decide-policy",
            G_CALLBACK(
                +[](WebKitWebView *, WebKitPolicyDecision *decision, WebKitPolicyDecisionType type, WpeEngineView *)
                    -> gboolean {
                    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
                        auto *response = WEBKIT_RESPONSE_POLICY_DECISION(decision);
                        if (!webkit_response_policy_decision_is_mime_type_supported(response)) {
                            webkit_policy_decision_download(decision);
                            return TRUE;
                        }
                    }
                    return FALSE;
                }
            ),
            this
        );
        g_signal_connect(
            m_view,
            "enter-fullscreen",
            G_CALLBACK(+[](WebKitWebView *, WpeEngineView *self) -> gboolean {
                emit self->fullscreenRequested(true);
                return FALSE;
            }),
            this
        );
        g_signal_connect(
            m_view,
            "leave-fullscreen",
            G_CALLBACK(+[](WebKitWebView *, WpeEngineView *self) -> gboolean {
                emit self->fullscreenRequested(false);
                return FALSE;
            }),
            this
        );
        g_signal_connect(
            m_view,
            "script-dialog",
            G_CALLBACK(+[](WebKitWebView *, WebKitScriptDialog *dialog, WpeEngineView *self) -> gboolean {
                JavaScriptDialogInfo info;
                info.id = self->m_nextRequest++;
                const auto *origin =
                    static_cast<const char *>(g_object_get_data(G_OBJECT(self->m_view), "eden-dialog-origin-v1"));
                info.origin = autofillOrigin(QUrl(QString::fromUtf8(origin)));
                info.message = QString::fromUtf8(webkit_script_dialog_get_message(dialog));
                switch (webkit_script_dialog_get_dialog_type(dialog)) {
                case WEBKIT_SCRIPT_DIALOG_ALERT:
                    info.kind = "alert";
                    break;
                case WEBKIT_SCRIPT_DIALOG_CONFIRM:
                    info.kind = "confirm";
                    break;
                case WEBKIT_SCRIPT_DIALOG_PROMPT:
                    info.kind = "prompt";
                    info.defaultText = QString::fromUtf8(webkit_script_dialog_prompt_get_default_text(dialog));
                    break;
                case WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM:
                    info.kind = "beforeUnload";
                    break;
                }
                self->m_dialogs.insert(info.id, webkit_script_dialog_ref(dialog));
                emit self->javaScriptDialogRequested(info);
                return TRUE;
            }),
            this
        );
        g_signal_connect(
            m_view,
            "run-file-chooser",
            G_CALLBACK(+[](WebKitWebView *, WebKitFileChooserRequest *request, WpeEngineView *self) -> gboolean {
                FileDialogInfo info;
                info.id = self->m_nextRequest++;
                info.title = "Choose files";
                info.mode = webkit_file_chooser_request_get_select_multiple(request) ? "openMultiple" : "open";
                if (const char *const *types = webkit_file_chooser_request_get_mime_types(request)) {
                    for (auto type = types; *type; ++type) {
                        const auto mime = QMimeDatabase().mimeTypeForName(QString::fromUtf8(*type));
                        if (mime.isValid() && !mime.globPatterns().isEmpty()) {
                            info.nameFilters.append(mime.filterString());
                        }
                    }
                }
                self->m_files.insert(info.id, WEBKIT_FILE_CHOOSER_REQUEST(g_object_ref(request)));
                emit self->fileDialogRequested(info);
                return TRUE;
            }),
            this
        );
        g_signal_connect(
            m_view,
            "permission-request",
            G_CALLBACK(+[](WebKitWebView *, WebKitPermissionRequest *request, WpeEngineView *self) -> gboolean {
                return self->permissionRequest(request);
            }),
            this
        );
        g_signal_connect(
            m_view,
            "query-permission-state",
            G_CALLBACK(+[](WebKitWebView *, WebKitPermissionStateQuery *query, WpeEngineView *) -> gboolean {
                webkit_permission_state_query_finish(query, WEBKIT_PERMISSION_STATE_PROMPT);
                return TRUE;
            }),
            this
        );
        g_signal_connect(
            m_view,
            "context-menu",
            G_CALLBACK(
                +[](WebKitWebView *, WebKitContextMenu *menu, WebKitHitTestResult *hit, WpeEngineView *self)
                    -> gboolean {
                    self->showContextMenu(menu, hit, false);
                    return TRUE;
                }
            ),
            this
        );
        g_signal_connect(
            m_view,
            "show-option-menu",
            G_CALLBACK(
                +[](WebKitWebView *, WebKitOptionMenu *menu, WebKitRectangle *rectangle, WpeEngineView *self)
                    -> gboolean {
                    self->dismissContextMenu();
                    self->m_optionMenu = WEBKIT_OPTION_MENU(g_object_ref(menu));
                    ContextMenuInfo info;
                    info.position = QPoint(rectangle->x, rectangle->y + rectangle->height);
                    for (guint index = 0; index < webkit_option_menu_get_n_items(menu); ++index) {
                        auto *item = webkit_option_menu_get_item(menu, index);
                        info.actions.append(
                            QVariantMap{
                                {"id", QString("wpe-option-%1").arg(index)},
                                {"title", QString::fromUtf8(webkit_option_menu_item_get_label(item))},
                                {"enabled",
                                 bool(
                                     webkit_option_menu_item_is_enabled(item) &&
                                     !webkit_option_menu_item_is_group_label(item)
                                 )},
                                {"checked", bool(webkit_option_menu_item_is_selected(item))}
                            }
                        );
                    }
                    emit self->contextMenuRequested(info);
                    return TRUE;
                }
            ),
            this
        );
        synchronizeState();
    }

    void WpeEngineView::destroyNative() {
        ++m_generation;
        m_committed = false;
        cancelRequests();
        closeDevTools();
        delete m_surface.data();
        if (m_cancellable) {
            g_cancellable_cancel(m_cancellable);
            g_clear_object(&m_cancellable);
        }
        if (m_view) {
            g_signal_handlers_disconnect_by_data(webkit_web_view_get_back_forward_list(m_view), this);
            g_signal_handlers_disconnect_by_data(m_view, this);
            webkit_web_view_stop_loading(m_view);
            g_clear_object(&m_view);
        }
        if (m_content) {
            g_signal_handlers_disconnect_by_data(m_content, this);
            g_clear_object(&m_content);
        }
    }

    void WpeEngineView::synchronizeState() {
        if (!m_view) {
            return;
        }
        QUrl nextUrl(QString::fromUtf8(webkit_web_view_get_uri(m_view)));
        if (nextUrl.scheme() == "webkit") {
            nextUrl.setScheme("eden");
        }
        const QString nextTitle = QString::fromUtf8(webkit_web_view_get_title(m_view));
        const int nextProgress =
            qBound(0, int(std::lround(webkit_web_view_get_estimated_load_progress(m_view) * 100)), 100);
        const bool nextLoading = webkit_web_view_is_loading(m_view);
        const bool nextBack = webkit_web_view_can_go_back(m_view);
        const bool nextForward = webkit_web_view_can_go_forward(m_view);
        const bool nextAudible = webkit_web_view_is_playing_audio(m_view);
        const bool nextMuted = webkit_web_view_get_is_muted(m_view);
        if (m_url != nextUrl) {
            m_url = nextUrl;
            emit urlChanged();
            emit securityStateChanged();
            emit certificateDetailsChanged();
        }
        if (m_title != nextTitle) {
            m_title = nextTitle;
            emit titleChanged();
        }
        if (m_progress != nextProgress) {
            m_progress = nextProgress;
            emit loadProgressChanged();
        }
        if (m_loading != nextLoading) {
            m_loading = nextLoading;
            emit loadingChanged();
        }
        if (m_back != nextBack) {
            m_back = nextBack;
            emit canGoBackChanged();
        }
        if (m_forward != nextForward) {
            m_forward = nextForward;
            emit canGoForwardChanged();
        }
        if (m_audible != nextAudible) {
            m_audible = nextAudible;
            emit audibleChanged();
        }
        if (m_muted != nextMuted) {
            m_muted = nextMuted;
            emit mutedChanged();
        }
        const bool video = webkit_web_view_get_camera_capture_state(m_view) != WEBKIT_MEDIA_CAPTURE_STATE_NONE ||
                           webkit_web_view_get_display_capture_state(m_view) != WEBKIT_MEDIA_CAPTURE_STATE_NONE;
        const bool audio = webkit_web_view_get_microphone_capture_state(m_view) != WEBKIT_MEDIA_CAPTURE_STATE_NONE;
        setMediaCapture(video, audio);
        const int activities =
            (webkit_web_view_get_camera_capture_state(m_view) != WEBKIT_MEDIA_CAPTURE_STATE_NONE ? 1 : 0) |
            (audio ? 2 : 0) |
            (webkit_web_view_get_display_capture_state(m_view) != WEBKIT_MEDIA_CAPTURE_STATE_NONE ? 4 : 0);
        setCaptureDetails("wpe-native-capture", activities);
    }

    void WpeEngineView::invalidateFormTarget() {
        m_committed = false;
        m_documentId.clear();
        m_formOrigin.clear();
        emit formFieldFocused({});
    }

    void WpeEngineView::navigationChanged(WebKitLoadEvent event) {
        if (event == WEBKIT_LOAD_STARTED) {
            ++m_generation;
            m_committed = false;
            m_documentId.clear();
            m_formOrigin.clear();
            m_insecureContent = false;
            cancelRequests();
            installFormHooks();
            if (m_surface) {
                m_surface->resetInput();
            }
            if (!m_favicon.isEmpty()) {
                m_favicon.clear();
                m_faviconResolution = 0;
                emit faviconUrlChanged();
            }
            emit formFieldFocused({});
        }
        if (event == WEBKIT_LOAD_COMMITTED ||
            (event == WEBKIT_LOAD_FINISHED && webkit_web_view_get_main_resource(m_view))) {
            m_committed = true;
        }
        synchronizeState();
        if (event == WEBKIT_LOAD_COMMITTED || event == WEBKIT_LOAD_FINISHED) {
            const auto generation = m_generation;
            const QPointer<WpeEngineView> guard(this);
            requestAutofillTarget([guard, generation](AutofillTarget target) {
                if (guard && guard->m_generation == generation && target.isValid()) {
                    guard->m_documentId = target.documentId;
                    guard->m_formOrigin = target.origin;
                }
            });
        }
    }

    void WpeEngineView::showError(const QUrl &uri, const QString &message) {
        if (m_errorPage) {
            return;
        }
        m_errorPage = true;
        const QString html = QString(
                                 "<!doctype html><meta charset=utf-8><meta name=color-scheme content='dark "
                                 "light'><title>Page unavailable</title><style>body{font:18px system-ui;margin:12vh "
                                 "auto;max-width:42em;padding:2em}h1{font-size:2em}p{line-height:1.6}</style><h1>Page "
                                 "unavailable</h1><p>%1</p><p>Use Reload to try again.</p>"
        )
                                 .arg(message.toHtmlEscaped());
        webkit_web_view_load_alternate_html(m_view, html.toUtf8().constData(), uri.toEncoded().constData(), nullptr);
    }

    QUrl WpeEngineView::url() const {
        return m_url;
    }
    QString WpeEngineView::title() const {
        return m_title;
    }
    QUrl WpeEngineView::faviconUrl() const {
        return m_favicon;
    }
    int WpeEngineView::loadProgress() const {
        return m_progress;
    }
    bool WpeEngineView::isLoading() const {
        return m_loading;
    }
    bool WpeEngineView::canGoBack() const {
        return m_back;
    }
    bool WpeEngineView::canGoForward() const {
        return m_forward;
    }
    bool WpeEngineView::isAudible() const {
        return m_audible;
    }
    bool WpeEngineView::isMuted() const {
        return m_muted;
    }
    qint64 WpeEngineView::rendererProcessId() const {
        using ProcessID = gint64 (*)(WebKitWebView *);
        const auto processID = reinterpret_cast<ProcessID>(
            g_type_get_qdata(WEBKIT_TYPE_WEB_VIEW, g_quark_from_static_string("eden-renderer-process-id-v1"))
        );
        return m_view && processID ? processID(m_view) : 0;
    }

    QString WpeEngineView::backendName() const {
        return "WebKit";
    }
    EngineView::Capabilities WpeEngineView::capabilities() const {
        Capabilities result = HistorySerialization | ThumbnailCapture | OsrCompositing | PerTabMute;
        if (g_type_get_qdata(WEBKIT_TYPE_WEB_VIEW, g_quark_from_static_string("eden-embedding-api-v1"))) {
            result |= DockedDevtools;
        }
        return result;
    }
    WebKitWebView *WpeEngineView::nativeView() const {
        return m_view;
    }
    WpeSurface *WpeEngineView::surface() const {
        return m_surface;
    }

    QString WpeEngineView::securityState() const {
        if (m_errorPage) {
            return "insecure";
        }
        if (m_url.scheme() == "about" || m_url.scheme() == "eden" || m_url.scheme() == "file" ||
            m_url.scheme() == "webkit") {
            return "local";
        }
        GTlsCertificate *certificate = nullptr;
        GTlsCertificateFlags errors{};
        return m_url.scheme() == "https" && !m_insecureContent &&
                       webkit_web_view_get_tls_info(m_view, &certificate, &errors) && !errors
                   ? "secure"
                   : "insecure";
    }

    QVariantMap WpeEngineView::certificateDetails() const {
        GTlsCertificate *native = nullptr;
        GTlsCertificateFlags errors{};
        if (!m_view || !webkit_web_view_get_tls_info(m_view, &native, &errors) || !native) {
            return {};
        }
        QVariantList chain;
        QSet<GTlsCertificate *> visited;
        for (auto *entry = native; entry && chain.size() < 32 && !visited.contains(entry);
             entry = g_tls_certificate_get_issuer(entry)) {
            visited.insert(entry);
            char *pem = nullptr;
            g_object_get(entry, "certificate-pem", &pem, nullptr);
            const QSslCertificate certificate{QByteArray(pem)};
            g_free(pem);
            if (certificate.isNull()) {
                continue;
            }
            chain.append(
                QVariantMap{
                    {"subject", certificate.subjectInfo(QSslCertificate::CommonName).join(", ")},
                    {"issuer", certificate.issuerInfo(QSslCertificate::CommonName).join(", ")},
                    {"serialNumber", QString::fromLatin1(certificate.serialNumber())},
                    {"validFrom", certificate.effectiveDate()},
                    {"validUntil", certificate.expiryDate()},
                    {"sha256", QString::fromLatin1(certificate.digest(QCryptographicHash::Sha256).toHex(':').toUpper())}
                }
            );
        }
        if (chain.isEmpty()) {
            return {};
        }
        QVariantMap details = chain.first().toMap();
        details.insert("secure", !errors && !m_insecureContent);
        details.insert("error", errors != 0);
        details.insert("certificateStatus", static_cast<qulonglong>(errors));
        details.insert("chainLength", chain.size());
        details.insert("chain", chain);
        return details;
    }

    void WpeEngineView::load(const QUrl &uri) {
        if (!m_view) {
            return;
        }
        invalidateFormTarget();
        m_errorPage = false;
        webkit_web_view_load_uri(m_view, uri.toEncoded().constData());
    }
    void WpeEngineView::back() {
        if (m_view) {
            invalidateFormTarget();
            m_errorPage = false;
            webkit_web_view_go_back(m_view);
        }
    }
    void WpeEngineView::forward() {
        if (m_view) {
            invalidateFormTarget();
            m_errorPage = false;
            webkit_web_view_go_forward(m_view);
        }
    }
    void WpeEngineView::reload() {
        if (m_view) {
            if (m_errorPage) {
                load(m_url);
            } else {
                invalidateFormTarget();
                webkit_web_view_reload(m_view);
            }
        }
    }
    void WpeEngineView::stop() {
        if (m_view) {
            webkit_web_view_stop_loading(m_view);
        }
    }
    void WpeEngineView::setMuted(bool muted) {
        if (m_view) {
            webkit_web_view_set_is_muted(m_view, muted);
        }
    }
    void WpeEngineView::releaseFocus() {
        if (m_surface) {
            m_surface->setFocus(false);
        }
    }
    void WpeEngineView::attach(QQuickItem *viewport) {
        if (m_surface) {
            m_surface->attach(viewport);
        }
        if (viewport && viewport->window()) {
            connect(
                viewport->window(),
                &QQuickWindow::frameSwapped,
                this,
                [this] {
                    if (m_profile) {
                        WpeRuntime::initializeSpellChecking(m_profile->context());
                    }
                },
                Qt::ConnectionType(Qt::QueuedConnection | Qt::SingleShotConnection)
            );
        }
    }
    void WpeEngineView::exitFullscreen() {
        evaluate("if(document.fullscreenElement){document.exitFullscreen()}");
    }
    QUrl WpeEngineView::internalUrlFor(const QUrl &uri) const {
        QUrl result = uri;
        if (result.scheme() == "eden") {
            result.setScheme("webkit");
        }
        return result;
    }

    bool WpeEngineView::containsPageScenePoint(const QPointF &position) const {
        return m_surface && m_surface->isVisible() && m_surface->contains(m_surface->mapFromScene(position));
    }

#if EDEN_ENABLE_AUTOMATION
    bool WpeEngineView::automationWheel(int delta) {
        if (!m_surface || !m_surface->isVisible()) {
            return false;
        }
        const QPointF position = m_surface->boundingRect().center();
        QWheelEvent event(
            position,
            m_surface->mapToGlobal(position),
            {},
            QPoint(0, delta),
            Qt::NoButton,
            Qt::NoModifier,
            Qt::NoScrollPhase,
            false
        );
        return QCoreApplication::sendEvent(m_surface, &event);
    }
#endif

    QVariantList WpeEngineView::navigationHistory(int direction, int maximumItems) const {
        QVariantList result;
        if (!m_view || !direction) {
            return result;
        }
        auto *history = webkit_web_view_get_back_forward_list(m_view);
        const int step = direction < 0 ? -1 : 1;
        for (int count = 1; count <= qBound(0, maximumItems, 100); ++count) {
            const int offset = step * count;
            auto *item = webkit_back_forward_list_get_nth_item(history, offset);
            if (!item) {
                break;
            }
            result.append(
                QVariantMap{
                    {"offset", offset},
                    {"url", QUrl(QString::fromUtf8(webkit_back_forward_list_item_get_uri(item)))},
                    {"title", QString::fromUtf8(webkit_back_forward_list_item_get_title(item))}
                }
            );
        }
        return result;
    }

    void WpeEngineView::goToHistoryOffset(int offset) {
        if (!m_view || !offset) {
            return;
        }
        auto *item = webkit_back_forward_list_get_nth_item(webkit_web_view_get_back_forward_list(m_view), offset);
        if (item) {
            invalidateFormTarget();
            m_errorPage = false;
            webkit_web_view_go_to_back_forward_list_item(m_view, item);
        }
    }

    void WpeEngineView::findInPage(const QString &text, FindFlags flags) {
        if (!m_view) {
            return;
        }
        auto *controller = webkit_web_view_get_find_controller(m_view);
        if (text.isEmpty()) {
            webkit_find_controller_search_finish(controller);
            return;
        }
        unsigned options = WEBKIT_FIND_OPTIONS_WRAP_AROUND;
        if (!(flags & FindCaseSensitive)) {
            options |= WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE;
        }
        if (flags & FindBackward) {
            options |= WEBKIT_FIND_OPTIONS_BACKWARDS;
        }
        webkit_find_controller_search(controller, text.toUtf8().constData(), options, 10000);
    }

    bool WpeEngineView::adoptPopup(WebKitWebView *source, WpeProfile *profile) {
        if (profile != m_profile || !source) {
            return false;
        }
        QPointer<QQuickItem> viewport(m_surface ? m_surface->parentItem() : nullptr);
        destroyNative();
        createNative(source);
        attach(viewport);
        return m_view != nullptr;
    }

    void WpeEngineView::openDevTools() {
        if (!m_view || m_inspector || !(capabilities() & DockedDevtools)) {
            return;
        }
        auto *view = WpeRuntime::captureInspector([this] { webkit_web_view_toggle_inspector(m_view); });
        if (!view) {
            return;
        }
        if (g_signal_lookup("eden-context-menu-v1", WPE_TYPE_VIEW)) {
            g_signal_connect(
                view,
                "eden-context-menu-v1",
                G_CALLBACK(+[](WPEView *, WebKitContextMenu *menu, WebKitHitTestResult *hit, WpeEngineView *self) {
                    self->showContextMenu(menu, hit, true);
                }),
                this
            );
        }
        m_inspector = new WpeSurface(view);
        m_inspector->setParent(this);
        connect(m_inspector, &WpeSurface::shortcutRequested, this, &EngineView::shortcutRequested);
        connect(
            m_inspector,
            &WpeSurface::nativeClosed,
            this,
            [this, inspector = QPointer<WpeSurface>(m_inspector)] {
                if (!inspector || m_inspector != inspector) {
                    return;
                }
                g_signal_handlers_disconnect_by_data(inspector->nativeView(), this);
                if (m_inspectorMenu) {
                    dismissContextMenu();
                }
                delete m_inspector.data();
                setDevToolsOpen(false);
            },
            Qt::QueuedConnection
        );
        g_object_unref(view);
        setDevToolsOpen(true);
        if (m_inspectorViewport) {
            m_inspector->attach(m_inspectorViewport);
        }
    }

    void WpeEngineView::closeDevTools() {
        if (!m_inspector) {
            setDevToolsOpen(false);
            return;
        }
        if (m_inspectorMenu) {
            dismissContextMenu();
        }
        g_signal_handlers_disconnect_by_data(m_inspector->nativeView(), this);
        delete m_inspector.data();
        if (m_view) {
            webkit_web_view_toggle_inspector(m_view);
        }
        setDevToolsOpen(false);
    }

    void WpeEngineView::attachDevTools(QQuickItem *viewport) {
        m_inspectorViewport = viewport;
        if (m_inspector) {
            m_inspector->attach(viewport);
        }
    }
    void WpeEngineView::detachDevTools(QQuickItem *viewport) {
        if (m_inspectorViewport != viewport) {
            return;
        }
        m_inspectorViewport.clear();
        if (m_inspector) {
            m_inspector->attach(nullptr);
        }
    }

    void WpeEngineView::updateFavicon(GBytes *bytes) {
        gsize length = 0;
        const auto *data = static_cast<const char *>(g_bytes_get_data(bytes, &length));
        if (!length || length > 262144) {
            return;
        }
        QBuffer input;
        input.setData(QByteArray(data, qsizetype(length)));
        if (!input.open(QIODevice::ReadOnly)) {
            return;
        }
        int bestImage = 0;
        int bestResolution = 0;
        {
            QImageReader reader(&input);
            const int count = std::clamp(reader.imageCount(), 1, 64);
            for (int index = 0; index < count; ++index) {
                if (index && !reader.jumpToImage(index)) {
                    break;
                }
                const QSize size = reader.size();
                if (size.isEmpty() || size.width() > 4096 || size.height() > 4096) {
                    continue;
                }
                const int resolution = reader.format() == "svg" ? 128 : std::min({128, size.width(), size.height()});
                if (resolution > bestResolution) {
                    bestResolution = resolution;
                    bestImage = index;
                }
            }
        }
        if (!bestResolution || bestResolution < m_faviconResolution || !input.seek(0)) {
            return;
        }
        QImageReader reader(&input);
        if (bestImage && !reader.jumpToImage(bestImage)) {
            return;
        }
        const QSize size = reader.size();
        if (size.width() > 128 || size.height() > 128 || reader.format() == "svg") {
            reader.setScaledSize(size.scaled(128, 128, Qt::KeepAspectRatio));
        }
        QImage image = reader.read();
        if (image.isNull()) {
            return;
        }
        if (image.width() > 128 || image.height() > 128) {
            image = image.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        QByteArray encoded;
        QBuffer output(&encoded);
        if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "PNG")) {
            return;
        }
        m_faviconResolution = bestResolution;
        const QUrl next("data:image/png;base64," + QString::fromLatin1(encoded.toBase64()));
        if (m_favicon != next) {
            m_favicon = next;
            emit faviconUrlChanged();
        }
    }

    void WpeEngineView::showContextMenu(WebKitContextMenu *menu, WebKitHitTestResult *hit, bool inspector) {
        dismissContextMenu();
        m_inspectorMenu = inspector;
        ContextMenuInfo info;
        auto *surface = inspector ? m_inspector.data() : m_surface.data();
        info.position = surface ? surface->pointerPosition() : QPoint();
        int x = 0;
        int y = 0;
        if (webkit_context_menu_get_position(menu, &x, &y)) {
            info.position = QPoint(x, y);
        }
        info.surface = inspector ? "devtools" : "page";
        info.linkUrl = QUrl(QString::fromUtf8(webkit_hit_test_result_get_link_uri(hit)));
        info.mediaUrl = QUrl(
            QString::fromUtf8(
                webkit_hit_test_result_context_is_image(hit) ? webkit_hit_test_result_get_image_uri(hit)
                                                             : webkit_hit_test_result_get_media_uri(hit)
            )
        );
        info.editable = webkit_hit_test_result_context_is_editable(hit);
        info.actions = menuActions(menu);
        if (!inspector && (capabilities() & DockedDevtools) &&
            g_type_get_qdata(WEBKIT_TYPE_WEB_VIEW, g_quark_from_static_string("eden-show-page-source-v1"))) {
            info.actions.append(
                QVariantMap{{"id", "view_source"}, {"title", "View page source"}, {"icon", "code"}, {"enabled", true}}
            );
        }
        emit contextMenuRequested(info);
    }

    static QString menuIcon(WebKitContextMenuAction action, const QString &title) {
        switch (action) {
        case WEBKIT_CONTEXT_MENU_ACTION_OPEN_LINK:
            return "link";
        case WEBKIT_CONTEXT_MENU_ACTION_OPEN_LINK_IN_NEW_WINDOW:
        case WEBKIT_CONTEXT_MENU_ACTION_OPEN_IMAGE_IN_NEW_WINDOW:
        case WEBKIT_CONTEXT_MENU_ACTION_OPEN_FRAME_IN_NEW_WINDOW:
        case WEBKIT_CONTEXT_MENU_ACTION_OPEN_VIDEO_IN_NEW_WINDOW:
        case WEBKIT_CONTEXT_MENU_ACTION_OPEN_AUDIO_IN_NEW_WINDOW:
            return "new-window";
        case WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_LINK_TO_DISK:
        case WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_IMAGE_TO_DISK:
        case WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_VIDEO_TO_DISK:
        case WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_AUDIO_TO_DISK:
            return "download";
        case WEBKIT_CONTEXT_MENU_ACTION_COPY_LINK_TO_CLIPBOARD:
        case WEBKIT_CONTEXT_MENU_ACTION_COPY_VIDEO_LINK_TO_CLIPBOARD:
        case WEBKIT_CONTEXT_MENU_ACTION_COPY_AUDIO_LINK_TO_CLIPBOARD:
            return "link";
        case WEBKIT_CONTEXT_MENU_ACTION_COPY_IMAGE_TO_CLIPBOARD:
        case WEBKIT_CONTEXT_MENU_ACTION_COPY:
            return "copy";
        case WEBKIT_CONTEXT_MENU_ACTION_CUT:
            return "cut";
        case WEBKIT_CONTEXT_MENU_ACTION_PASTE:
            return "clipboard";
        case WEBKIT_CONTEXT_MENU_ACTION_GO_BACK:
            return "arrow-left";
        case WEBKIT_CONTEXT_MENU_ACTION_GO_FORWARD:
            return "arrow-right";
        case WEBKIT_CONTEXT_MENU_ACTION_STOP:
            return "close";
        case WEBKIT_CONTEXT_MENU_ACTION_RELOAD:
            return "refresh";
        case WEBKIT_CONTEXT_MENU_ACTION_INSPECT_ELEMENT:
            return "code";
        case WEBKIT_CONTEXT_MENU_ACTION_ENTER_VIDEO_FULLSCREEN:
            return "maximize";
        case WEBKIT_CONTEXT_MENU_ACTION_MEDIA_MUTE:
            return "muted";
        case WEBKIT_CONTEXT_MENU_ACTION_MEDIA_PLAY:
        case WEBKIT_CONTEXT_MENU_ACTION_MEDIA_PAUSE:
            return "volume";
        case WEBKIT_CONTEXT_MENU_ACTION_TOGGLE_MEDIA_LOOP:
            return "refresh";
        case WEBKIT_CONTEXT_MENU_ACTION_TOGGLE_MEDIA_CONTROLS:
        case WEBKIT_CONTEXT_MENU_ACTION_FONT_MENU:
            return "tuning";
        case WEBKIT_CONTEXT_MENU_ACTION_SPELLING_GUESS:
        case WEBKIT_CONTEXT_MENU_ACTION_NO_GUESSES_FOUND:
        case WEBKIT_CONTEXT_MENU_ACTION_IGNORE_SPELLING:
        case WEBKIT_CONTEXT_MENU_ACTION_LEARN_SPELLING:
        case WEBKIT_CONTEXT_MENU_ACTION_IGNORE_GRAMMAR:
        case WEBKIT_CONTEXT_MENU_ACTION_BOLD:
        case WEBKIT_CONTEXT_MENU_ACTION_ITALIC:
        case WEBKIT_CONTEXT_MENU_ACTION_UNDERLINE:
        case WEBKIT_CONTEXT_MENU_ACTION_OUTLINE:
            return "text-selection";
        default:
            break;
        }
        const QString text = title.toLower();
        for (const auto &[needle, icon] :
             {std::pair{"copy", "copy"},
              {"paste", "clipboard"},
              {"cut", "cut"},
              {"undo", "undo"},
              {"redo", "redo"},
              {"select", "text-selection"},
              {"delete", "trash"},
              {"remove", "trash"},
              {"clear", "trash"},
              {"edit", "gallery-edit"},
              {"expand", "expand"},
              {"collapse", "collapse"},
              {"save", "download"},
              {"download", "download"},
              {"source", "code"},
              {"breakpoint", "code"},
              {"reveal", "object-scan"},
              {"inspect", "object-scan"},
              {"open", "new-window"},
              {"search", "search"},
              {"filter", "filter"},
              {"sort", "sort"},
              {"enable", "check"},
              {"disable", "close"},
              {"add", "add"},
              {"hide", "eye"},
              {"show", "eye"},
              {"log", "programming"},
              {"capture", "camera"}}) {
            if (text.contains(QLatin1String(needle))) {
                return QLatin1String(icon);
            }
        }
        return "list";
    }

    QVariantList WpeEngineView::menuActions(WebKitContextMenu *menu) {
        QVariantList result;
        for (auto *entry = webkit_context_menu_get_items(menu); entry; entry = entry->next) {
            auto *item = WEBKIT_CONTEXT_MENU_ITEM(entry->data);
            if (webkit_context_menu_item_is_separator(item)) {
                result.append(QVariantMap{{"separator", true}});
                continue;
            }
            GAction *action = webkit_context_menu_item_get_gaction(item);
            const bool inspect =
                webkit_context_menu_item_get_stock_action(item) == WEBKIT_CONTEXT_MENU_ACTION_INSPECT_ELEMENT;
            if (inspect && !(capabilities() & DockedDevtools)) {
                continue;
            }
            const QString id = inspect ? QString("inspect") : QString("wpe-menu-%1").arg(m_menuActions.size());
            if (action) {
                m_menuActions.insert(id, WEBKIT_CONTEXT_MENU_ITEM(g_object_ref(item)));
            }
            QString label = QString::fromUtf8(webkit_context_menu_item_get_title(item));
            label.remove('_');
            QVariantMap value{
                {"id", id},
                {"title", label},
                {"icon", menuIcon(webkit_context_menu_item_get_stock_action(item), label)},
                {"enabled", action && g_action_get_enabled(action)}
            };
            if (auto *submenu = webkit_context_menu_item_get_submenu(item)) {
                value.insert("children", menuActions(submenu));
            }
            if (action) {
                GVariant *state = g_action_get_state(action);
                if (state && g_variant_is_of_type(state, G_VARIANT_TYPE_BOOLEAN)) {
                    value.insert("checked", bool(g_variant_get_boolean(state)));
                }
                if (state) {
                    g_variant_unref(state);
                }
            }
            result.append(value);
        }
        return result;
    }

    void WpeEngineView::executeContextMenuCommand(const QString &command) {
        if (m_optionMenu && command.startsWith("wpe-option-")) {
            bool valid = false;
            const uint index = command.mid(11).toUInt(&valid);
            if (valid && index < webkit_option_menu_get_n_items(m_optionMenu)) {
                auto *item = webkit_option_menu_get_item(m_optionMenu, index);
                if (webkit_option_menu_item_is_enabled(item) && !webkit_option_menu_item_is_group_label(item)) {
                    webkit_option_menu_activate_item(m_optionMenu, index);
                }
            }
        } else if (auto *item = m_menuActions.value(command)) {
            g_object_ref(item);
            if (command == "inspect") {
                openDevTools();
            }
            dismissContextMenu();
            auto *action = webkit_context_menu_item_get_gaction(item);
            if (g_action_get_enabled(action)) {
                g_action_activate(action, webkit_context_menu_item_get_gaction_target(item));
            }
            g_object_unref(item);
            return;
        } else if (command == "inspect") {
            openDevTools();
        } else if (command == "view_source") {
            using ShowSource = void (*)(WebKitWebView *);
            auto showSource = reinterpret_cast<ShowSource>(
                g_type_get_qdata(WEBKIT_TYPE_WEB_VIEW, g_quark_from_static_string("eden-show-page-source-v1"))
            );
            if (showSource) {
                openDevTools();
                if (m_inspector) {
                    showSource(m_view);
                }
            }
        } else if (command == "copy") {
            webkit_web_view_execute_editing_command(m_view, WEBKIT_EDITING_COMMAND_COPY);
        } else if (command == "paste") {
            webkit_web_view_execute_editing_command(m_view, WEBKIT_EDITING_COMMAND_PASTE);
        } else if (command == "cut") {
            webkit_web_view_execute_editing_command(m_view, WEBKIT_EDITING_COMMAND_CUT);
        } else if (command == "selectAll" || command == "select_all") {
            webkit_web_view_execute_editing_command(m_view, WEBKIT_EDITING_COMMAND_SELECT_ALL);
        }
        dismissContextMenu();
    }

    void WpeEngineView::dismissContextMenu() {
        for (auto *action : std::as_const(m_menuActions)) {
            g_object_unref(action);
        }
        m_menuActions.clear();
        m_inspectorMenu = false;
        if (m_optionMenu) {
            webkit_option_menu_close(m_optionMenu);
            g_clear_object(&m_optionMenu);
        }
    }

    void WpeEngineView::resolveJavaScriptDialog(quint64 id, bool accepted, const QString &text) {
        auto *dialog = m_dialogs.take(id);
        if (!dialog) {
            return;
        }
        const auto type = webkit_script_dialog_get_dialog_type(dialog);
        if (type == WEBKIT_SCRIPT_DIALOG_CONFIRM || type == WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM) {
            webkit_script_dialog_confirm_set_confirmed(dialog, accepted);
        }
        if (type == WEBKIT_SCRIPT_DIALOG_PROMPT && accepted) {
            webkit_script_dialog_prompt_set_text(dialog, text.toUtf8().constData());
        }
        webkit_script_dialog_close(dialog);
        webkit_script_dialog_unref(dialog);
        emit javaScriptDialogClosed(id);
    }

    void WpeEngineView::resolveFileDialog(quint64 id, bool accepted, const QList<QUrl> &files) {
        auto *request = m_files.take(id);
        if (!request) {
            return;
        }
        QList<QByteArray> paths;
        for (const auto &url : files) {
            if (url.isLocalFile() && !url.toLocalFile().contains(QChar('\0'))) {
                paths.append(QFile::encodeName(url.toLocalFile()));
            }
        }
        if (accepted && !paths.isEmpty()) {
            QVector<const char *> pointers;
            for (const auto &path : paths) {
                pointers.append(path.constData());
            }
            pointers.append(nullptr);
            webkit_file_chooser_request_select_files(request, pointers.constData());
        } else {
            webkit_file_chooser_request_cancel(request);
        }
        g_object_unref(request);
        emit fileDialogClosed(id);
    }

    bool WpeEngineView::permissionRequest(WebKitPermissionRequest *request) {
        const auto *originText =
            static_cast<const char *>(g_object_get_data(G_OBJECT(request), "eden-permission-origin-v1"));
        const QUrl origin(QString::fromUtf8(originText));
        QStringList permissions;
        if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(request)) {
            permissions.append("location");
        } else if (WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(request)) {
            permissions.append("notifications");
        } else if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(request)) {
            auto *media = WEBKIT_USER_MEDIA_PERMISSION_REQUEST(request);
            if (webkit_user_media_permission_is_for_display_device(media)) {
                if (origin.isEmpty() || origin != autofillOrigin(origin)) {
                    webkit_permission_request_deny(request);
                    return true;
                }
                DisplayCaptureRequestInfo info;
                info.id = m_nextRequest++;
                info.origin = origin;
                info.audioRequested = webkit_user_media_permission_is_for_audio_device(media);
                m_displayRequests.insert(info.id, WEBKIT_PERMISSION_REQUEST(g_object_ref(request)));
                emit displayCaptureRequested(info);
                return true;
            } else {
                if (webkit_user_media_permission_is_for_audio_device(media)) {
                    permissions.append("microphone");
                }
                if (webkit_user_media_permission_is_for_video_device(media)) {
                    permissions.append("camera");
                }
            }
        }
        if (origin.isEmpty() || origin != autofillOrigin(origin) || permissions.isEmpty()) {
            webkit_permission_request_deny(request);
            return true;
        }
        PermissionRequestInfo info;
        info.id = m_nextRequest++;
        info.origin = origin;
        info.permissions = permissions;
        m_permissions.insert(info.id, WEBKIT_PERMISSION_REQUEST(g_object_ref(request)));
        emit permissionRequested(info);
        return true;
    }

    void WpeEngineView::resolvePermissionRequest(quint64 id, bool allowed) {
        auto *request = m_permissions.take(id);
        if (!request) {
            return;
        }
        if (allowed) {
            webkit_permission_request_allow(request);
        } else {
            webkit_permission_request_deny(request);
        }
        g_object_unref(request);
        emit permissionRequestClosed(id);
    }

    void WpeEngineView::dismissPermissionRequest(quint64 id) {
        resolvePermissionRequest(id, false);
    }

    void WpeEngineView::resolveDisplayCaptureRequest(quint64 id, const QString &source) {
        auto *request = m_displayRequests.take(id);
        if (!request) {
            return;
        }
        if (source == "window" || source == "screen") {
            g_object_set_data(G_OBJECT(request), "eden-display-window-v1", GINT_TO_POINTER(source == "window"));
            webkit_permission_request_allow(request);
        } else {
            webkit_permission_request_deny(request);
        }
        g_object_unref(request);
        emit displayCaptureRequestClosed(id);
    }

    void WpeEngineView::cancelRequests() {
        dismissContextMenu();
        for (auto id : m_dialogs.keys()) {
            resolveJavaScriptDialog(id, false, {});
        }
        for (auto id : m_files.keys()) {
            resolveFileDialog(id, false, {});
        }
        for (auto id : m_permissions.keys()) {
            dismissPermissionRequest(id);
        }
        for (auto id : m_displayRequests.keys()) {
            resolveDisplayCaptureRequest(id, {});
        }
    }

    void WpeEngineView::evaluate(const QString &script, std::function<void(QVariant)> callback, const char *world) {
        if (!m_view) {
            if (callback) {
                callback({});
            }
            return;
        }
        struct Evaluation {
            QPointer<WpeEngineView> view;
            quint64 generation;
            std::function<void(QVariant)> callback;
        };
        auto *evaluation = new Evaluation{this, m_generation, std::move(callback)};
        const QByteArray bytes = script.toUtf8();
        webkit_web_view_evaluate_javascript(
            m_view,
            bytes.constData(),
            bytes.size(),
            world,
            nullptr,
            m_cancellable,
            [](GObject *object, GAsyncResult *result, gpointer data) {
                std::unique_ptr<Evaluation> evaluation(static_cast<Evaluation *>(data));
                GError *error = nullptr;
                JSCValue *value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(object), result, &error);
                QVariant converted;
                if (value && evaluation->view && evaluation->view->m_generation == evaluation->generation && !error) {
                    if (jsc_value_is_string(value)) {
                        char *text = jsc_value_to_string(value);
                        converted = QString::fromUtf8(text);
                        g_free(text);
                    } else if (jsc_value_is_boolean(value)) {
                        converted = bool(jsc_value_to_boolean(value));
                    } else if (jsc_value_is_number(value)) {
                        converted = jsc_value_to_double(value);
                    } else {
                        char *json = jsc_value_to_json(value, 0);
                        if (json) {
                            converted = QJsonDocument::fromJson(QByteArray(json)).toVariant();
                            g_free(json);
                        }
                    }
                }
                g_clear_object(&value);
                g_clear_error(&error);
                if (evaluation->callback) {
                    evaluation->callback(converted);
                }
            },
            evaluation
        );
    }

    void WpeEngineView::installFormHooks() {
        m_formToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
        static const QByteArray source = [] {
            QFile file(":/eden/wpe/wpeforms.js");
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        }();
        if (source.isEmpty()) {
            return;
        }
        const QByteArray script = QByteArray(source).replace("EDEN_FORM_TOKEN", m_formToken.toUtf8());
        auto *native = webkit_user_script_new_for_world(
            script.constData(),
            WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
            "eden.forms",
            nullptr,
            nullptr
        );
        webkit_user_content_manager_remove_all_scripts(m_content);
        webkit_user_content_manager_add_script(m_content, native);
        webkit_user_script_unref(native);
    }

    void WpeEngineView::handleFormMessage(JSCValue *message) {
        char *json = jsc_value_to_json(message, 0);
        const QJsonObject report = QJsonDocument::fromJson(QByteArray(json)).object();
        g_free(json);
        const QUrl origin(report.value("origin").toString());
        if (m_documentId.isEmpty() || report.value("token").toString() != m_formToken ||
            report.value("documentId").toString() != m_documentId || origin != m_formOrigin ||
            origin != autofillOrigin(m_url)) {
            return;
        }
        const auto values = report.value("values").toArray();
        if (report.value("kind") == "credential" && values.size() == 2 && values[0].isString() &&
            values[1].isString()) {
            CredentialSubmissionInfo info;
            info.origin = origin;
            info.username = values[0].toString();
            info.password = values[1].toString();
            emit credentialSubmitted(info);
        } else if (report.value("kind") == "field" && values.size() == 8) {
            for (int index = 0; index < 4; ++index) {
                if (!values[index].isString()) {
                    return;
                }
            }
            for (int index = 4; index < 8; ++index) {
                if (!values[index].isDouble() || !std::isfinite(values[index].toDouble())) {
                    return;
                }
            }
            if (values[0].toString().isEmpty()) {
                emit formFieldFocused({});
                return;
            }
            emit formFieldFocused(
                {{"origin", origin},
                 {"type", values[0].toString()},
                 {"name", values[1].toString()},
                 {"autocomplete", values[2].toString()},
                 {"value", values[0] == "password" ? QString() : values[3].toString()},
                 {"x", values[4].toDouble()},
                 {"y", values[5].toDouble()},
                 {"width", values[6].toDouble()},
                 {"height", values[7].toDouble()}}
            );
        }
    }

    void WpeEngineView::requestAutofillTarget(AutofillTargetCallback callback) {
        if (!callback) {
            return;
        }
        if (!m_committed || m_errorPage) {
            callback({});
            return;
        }
        const QByteArray payload =
            QJsonDocument(QJsonArray{QUuid::createUuid().toString(QUuid::WithoutBraces), m_formToken})
                .toJson(QJsonDocument::Compact);
        const QString script =
            QString(
                "(function(values){if(window.top!==window||globalThis.origin==='null'){return "
                "null;}if(!Object.hasOwn(globalThis,'__edenAutofillDocument')){Object.defineProperty(globalThis,'__"
                "edenAutofillDocument',{value:values[0]});}globalThis.__edenFormToken=values[1];return{documentId:"
                "globalThis.__edenAutofillDocument,origin:globalThis.origin,url:location.href};})(%1)"
            )
                .arg(QString::fromUtf8(payload));
        const auto generation = m_generation;
        evaluate(
            script,
            [guard = QPointer<WpeEngineView>(this), generation, callback = std::move(callback)](QVariant result) {
                const auto values = result.toMap();
                AutofillTarget target{
                    QUrl(values.value("url").toString()),
                    QUrl(values.value("origin").toString()),
                    values.value("documentId").toString()
                };
                if (!guard || guard->m_generation != generation || !target.isValid() ||
                    target.origin != autofillOrigin(guard->m_url)) {
                    target = {};
                }
                if (guard && target.isValid()) {
                    guard->m_documentId = target.documentId;
                    guard->m_formOrigin = target.origin;
                }
                callback(std::move(target));
            }
        );
    }

    void WpeEngineView::requestThumbnail(const QSize &size, ThumbnailCallback callback) {
        if (!callback) {
            return;
        }
        if (!m_view || size.isEmpty() || size.width() > 4096 || size.height() > 4096) {
            callback({});
            return;
        }
        struct Snapshot {
            QPointer<WpeEngineView> view;
            quint64 generation;
            QSize size;
            ThumbnailCallback callback;
        };
        auto *snapshot = new Snapshot{this, m_generation, size, std::move(callback)};
        webkit_web_view_get_snapshot(
            m_view,
            WEBKIT_SNAPSHOT_REGION_VISIBLE,
            WEBKIT_SNAPSHOT_OPTIONS_NONE,
            m_cancellable,
            [](GObject *object, GAsyncResult *result, gpointer data) {
                std::unique_ptr<Snapshot> snapshot(static_cast<Snapshot *>(data));
                GError *error = nullptr;
                WebKitImage *image = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(object), result, &error);
                QImage thumbnail;
                if (image && snapshot->view && snapshot->view->m_generation == snapshot->generation && !error) {
                    GBytes *bytes = webkit_image_as_bytes(image);
                    gsize length = 0;
                    const auto *pixels = static_cast<const uchar *>(g_bytes_get_data(bytes, &length));
                    const int width = webkit_image_get_width(image);
                    const int height = webkit_image_get_height(image);
                    const guint stride = webkit_image_get_stride(image);
                    if (width > 0 && height > 0 && length >= quint64(stride) * height && stride >= quint64(width) * 4) {
                        thumbnail =
                            QImage(pixels, width, height, stride, QImage::Format_ARGB32_Premultiplied)
                                .scaled(snapshot->size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                        thumbnail = thumbnail.copy(
                            (thumbnail.width() - snapshot->size.width()) / 2,
                            (thumbnail.height() - snapshot->size.height()) / 2,
                            snapshot->size.width(),
                            snapshot->size.height()
                        );
                    }
                }
                g_clear_object(&image);
                g_clear_error(&error);
                snapshot->callback(thumbnail);
            },
            snapshot
        );
    }

    QVariantMap WpeEngineView::serializeState() const {
        QVariantMap state = EngineView::serializeState();
        if (!m_view) {
            return state;
        }
        auto *session = webkit_web_view_get_session_state(m_view);
        GBytes *bytes = webkit_web_view_session_state_serialize(session);
        gsize length = 0;
        const auto *data = static_cast<const char *>(g_bytes_get_data(bytes, &length));
        if (length <= 8 * 1024 * 1024) {
            state.insert("wpeSession", QString::fromLatin1(QByteArray(data, qsizetype(length)).toBase64()));
        }
        g_bytes_unref(bytes);
        webkit_web_view_session_state_unref(session);
        return state;
    }

    void WpeEngineView::restoreState(const QVariantMap &state) {
        setMuted(state.value("muted").toBool());
        const QString encoded = state.value("wpeSession").toString();
        if (m_view && !encoded.isEmpty() && encoded.size() <= 12 * 1024 * 1024) {
            const QByteArray decoded =
                QByteArray::fromBase64(encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
            GBytes *bytes = g_bytes_new(decoded.constData(), decoded.size());
            auto *session = webkit_web_view_session_state_new(bytes);
            g_bytes_unref(bytes);
            if (session) {
                webkit_web_view_restore_session_state(m_view, session);
                webkit_web_view_session_state_unref(session);
                auto *current =
                    webkit_back_forward_list_get_current_item(webkit_web_view_get_back_forward_list(m_view));
                if (current) {
                    webkit_web_view_go_to_back_forward_list_item(m_view, current);
                    return;
                }
            }
        }
        EngineView::restoreState(state);
    }

    void WpeEngineView::fillCredential(const AutofillTarget &target, const QString &username, const QString &password) {
        if (!m_view || !m_committed || m_errorPage || !target.isValid() || target.documentId != m_documentId ||
            target.origin != m_formOrigin) {
            return;
        }
        const QByteArray values =
            QJsonDocument(QJsonArray{username, password, target.documentId, target.origin.toString(QUrl::FullyEncoded)})
                .toJson(QJsonDocument::Compact);
        const QString script =
            QStringLiteral(
                "(function(values){if(globalThis.__edenAutofillDocument!==values[2]||globalThis.origin!==values[3])"
                "return;var fields=Array.from(document.querySelectorAll('input,textarea'));var tokens="
                "function(f){return String(f.autocomplete||'').toLowerCase().split(/\\s+/);};var has=function(f,v)"
                "{return tokens(f).indexOf(v)>=0;};var identity=function(f){return[f.name,f.id,f.placeholder,f."
                "getAttribute('aria-label')].filter(Boolean).join(' ').toLowerCase();};var secret=fields.find(function"
                "(f){return!f.disabled&&!f.readOnly&&!has(f,'one-time-code')&&(f.type==='password'||has(f,'current-"
                "password')||/password|passwd|passphrase|token|api[ _-]*key|secret|access[ _-"
                "]*key/.test(identity(f)));});if(!secret)return;var username=fields.find(function(f){return!f.disabled"
                "&&(has(f,'username')||has(f,'email')||f.type==='email'||/user|email|login|identifier/.test(identity"
                "(f)));});if(!username){var i=fields.indexOf(secret);for(var p=i-1;p>=0;--p){if(/^(text|email|tel|$)/"
                ".test(fields[p].type||'')){username=fields[p];break;}}}var set=function(field,value){if(!field)return;"
                "var prototype=field.tagName==='TEXTAREA'?HTMLTextAreaElement.prototype:HTMLInputElement.prototype;"
                "var setter=Object.getOwnPropertyDescriptor(prototype,'value').set;setter.call(field,value);"
                "field.dispatchEvent(new Event('input',{bubbles:true}));field.dispatchEvent(new Event('change',"
                "{bubbles:true}));};set(username,values[0]);set(secret,values[1]);})(%1)"
            )
                .arg(QString::fromUtf8(values));
        evaluate(script);
    }

    void WpeEngineView::fillForm(const AutofillTarget &target, const QVariantMap &fields) {
        if (!m_view || !m_committed || m_errorPage || !target.isValid() || target.documentId != m_documentId ||
            target.origin != m_formOrigin) {
            return;
        }
        const QByteArray values = QJsonDocument(
                                      QJsonArray{
                                          QJsonObject::fromVariantMap(fields),
                                          target.documentId,
                                          target.origin.toString(QUrl::FullyEncoded)
                                      }
        )
                                      .toJson(QJsonDocument::Compact);
        const QString script =
            QStringLiteral(
                "(function(payload){if(globalThis.__edenAutofillDocument!==payload[1]||globalThis.origin!==payload[2])"
                "return;var values=payload[0];var "
                "aliases={name:['name','full-name'],email:['email'],phone:['tel','phone'],"
                "addressLine1:['address-line1','street-address'],addressLine2:['address-line2'],city:["
                "'address-level2','city'],region:['address-level1','state','province'],postalCode:['postal-code',"
                "'zip'],country:['country','country-name']};var normalize=function(v){return String(v||'')."
                "toLowerCase().replace(/[^a-z0-9]/g,'');};var set=function(field,value){if(!field||!value)return;"
                "var prototype=field.tagName==='TEXTAREA'?HTMLTextAreaElement.prototype:HTMLInputElement.prototype;"
                "var setter=Object.getOwnPropertyDescriptor(prototype,'value').set;setter.call(field,value);field."
                "dispatchEvent(new Event('input',{bubbles:true}));field.dispatchEvent(new Event('change',{bubbles:"
                "true}));};Object.keys(aliases).forEach(function(key){var names=aliases[key].map(normalize);var field="
                "Array.from(document.querySelectorAll('input,textarea')).find(function(item){var candidates=[item."
                "autocomplete,item.name,item.id,item.placeholder].map(normalize);return candidates.some(function("
                "candidate){return names.some(function(name){return candidate.indexOf(name)>=0;});});});set(field,"
                "values[key]);});})(%1)"
            )
                .arg(QString::fromUtf8(values));
        evaluate(script);
    }

}

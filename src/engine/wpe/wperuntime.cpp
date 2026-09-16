#include "engine/wpe/wperuntime.h"
#include "engine/wpe/wpedisplaysync.h"
#include "engine/wpe/wpesurface.h"

#include <EGL/egl.h>
#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QInputMethod>
#include <QMimeData>
#include <QStyleHints>
#include <QThread>
#include <wpe/headless/wpe-headless.h>

namespace eden::engine {

    struct EdenWpeView {
        WPEView parent;
    };
    struct EdenWpeViewClass {
        WPEViewClass parent;
    };
    struct EdenWpeToplevel {
        WPEToplevel parent;
    };
    struct EdenWpeToplevelClass {
        WPEToplevelClass parent;
    };
    struct EdenWpeInput {
        WPEInputMethodContext parent;
    };
    struct EdenWpeInputClass {
        WPEInputMethodContextClass parent;
    };
    struct EdenWpeClipboard {
        WPEClipboard parent;
        bool synchronizing;
    };
    struct EdenWpeClipboardClass {
        WPEClipboardClass parent;
    };
    struct EdenWpeDisplay {
        WPEDisplay parent;
        WPEDisplay *graphics;
        WPEClipboard *clipboard;
        bool explicitSync;
    };
    struct EdenWpeDisplayClass {
        WPEDisplayClass parent;
    };

    G_DEFINE_TYPE(EdenWpeView, eden_wpe_view, WPE_TYPE_VIEW)
    G_DEFINE_TYPE(EdenWpeToplevel, eden_wpe_toplevel, WPE_TYPE_TOPLEVEL)
    G_DEFINE_TYPE(EdenWpeInput, eden_wpe_input, WPE_TYPE_INPUT_METHOD_CONTEXT)
    G_DEFINE_TYPE(EdenWpeClipboard, eden_wpe_clipboard, WPE_TYPE_CLIPBOARD)
    G_DEFINE_TYPE(EdenWpeDisplay, eden_wpe_display, WPE_TYPE_DISPLAY)

    static WPEDisplay *runtimeDisplay = nullptr;
    static std::unique_ptr<WpeDisplaySync> runtimeDisplaySync;
    static QString runtimeUserAgentSuffix;
    static bool spellCheckingInitialized = false;
    static WPEView **inspectorCapture = nullptr;
    static QMetaObject::Connection colorSchemeConnection;
    static QMetaObject::Connection clipboardConnection;

    static WpeSurface *surfaceFor(WPEView *view) {
        return view ? static_cast<WpeSurface *>(g_object_get_data(G_OBJECT(view), "eden-surface")) : nullptr;
    }

    void bindWpeSurface(WPEView *view, WpeSurface *surface) {
        g_object_set_data(G_OBJECT(view), "eden-surface", surface);
        if (!surface && runtimeDisplaySync) {
            runtimeDisplaySync->bind(view, nullptr, false);
        }
        auto *input = static_cast<WPEInputMethodContext *>(g_object_get_data(G_OBJECT(view), "eden-input"));
        if (surface && input) {
            surface->setInputContext(input);
        }
    }

    static void eden_wpe_view_init(EdenWpeView *) {}

    static void eden_wpe_view_class_init(EdenWpeViewClass *klass) {
        auto *viewClass = WPE_VIEW_CLASS(klass);
        viewClass->render_buffer =
            [](WPEView *view, WPEBuffer *buffer, const WPERectangle *, guint, GError **) -> gboolean {
            if (auto *surface = surfaceFor(view)) {
                return surface->submit(buffer);
            }
            auto frame = std::make_shared<WpeFrame>(view, buffer);
            frame->rendered();
            return TRUE;
        };
        viewClass->can_be_mapped = [](WPEView *) -> gboolean {
            return TRUE;
        };
        viewClass->set_cursor_from_name = [](WPEView *view, const char *name) {
            if (auto *surface = surfaceFor(view)) {
                surface->setPointerCursor(QString::fromUtf8(name));
            }
        };
        viewClass->set_cursor_from_bytes =
            [](WPEView *view, GBytes *bytes, guint width, guint height, guint stride, guint x, guint y) {
                gsize length = 0;
                const auto *pixels = static_cast<const uchar *>(g_bytes_get_data(bytes, &length));
                if (width > 0 && height > 0 && width <= 1024 && height <= 1024 && stride >= width * 4 &&
                    length >= quint64(stride) * height) {
                    if (auto *surface = surfaceFor(view)) {
                        surface->setPointerCursor(
                            QImage(pixels, int(width), int(height), int(stride), QImage::Format_ARGB32_Premultiplied)
                                .copy(),
                            QPoint(int(x), int(y))
                        );
                    }
                }
            };
    }

    static void eden_wpe_toplevel_init(EdenWpeToplevel *) {}

    static void eden_wpe_toplevel_class_init(EdenWpeToplevelClass *klass) {
        auto *toplevelClass = WPE_TOPLEVEL_CLASS(klass);
        toplevelClass->get_screen = [](WPEToplevel *toplevel) -> WPEScreen * {
            return static_cast<WPEScreen *>(g_object_get_data(G_OBJECT(toplevel), "eden-screen"));
        };
        toplevelClass->resize = [](WPEToplevel *toplevel, int width, int height) -> gboolean {
            wpe_toplevel_resized(toplevel, width, height);
            return TRUE;
        };
        toplevelClass->set_fullscreen = [](WPEToplevel *toplevel, gboolean fullscreen) -> gboolean {
            auto state = unsigned(wpe_toplevel_get_state(toplevel));
            state = fullscreen ? state | WPE_TOPLEVEL_STATE_FULLSCREEN : state & ~WPE_TOPLEVEL_STATE_FULLSCREEN;
            wpe_toplevel_state_changed(toplevel, WPEToplevelState(state));
            return TRUE;
        };
    }

    static WpeSurface *inputSurface(WPEInputMethodContext *context) {
        return surfaceFor(wpe_input_method_context_get_view(context));
    }

    static void eden_wpe_input_init(EdenWpeInput *) {}

    static void eden_wpe_input_class_init(EdenWpeInputClass *klass) {
        auto *inputClass = WPE_INPUT_METHOD_CONTEXT_CLASS(klass);
        inputClass->filter_key_event = [](WPEInputMethodContext *, WPEEvent *) -> gboolean {
            return FALSE;
        };
        inputClass->focus_in = [](WPEInputMethodContext *context) {
            if (auto *surface = inputSurface(context)) {
                surface->setInputFocus(true);
            }
        };
        inputClass->focus_out = [](WPEInputMethodContext *context) {
            if (auto *surface = inputSurface(context)) {
                surface->setInputFocus(false);
            }
        };
        inputClass->set_cursor_area = [](WPEInputMethodContext *context, int x, int y, int width, int height) {
            if (auto *surface = inputSurface(context)) {
                surface->setInputCursor(QRect(x, y, width, height));
            }
        };
        inputClass->set_surrounding =
            [](WPEInputMethodContext *context, const char *text, guint length, guint cursor, guint selection) {
                if (auto *surface = inputSurface(context)) {
                    const QByteArray bytes(text, length);
                    surface->setInputSurrounding(
                        QString::fromUtf8(bytes),
                        QString::fromUtf8(bytes.first(qMin(cursor, length))).size(),
                        QString::fromUtf8(bytes.first(qMin(selection, length))).size()
                    );
                }
            };
        inputClass->get_preedit_string =
            [](WPEInputMethodContext *context, char **text, GList **underlines, guint *cursor) {
                auto *surface = inputSurface(context);
                const QString preedit = surface ? surface->inputPreedit() : QString();
                *text = g_strdup(preedit.toUtf8().constData());
                *underlines = preedit.isEmpty()
                                  ? nullptr
                                  : g_list_append(nullptr, wpe_input_method_underline_new(0, preedit.toUcs4().size()));
                *cursor = surface ? preedit.left(surface->inputPreeditCursor()).toUcs4().size() : 0;
            };
        inputClass->reset = [](WPEInputMethodContext *context) {
            if (auto *surface = inputSurface(context)) {
                surface->resetInput();
            }
        };
    }

    static void eden_wpe_clipboard_init(EdenWpeClipboard *clipboard) {
        clipboard->synchronizing = false;
    }

    static void eden_wpe_clipboard_class_init(EdenWpeClipboardClass *klass) {
        auto *clipboardClass = WPE_CLIPBOARD_CLASS(klass);
        clipboardClass->read = [](WPEClipboard *, const char *format) -> GBytes * {
            const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
            if (!mime) {
                return nullptr;
            }
            const QByteArray bytes = QByteArrayView(format).startsWith("text/plain")
                                         ? mime->text().toUtf8()
                                         : mime->data(QString::fromUtf8(format));
            return g_bytes_new(bytes.constData(), bytes.size());
        };
        clipboardClass->changed =
            [](WPEClipboard *clipboard, GPtrArray *formats, gboolean local, WPEClipboardContent *content) {
                WPE_CLIPBOARD_CLASS(eden_wpe_clipboard_parent_class)->changed(clipboard, formats, local, content);
                auto *native = reinterpret_cast<EdenWpeClipboard *>(clipboard);
                if (!local || native->synchronizing) {
                    return;
                }
                native->synchronizing = true;
                auto *mime = new QMimeData;
                if (content && formats) {
                    for (guint index = 0; index < formats->len; ++index) {
                        const auto *format = static_cast<const char *>(g_ptr_array_index(formats, index));
                        if (!format) {
                            break;
                        }
                        GBytes *bytes = wpe_clipboard_content_get_bytes(content, format);
                        if (bytes) {
                            gsize size = 0;
                            const auto *data = static_cast<const char *>(g_bytes_get_data(bytes, &size));
                            mime->setData(QString::fromUtf8(format), QByteArray(data, qsizetype(size)));
                        }
                    }
                    if (const char *text = wpe_clipboard_content_get_text(content)) {
                        mime->setText(QString::fromUtf8(text));
                    }
                }
                QGuiApplication::clipboard()->setMimeData(mime);
                native->synchronizing = false;
            };
    }

    static void updateClipboard(EdenWpeClipboard *clipboard) {
        if (clipboard->synchronizing) {
            return;
        }
        GPtrArray *formats = g_ptr_array_new();
        if (const QMimeData *mime = QGuiApplication::clipboard()->mimeData()) {
            QStringList textFormats = mime->formats();
            if (mime->hasText()) {
                textFormats.append("text/plain");
                textFormats.append("text/plain;charset=utf-8");
                textFormats.removeDuplicates();
            }
            for (const QString &format : textFormats) {
                g_ptr_array_add(formats, const_cast<char *>(g_intern_string(format.toUtf8().constData())));
            }
        }
        g_ptr_array_add(formats, nullptr);
        WPE_CLIPBOARD_CLASS(eden_wpe_clipboard_parent_class)
            ->changed(WPE_CLIPBOARD(clipboard), formats, FALSE, nullptr);
        g_ptr_array_unref(formats);
    }

    static void eden_wpe_display_init(EdenWpeDisplay *display) {
        display->graphics = wpe_display_headless_new();
        display->clipboard = nullptr;
        display->explicitSync = false;
    }

    static void eden_wpe_display_class_init(EdenWpeDisplayClass *klass) {
        G_OBJECT_CLASS(klass)->dispose = [](GObject *object) {
            auto *display = reinterpret_cast<EdenWpeDisplay *>(object);
            g_clear_object(&display->clipboard);
            g_clear_object(&display->graphics);
            G_OBJECT_CLASS(eden_wpe_display_parent_class)->dispose(object);
        };
        auto *displayClass = WPE_DISPLAY_CLASS(klass);
        displayClass->get_n_screens = [](WPEDisplay *) -> guint {
            return runtimeDisplaySync ? runtimeDisplaySync->screenCount() : 0;
        };
        displayClass->get_screen = [](WPEDisplay *, guint index) -> WPEScreen * {
            return runtimeDisplaySync ? runtimeDisplaySync->screen(index) : nullptr;
        };
        displayClass->connect = [](WPEDisplay *display, GError **error) -> gboolean {
            auto *native = reinterpret_cast<EdenWpeDisplay *>(display);
            if (!wpe_display_connect(native->graphics, error)) {
                return FALSE;
            }
            const auto eglDisplay = static_cast<EGLDisplay>(wpe_display_get_egl_display(native->graphics, nullptr));
            if (eglDisplay != EGL_NO_DISPLAY && qgetenv("EDEN_WPE_EXPLICIT_SYNC") != "0") {
                const QByteArray extensions(eglQueryString(eglDisplay, EGL_EXTENSIONS));
                const auto names = extensions.split(' ');
                native->explicitSync = names.contains("EGL_ANDROID_native_fence_sync") &&
                                       names.contains("EGL_KHR_wait_sync") && eglGetProcAddress("eglCreateSyncKHR") &&
                                       eglGetProcAddress("eglDestroySyncKHR") && eglGetProcAddress("eglWaitSyncKHR");
            }
            return TRUE;
        };
        displayClass->get_egl_display = [](WPEDisplay *display, GError **error) -> gpointer {
            return wpe_display_get_egl_display(reinterpret_cast<EdenWpeDisplay *>(display)->graphics, error);
        };
        displayClass->use_explicit_sync = [](WPEDisplay *display) -> gboolean {
            return reinterpret_cast<EdenWpeDisplay *>(display)->explicitSync;
        };
        displayClass->get_drm_device = [](WPEDisplay *display) -> WPEDRMDevice * {
            return wpe_display_get_drm_device(reinterpret_cast<EdenWpeDisplay *>(display)->graphics);
        };
        displayClass->create_toplevel = [](WPEDisplay *display, guint maxViews) -> WPEToplevel * {
            return WPE_TOPLEVEL(
                g_object_new(eden_wpe_toplevel_get_type(), "display", display, "max-views", maxViews, nullptr)
            );
        };
        displayClass->create_view = [](WPEDisplay *display) -> WPEView * {
            auto *view = WPE_VIEW(g_object_new(eden_wpe_view_get_type(), "display", display, nullptr));
            if (inspectorCapture && !*inspectorCapture) {
                *inspectorCapture = WPE_VIEW(g_object_ref(view));
            }
            return view;
        };
        displayClass->create_input_method_context = [](WPEDisplay *, WPEView *view) -> WPEInputMethodContext * {
            auto *context = WPE_INPUT_METHOD_CONTEXT(g_object_new(eden_wpe_input_get_type(), "view", view, nullptr));
            g_object_set_data_full(G_OBJECT(view), "eden-input", g_object_ref(context), g_object_unref);
            if (auto *surface = surfaceFor(view)) {
                surface->setInputContext(context);
            }
            return context;
        };
        displayClass->get_clipboard = [](WPEDisplay *display) -> WPEClipboard * {
            auto *native = reinterpret_cast<EdenWpeDisplay *>(display);
            if (!native->clipboard) {
                native->clipboard =
                    WPE_CLIPBOARD(g_object_new(eden_wpe_clipboard_get_type(), "display", display, nullptr));
                updateClipboard(reinterpret_cast<EdenWpeClipboard *>(native->clipboard));
                clipboardConnection =
                    QObject::connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, qApp, [native] {
                        updateClipboard(reinterpret_cast<EdenWpeClipboard *>(native->clipboard));
                    });
            }
            return native->clipboard;
        };
    }

    bool WpeRuntime::initialize(const QString &product, const QString &version) {
        Q_ASSERT(QThread::currentThread() == qApp->thread());
        if (runtimeDisplay) {
            return true;
        }
        auto *display = WPE_DISPLAY(g_object_new(eden_wpe_display_get_type(), nullptr));
        GError *error = nullptr;
        if (!wpe_display_connect(display, &error)) {
            qCritical("WebKit display initialization failed: %s", error ? error->message : "Unknown error");
            g_clear_error(&error);
            g_object_unref(display);
            return false;
        }
        wpe_display_set_available_input_devices(
            display,
            WPEAvailableInputDevices(
                WPE_AVAILABLE_INPUT_DEVICE_MOUSE | WPE_AVAILABLE_INPUT_DEVICE_KEYBOARD |
                WPE_AVAILABLE_INPUT_DEVICE_TOUCHSCREEN
            )
        );
        const auto updateColorScheme = [display] {
            GError *error = nullptr;
            if (!wpe_settings_set_boolean(
                    wpe_display_get_settings(display),
                    WPE_SETTING_DARK_MODE,
                    QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark,
                    WPE_SETTINGS_SOURCE_PLATFORM,
                    &error
                )) {
                qWarning("WebKit color scheme could not be updated: %s", error ? error->message : "Unknown error");
                g_clear_error(&error);
            }
        };
        updateColorScheme();
        colorSchemeConnection =
            QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, qApp, updateColorScheme);
        if (qEnvironmentVariable("EDEN_WPE_DISPLAY_SYNC") == "1") {
            runtimeDisplaySync = std::make_unique<WpeDisplaySync>(display);
        }
        wpe_display_set_primary(display);
        runtimeDisplay = display;
        runtimeUserAgentSuffix = product + '/' + version;
        return true;
    }

    void WpeRuntime::initializeSpellChecking(WebKitWebContext *context) {
        if (spellCheckingInitialized || !context) {
            return;
        }
        spellCheckingInitialized = true;
        const char *languages[] = {nullptr};
        webkit_web_context_set_spell_checking_languages(context, languages);
        const char *const *loaded = webkit_web_context_get_spell_checking_languages(context);
        webkit_web_context_set_spell_checking_enabled(context, loaded && *loaded);
    }

    void WpeRuntime::shutdown() {
        spellCheckingInitialized = false;
        QObject::disconnect(clipboardConnection);
        QObject::disconnect(colorSchemeConnection);
        runtimeDisplaySync.reset();
        g_clear_object(&runtimeDisplay);
    }

    guint64 WpeRuntime::displayPulseCount() {
        return runtimeDisplaySync ? runtimeDisplaySync->pulseCount() : 0;
    }

    void WpeRuntime::synchronizeScreen(WPEView *view, QQuickWindow *window, bool visible) {
        if (runtimeDisplaySync) {
            runtimeDisplaySync->bind(view, window, visible);
        }
    }

    WPEDisplay *WpeRuntime::display() {
        return runtimeDisplay;
    }

    const QString &WpeRuntime::userAgentSuffix() {
        return runtimeUserAgentSuffix;
    }

    WPEView *WpeRuntime::captureInspector(const std::function<void()> &open) {
        WPEView *result = nullptr;
        auto **previous = inspectorCapture;
        inspectorCapture = &result;
        open();
        inspectorCapture = previous;
        return result;
    }

}

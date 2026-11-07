#include "engine/wpe/wpesurface.h"
#include "core/settings/shortcutregistry.h"
#include "engine/wpe/wperuntime.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QInputMethod>
#include <QKeyEvent>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QPainter>
#include <QPalette>
#include <QPointingDevice>
#include <QQuickPaintedItem>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QThread>
#include <cerrno>
#include <cstring>
#include <deque>
#include <drm_fourcc.h>
#include <poll.h>
#include <qsgtexture_platform.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

namespace eden::engine {

    static const auto createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    static const auto destroyImage =
        reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    static const auto createSync = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
    static const auto destroySync = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
    static const auto clientWaitSync =
        reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(eglGetProcAddress("eglClientWaitSyncKHR"));
    static const auto waitSync = reinterpret_cast<PFNEGLWAITSYNCKHRPROC>(eglGetProcAddress("eglWaitSyncKHR"));
    static const auto duplicateFence =
        reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(eglGetProcAddress("eglDupNativeFenceFDANDROID"));

    static bool hasEglExtension(EGLDisplay display, const char *extension) {
        const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
        if (!extensions) {
            return false;
        }
        const size_t length = std::strlen(extension);
        const char *found = extensions;
        while ((found = std::strstr(found, extension))) {
            if ((found == extensions || found[-1] == ' ') && (found[length] == '\0' || found[length] == ' ')) {
                return true;
            }
            found += length;
        }
        return false;
    }

    static const int mainThreadCallType = QEvent::registerEventType();

    class WpeMainThreadCall final : public QEvent {
      public:
        explicit WpeMainThreadCall(std::function<void()> call)
            : QEvent(QEvent::Type(mainThreadCallType)),
              callback(std::move(call)) {}
        std::function<void()> callback;
    };

    class WpeMainThreadCaller final : public QObject {
      public:
        using QObject::QObject;

      protected:
        bool event(QEvent *event) override {
            if (event->type() == mainThreadCallType) {
                static_cast<WpeMainThreadCall *>(event)->callback();
                return true;
            }
            return QObject::event(event);
        }
    };

    static QPointer<WpeMainThreadCaller> mainThreadCaller;
    static std::atomic<int> heldFramesPeak{0};

    static void ensureMainThreadCaller() {
        if (!mainThreadCaller && qApp && QThread::isMainThread()) {
            mainThreadCaller = new WpeMainThreadCaller(qApp);
        }
    }

    static void onMain(std::function<void()> callback) {
        if (QThread::isMainThread()) {
            callback();
        } else if (mainThreadCaller) {
            QCoreApplication::postEvent(
                mainThreadCaller,
                new WpeMainThreadCall(std::move(callback)),
                Qt::HighEventPriority
            );
        } else {
            QMetaObject::invokeMethod(qApp, std::move(callback), Qt::QueuedConnection);
        }
    }

    static void onWpeMain(GMainContext *context, std::function<void()> callback, int priority) {
        auto *task = new std::function<void()>(std::move(callback));
        GSource *source = g_idle_source_new();
        g_source_set_priority(source, priority);
        g_source_set_callback(
            source,
            +[](gpointer data) -> gboolean {
                (*static_cast<std::function<void()> *>(data))();
                return G_SOURCE_REMOVE;
            },
            task,
            +[](gpointer data) { delete static_cast<std::function<void()> *>(data); }
        );
        g_source_attach(source, context);
        g_source_unref(source);
        g_main_context_wakeup(context);
    }

    WpeFrame::WpeFrame(WPEView *nativeView, WPEBuffer *nativeBuffer)
        : view(WPE_VIEW(g_object_ref(nativeView))),
          buffer(WPE_BUFFER(g_object_ref(nativeBuffer))),
          size(wpe_buffer_get_width(nativeBuffer), wpe_buffer_get_height(nativeBuffer)),
          scale(wpe_view_get_scale(nativeView)),
          renderingFence(wpe_buffer_take_rendering_fence(nativeBuffer)),
          mainContext(g_main_context_ref_thread_default()) {
        ensureMainThreadCaller();
        const int held = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(view), "eden-held-frames")) + 1;
        g_object_set_data(G_OBJECT(view), "eden-held-frames", GINT_TO_POINTER(held));
        int peak = heldFramesPeak.load();
        while (held > peak && !heldFramesPeak.compare_exchange_weak(peak, held)) {
        }
        if (held == 3) {
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true)) {
                qInfo("Eden WebKit view holds %d buffers at once", held);
            }
        }
    }

    WpeFrame::~WpeFrame() {
        const bool needsAcknowledgement = !acknowledged.exchange(true);
        int fence = releaseFence.exchange(-1);
        const int unusedRenderingFence = renderingFence.exchange(-1);
        if (unusedRenderingFence >= 0) {
            if (fence >= 0) {
                close(fence);
            }
            fence = unusedRenderingFence;
        }
        auto release = [nativeView = view, nativeBuffer = buffer, fence, needsAcknowledgement] {
            if (needsAcknowledgement) {
                wpe_view_buffer_rendered(nativeView, nativeBuffer);
            }
            if (fence >= 0) {
                wpe_buffer_set_release_fence(nativeBuffer, fence);
            }
            wpe_view_buffer_released(nativeView, nativeBuffer);
            const int held = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(nativeView), "eden-held-frames"));
            g_object_set_data(G_OBJECT(nativeView), "eden-held-frames", GINT_TO_POINTER(qMax(0, held - 1)));
            g_object_unref(nativeBuffer);
            g_object_unref(nativeView);
        };
        if (needsAcknowledgement) {
            onMain(std::move(release));
        } else {
            onWpeMain(mainContext, std::move(release), G_PRIORITY_HIGH_IDLE + 1);
        }
        g_main_context_unref(mainContext);
    }

    int WpeFrame::heldPeak() {
        return heldFramesPeak.load();
    }

    void WpeFrame::rendered() {
        if (acknowledged.exchange(true)) {
            return;
        }
        auto *retainedView = WPE_VIEW(g_object_ref(view));
        auto *retainedBuffer = WPE_BUFFER(g_object_ref(buffer));
        onWpeMain(
            mainContext,
            [retainedView, retainedBuffer] {
                wpe_view_buffer_rendered(retainedView, retainedBuffer);
                g_object_unref(retainedBuffer);
                g_object_unref(retainedView);
            },
            G_PRIORITY_HIGH_IDLE
        );
    }

    class WpeTextureNode final : public QSGSimpleTextureNode {
      public:
        struct CachedTexture {
            WPEBuffer *buffer;
            QSGTexture *texture;
            GLuint textureId;
            EGLDisplay display;
            EGLImageKHR image;
        };

        ~WpeTextureNode() override {
            releaseTexture();
            for (auto &entry : cache) {
                destroyCached(entry);
            }
        }

        static void destroyCached(CachedTexture &entry) {
            delete entry.texture;
            if (entry.textureId && QOpenGLContext::currentContext()) {
                QOpenGLContext::currentContext()->extraFunctions()->glDeleteTextures(1, &entry.textureId);
            }
            if (entry.image != EGL_NO_IMAGE_KHR) {
                destroyImage(entry.display, entry.image);
            }
            onMain([buffer = entry.buffer] { g_object_unref(buffer); });
        }

        void releaseFrame() {
            if (frame && textureId && QOpenGLContext::currentContext()) {
                auto *functions = QOpenGLContext::currentContext()->extraFunctions();
                bool fenced = false;
                if (createSync && destroySync && duplicateFence &&
                    hasEglExtension(eglDisplay, "EGL_ANDROID_native_fence_sync")) {
                    const EGLint attributes[] = {EGL_NONE};
                    EGLSyncKHR fence = createSync(eglDisplay, EGL_SYNC_NATIVE_FENCE_ANDROID, attributes);
                    if (fence != EGL_NO_SYNC_KHR) {
                        functions->glFlush();
                        const int descriptor = duplicateFence(eglDisplay, fence);
                        destroySync(eglDisplay, fence);
                        if (descriptor >= 0) {
                            const int previous = frame->releaseFence.exchange(descriptor);
                            if (previous >= 0) {
                                close(previous);
                            }
                            fenced = true;
                        }
                    }
                }
                if (!fenced) {
                    functions->glFinish();
                }
            }
            frame.reset();
        }

        void releaseTexture() {
            releaseFrame();
            delete ownedTexture;
            ownedTexture = nullptr;
            if (textureId && QOpenGLContext::currentContext()) {
                QOpenGLContext::currentContext()->extraFunctions()->glDeleteTextures(1, &textureId);
            }
            if (eglImage != EGL_NO_IMAGE_KHR) {
                destroyImage(eglDisplay, eglImage);
            }
            textureId = 0;
            eglImage = EGL_NO_IMAGE_KHR;
        }

        static bool waitForRendering(const std::shared_ptr<WpeFrame> &next) {
            const int descriptor = next->renderingFence.exchange(-1);
            if (descriptor < 0) {
                return true;
            }
            const EGLDisplay display = eglGetCurrentDisplay();
            if (display != EGL_NO_DISPLAY && createSync && destroySync && waitSync) {
                const EGLint attributes[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, descriptor, EGL_NONE};
                EGLSyncKHR fence = createSync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, attributes);
                if (fence != EGL_NO_SYNC_KHR) {
                    const bool ready = waitSync(display, fence, 0) == EGL_TRUE ||
                                       (clientWaitSync && clientWaitSync(display, fence, 0, EGL_FOREVER_KHR) ==
                                                              EGL_CONDITION_SATISFIED_KHR);
                    destroySync(display, fence);
                    return ready;
                }
            }
            pollfd pending{descriptor, POLLIN, 0};
            int result;
            do {
                result = poll(&pending, 1, -1);
            } while (result < 0 && errno == EINTR);
            close(descriptor);
            return result > 0 && (pending.revents & POLLIN);
        }

        bool import(QQuickWindow *window, const std::shared_ptr<WpeFrame> &next, bool &created) {
            created = false;
            if (frame && frame->size != next->size) {
                markDirty(QSGNode::DirtyGeometry);
                releaseTexture();
                for (auto &entry : cache) {
                    destroyCached(entry);
                }
                cache.clear();
            }
            if (ownedTexture && frame && WPE_IS_BUFFER_DMA_BUF(frame->buffer)) {
                cache.push_back(
                    {WPE_BUFFER(g_object_ref(frame->buffer)), ownedTexture, textureId, eglDisplay, eglImage}
                );
                releaseFrame();
                ownedTexture = nullptr;
                textureId = 0;
                eglImage = EGL_NO_IMAGE_KHR;
                if (cache.size() > maximumCachedTextures) {
                    destroyCached(cache.front());
                    cache.pop_front();
                }
            } else {
                releaseTexture();
            }
            frame = next;
            if (!waitForRendering(next)) {
                return false;
            }
            for (auto entry = cache.begin(); entry != cache.end(); ++entry) {
                if (entry->buffer != next->buffer) {
                    continue;
                }
                ownedTexture = entry->texture;
                textureId = entry->textureId;
                eglDisplay = entry->display;
                eglImage = entry->image;
                g_object_unref(entry->buffer);
                cache.erase(entry);
                setTexture(ownedTexture);
                markDirty(QSGNode::DirtyMaterial);
                return true;
            }
            if (WPE_IS_BUFFER_DMA_BUF(frame->buffer)) {
                auto *context = QOpenGLContext::currentContext();
                eglDisplay = eglGetCurrentDisplay();
                if (!context || !createImage || !destroyImage || eglDisplay == EGL_NO_DISPLAY ||
                    !hasEglExtension(eglDisplay, "EGL_EXT_image_dma_buf_import")) {
                    return false;
                }
                auto *buffer = WPE_BUFFER_DMA_BUF(frame->buffer);
                const guint planes = wpe_buffer_dma_buf_get_n_planes(buffer);
                if (planes == 0 || planes > 4) {
                    return false;
                }
                const EGLint planeAttributes[4][5] = {
                    {EGL_DMA_BUF_PLANE0_FD_EXT,
                     EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                     EGL_DMA_BUF_PLANE0_PITCH_EXT,
                     EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                     EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT},
                    {EGL_DMA_BUF_PLANE1_FD_EXT,
                     EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                     EGL_DMA_BUF_PLANE1_PITCH_EXT,
                     EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
                     EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT},
                    {EGL_DMA_BUF_PLANE2_FD_EXT,
                     EGL_DMA_BUF_PLANE2_OFFSET_EXT,
                     EGL_DMA_BUF_PLANE2_PITCH_EXT,
                     EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
                     EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT},
                    {EGL_DMA_BUF_PLANE3_FD_EXT,
                     EGL_DMA_BUF_PLANE3_OFFSET_EXT,
                     EGL_DMA_BUF_PLANE3_PITCH_EXT,
                     EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
                     EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT}
                };
                QVector<EGLint> attributes{
                    EGL_WIDTH,
                    frame->size.width(),
                    EGL_HEIGHT,
                    frame->size.height(),
                    EGL_LINUX_DRM_FOURCC_EXT,
                    EGLint(wpe_buffer_dma_buf_get_format(buffer))
                };
                const quint64 modifier = wpe_buffer_dma_buf_get_modifier(buffer);
                if (modifier != DRM_FORMAT_MOD_INVALID &&
                    !hasEglExtension(eglDisplay, "EGL_EXT_image_dma_buf_import_modifiers")) {
                    return false;
                }
                for (guint plane = 0; plane < planes; ++plane) {
                    attributes << planeAttributes[plane][0] << wpe_buffer_dma_buf_get_fd(buffer, plane)
                               << planeAttributes[plane][1] << EGLint(wpe_buffer_dma_buf_get_offset(buffer, plane))
                               << planeAttributes[plane][2] << EGLint(wpe_buffer_dma_buf_get_stride(buffer, plane));
                    if (modifier != DRM_FORMAT_MOD_INVALID) {
                        attributes << planeAttributes[plane][3] << EGLint(modifier & 0xffffffff)
                                   << planeAttributes[plane][4] << EGLint(modifier >> 32);
                    }
                }
                attributes << EGL_NONE;
                eglImage =
                    createImage(eglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes.constData());
                if (eglImage == EGL_NO_IMAGE_KHR) {
                    return false;
                }
                auto *functions = context->extraFunctions();
                using ImportImage = void (*)(GLenum, void *);
                const auto importImage =
                    reinterpret_cast<ImportImage>(context->getProcAddress("glEGLImageTargetTexture2DOES"));
                if (!importImage) {
                    return false;
                }
                GLint previous = 0;
                functions->glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
                functions->glGenTextures(1, &textureId);
                functions->glBindTexture(GL_TEXTURE_2D, textureId);
                importImage(GL_TEXTURE_2D, eglImage);
                functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                functions->glBindTexture(GL_TEXTURE_2D, GLuint(previous));
                ownedTexture = QNativeInterface::QSGOpenGLTexture::fromNative(
                    textureId,
                    window,
                    frame->size,
                    QQuickWindow::TextureHasAlphaChannel
                );
            } else {
                GError *error = nullptr;
                GBytes *bytes = wpe_buffer_import_to_pixels(frame->buffer, &error);
                if (!bytes) {
                    g_clear_error(&error);
                    return false;
                }
                gsize length = 0;
                const auto *pixels = static_cast<const uchar *>(g_bytes_get_data(bytes, &length));
                const auto stride = WPE_IS_BUFFER_SHM(frame->buffer)
                                        ? wpe_buffer_shm_get_stride(WPE_BUFFER_SHM(frame->buffer))
                                        : guint(frame->size.width() * 4);
                if (stride >= quint64(frame->size.width()) * 4 && length >= quint64(stride) * frame->size.height()) {
                    const QImage image(
                        pixels,
                        frame->size.width(),
                        frame->size.height(),
                        stride,
                        QImage::Format_ARGB32_Premultiplied
                    );
                    ownedTexture = window->createTextureFromImage(image.copy());
                }
            }
            if (ownedTexture) {
                created = true;
                setTexture(ownedTexture);
                setFiltering(QSGTexture::Linear);
            }
            return ownedTexture != nullptr;
        }

        void retainLiveBuffers(const QSet<const void *> &live) {
            for (auto entry = cache.begin(); entry != cache.end();) {
                if (live.contains(entry->buffer)) {
                    ++entry;
                    continue;
                }
                destroyCached(*entry);
                entry = cache.erase(entry);
            }
        }

        static constexpr std::size_t maximumCachedTextures = 4;
        std::deque<CachedTexture> cache;
        std::shared_ptr<WpeFrame> frame;
        quint64 liveBuffersGeneration = 0;
        QSGTexture *ownedTexture = nullptr;
        GLuint textureId = 0;
        EGLDisplay eglDisplay = EGL_NO_DISPLAY;
        EGLImageKHR eglImage = EGL_NO_IMAGE_KHR;
    };

    class WpeRenderingNotice final : public QQuickPaintedItem {
      public:
        WpeRenderingNotice(QQuickItem *parent, QString text)
            : QQuickPaintedItem(parent),
              m_text(std::move(text)) {
            setZ(1000000);
            setAcceptedMouseButtons(Qt::NoButton);
            setAcceptHoverEvents(false);
            setHeight(64);
        }
        void paint(QPainter *painter) override {
            const auto palette = QGuiApplication::palette();
            painter->fillRect(boundingRect(), palette.window());
            painter->setPen(palette.windowText().color());
            painter->setFont(QGuiApplication::font());
            painter->drawText(boundingRect().adjusted(12, 8, -12, -8), Qt::AlignVCenter | Qt::TextWordWrap, m_text);
        }
        void setText(const QString &text) {
            m_text = text;
            update();
        }

      private:
        QString m_text;
    };

    void WpeSurface::reportRenderingFailure(const QString &message) {
        m_accelerated = false;
        if (m_renderingDiagnostic == message) {
            return;
        }
        m_renderingDiagnostic = message;
        if (!m_renderingNotice) {
            m_renderingNotice = new WpeRenderingNotice(this, message);
        } else {
            m_renderingNotice->setText(message);
        }
        m_renderingNotice->setWidth(width());
        m_renderingNotice->setY(qMax(0.0, height() - m_renderingNotice->height()));
        emit renderingFailed(message);
    }

    WpeScrollDelta wpeScrollDelta(const QWheelEvent &event, bool trustDeviceType) {
        const QPointingDevice *device = event.pointingDevice();
        const bool touchpad = trustDeviceType && device && device->type() == QInputDevice::DeviceType::TouchPad;
        const bool continuous = event.source() != Qt::MouseEventNotSynthesized || touchpad;
        if (event.phase() != Qt::NoScrollPhase || (continuous && !event.pixelDelta().isNull())) {
            return {QPointF(event.pixelDelta()), true};
        }
        return {QPointF(event.angleDelta()) / 120.0, false};
    }

    FrameAcknowledgement frameAcknowledgementPolicy() {
        const QString policy = qEnvironmentVariable("EDEN_WPE_FRAME_ACK");
        if (policy == QLatin1String("swap")) {
            return FrameAcknowledgement::Swap;
        }
        if (policy == QLatin1String("import")) {
            return FrameAcknowledgement::Import;
        }
        return FrameAcknowledgement::Arrival;
    }

    QString frameAcknowledgementName(FrameAcknowledgement policy) {
        switch (policy) {
        case FrameAcknowledgement::Arrival:
            return QStringLiteral("arrival");
        case FrameAcknowledgement::Import:
            return QStringLiteral("import");
        case FrameAcknowledgement::Swap:
            return QStringLiteral("swap");
        }
        return QStringLiteral("arrival");
    }

    WpeSurface::WpeSurface(WPEView *view)
        : m_view(WPE_VIEW(g_object_ref(view))),
          m_acknowledgement(frameAcknowledgementPolicy()) {
        ensureMainThreadCaller();
        setFlag(ItemHasContents);
        setFlag(ItemAcceptsInputMethod);
        setAcceptedMouseButtons(Qt::AllButtons);
        setAcceptHoverEvents(true);
        setAcceptTouchEvents(true);
        setActiveFocusOnTab(true);
        bindWpeSurface(m_view, this);
        g_signal_connect(
            m_view,
            "closed",
            G_CALLBACK(+[](WPEView *, WpeSurface *surface) { emit surface->nativeClosed(); }),
            this
        );
        g_signal_connect(
            m_view,
            "resized",
            G_CALLBACK(+[](WPEView *, WpeSurface *surface) { surface->synchronizeGeometry(); }),
            this
        );
        g_signal_connect(
            m_view,
            "notify::toplevel",
            G_CALLBACK(+[](WPEView *, GParamSpec *, WpeSurface *surface) {
                surface->synchronizeGeometry();
                surface->synchronizeActivation();
                surface->synchronizeVisibility();
            }),
            this
        );
        connect(this, &QQuickItem::visibleChanged, this, &WpeSurface::synchronizeVisibility);
        connect(this, &QQuickItem::enabledChanged, this, &WpeSurface::synchronizeVisibility);
        synchronizeVisibility();
    }

    WpeSurface::~WpeSurface() {
        bindWpeSurface(m_view, nullptr);
        g_signal_handlers_disconnect_by_data(m_view, this);
        wpe_view_focus_out(m_view);
        wpe_view_set_visible(m_view, FALSE);
        wpe_view_unmap(m_view);
        m_frame.reset();
        g_object_unref(m_view);
    }

    void WpeSurface::attach(QQuickItem *viewport) {
        for (const auto &connection : m_viewportConnections) {
            disconnect(connection);
        }
        m_viewportConnections.clear();
        m_viewport = viewport;
        setParentItem(viewport);
        if (viewport) {
            m_viewportConnections.append(
                connect(viewport, &QQuickItem::widthChanged, this, &WpeSurface::synchronizeGeometry)
            );
            m_viewportConnections.append(
                connect(viewport, &QQuickItem::heightChanged, this, &WpeSurface::synchronizeGeometry)
            );
            m_viewportConnections.append(connect(viewport, &QObject::destroyed, this, [this] { attach(nullptr); }));
        }
        synchronizeGeometry();
        synchronizeVisibility();
    }

    void WpeSurface::synchronizeGeometry() {
        if (m_viewport) {
            setSize(m_viewport->size());
        }
        if (m_renderingNotice) {
            m_renderingNotice->setWidth(width());
            m_renderingNotice->setY(qMax(0.0, height() - m_renderingNotice->height()));
        }
        const double scale = window() ? window()->effectiveDevicePixelRatio() : 1.0;
        if (auto *toplevel = wpe_view_get_toplevel(m_view)) {
            wpe_toplevel_scale_changed(toplevel, scale);
            wpe_toplevel_resized(toplevel, qMax(1, int(width())), qMax(1, int(height())));
        }
        wpe_view_resized(m_view, qMax(1, int(width())), qMax(1, int(height())));
        WpeRuntime::synchronizeScreen(m_view, window(), wpe_view_get_visible(m_view));
    }

    void WpeSurface::synchronizeActivation() {
        if (auto *toplevel = wpe_view_get_toplevel(m_view)) {
            auto state = unsigned(wpe_toplevel_get_state(toplevel));
            state = m_window && m_window->isActive() ? state | WPE_TOPLEVEL_STATE_ACTIVE
                                                     : state & ~WPE_TOPLEVEL_STATE_ACTIVE;
            wpe_toplevel_state_changed(toplevel, WPEToplevelState(state));
        }
    }

    void WpeSurface::synchronizeVisibility() {
        const bool visible = window() && window()->isVisible() && window()->visibility() != QWindow::Minimized &&
                             isVisible() && isEnabled() && width() > 0 && height() > 0;
        WpeRuntime::synchronizeScreen(m_view, window(), visible);
        wpe_view_set_visible(m_view, visible);
        if (visible) {
            wpe_view_map(m_view);
        } else {
            wpe_view_focus_out(m_view);
            wpe_view_unmap(m_view);
            if (m_frame) {
                m_frame->rendered();
            }
        }
    }

    bool WpeSurface::submit(WPEBuffer *buffer) {
        auto frame = std::make_shared<WpeFrame>(m_view, buffer);
        if (!wpe_view_get_mapped(m_view)) {
            frame->rendered();
            return true;
        }
        if (m_acknowledgement == FrameAcknowledgement::Arrival) {
            frame->rendered();
        }
        m_frame = std::move(frame);
        const bool accelerated = WPE_IS_BUFFER_DMA_BUF(buffer);
        if (!accelerated && m_accelerated) {
            reportRenderingFailure(
                QStringLiteral("WebKit is using software rendering. GPU acceleration is unavailable.")
            );
        }
        update();
        return true;
    }

    void WpeSurface::buffersChanged(WPEBuffer **buffers, guint count) {
        m_liveBuffers.clear();
        for (guint index = 0; index < count; ++index) {
            m_liveBuffers.insert(buffers[index]);
        }
        ++m_liveBuffersGeneration;
        update();
    }

    QSGNode *WpeSurface::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) {
        auto *node = static_cast<WpeTextureNode *>(oldNode);
        if (!m_frame) {
            delete node;
            return nullptr;
        }
        if (!node) {
            node = new WpeTextureNode;
        }
        if (node->liveBuffersGeneration != m_liveBuffersGeneration) {
            node->retainLiveBuffers(m_liveBuffers);
            node->liveBuffersGeneration = m_liveBuffersGeneration;
        }
        if (node->frame != m_frame) {
            bool created = false;
            if (!node->import(window(), m_frame, created)) {
                const QPointer<WpeSurface> guard(this);
                QMetaObject::invokeMethod(
                    this,
                    [guard] {
                        if (guard) {
                            guard->reportRenderingFailure(QStringLiteral(
                                "WebKit cannot display GPU frames in this window. Restart Eden with EGL OpenGL "
                                "rendering."
                            ));
                        }
                    },
                    Qt::QueuedConnection
                );
                m_frame->rendered();
                delete node;
                return nullptr;
            }
            if (created) {
                ++m_textureImports;
            }
            ++m_frameImports;
            if (m_acknowledgement != FrameAcknowledgement::Swap) {
                m_frame->rendered();
            } else {
                const auto frame = m_frame;
                QObject::connect(
                    window(),
                    &QQuickWindow::frameSwapped,
                    this,
                    [frame] { frame->rendered(); },
                    Qt::ConnectionType(Qt::QueuedConnection | Qt::SingleShotConnection)
                );
            }
        }
        const QSizeF logicalSize = QSizeF(m_frame->size) / m_frame->scale;
        node->setRect(
            QRectF(QPointF(), QSizeF(qMin(width(), logicalSize.width()), qMin(height(), logicalSize.height())))
        );
        node->setSourceRect(QRectF(QPointF(), node->rect().size() * m_frame->scale));
        return node;
    }

    void WpeSurface::geometryChange(const QRectF &next, const QRectF &previous) {
        QQuickItem::geometryChange(next, previous);
        synchronizeGeometry();
        synchronizeVisibility();
    }

    void WpeSurface::itemChange(ItemChange change, const ItemChangeData &data) {
        QQuickItem::itemChange(change, data);
        if (change == ItemSceneChange) {
            for (const auto &connection : m_windowConnections) {
                disconnect(connection);
            }
            m_windowConnections.clear();
            m_window = data.window;
            if (m_window) {
                m_windowConnections.append(
                    connect(m_window, &QWindow::visibilityChanged, this, &WpeSurface::synchronizeVisibility)
                );
                m_windowConnections.append(
                    connect(m_window, &QWindow::activeChanged, this, &WpeSurface::synchronizeActivation)
                );
                m_windowConnections.append(
                    connect(m_window, &QWindow::screenChanged, this, &WpeSurface::synchronizeGeometry)
                );
            }
            synchronizeGeometry();
            synchronizeVisibility();
        } else if (change == ItemDevicePixelRatioHasChanged) {
            synchronizeGeometry();
        }
    }

    static WPEModifiers modifiers(Qt::KeyboardModifiers keys) {
        unsigned result = 0;
        if (keys & Qt::ControlModifier) {
            result |= WPE_MODIFIER_KEYBOARD_CONTROL;
        }
        if (keys & Qt::ShiftModifier) {
            result |= WPE_MODIFIER_KEYBOARD_SHIFT;
        }
        if (keys & Qt::AltModifier) {
            result |= WPE_MODIFIER_KEYBOARD_ALT;
        }
        if (keys & Qt::MetaModifier) {
            result |= WPE_MODIFIER_KEYBOARD_META;
        }
        const auto buttons = QGuiApplication::mouseButtons();
        if (buttons & Qt::LeftButton) {
            result |= WPE_MODIFIER_POINTER_BUTTON1;
        }
        if (buttons & Qt::MiddleButton) {
            result |= WPE_MODIFIER_POINTER_BUTTON2;
        }
        if (buttons & Qt::RightButton) {
            result |= WPE_MODIFIER_POINTER_BUTTON3;
        }
        return WPEModifiers(result);
    }

    void WpeSurface::dispatch(WPEEvent *event) {
        wpe_view_event(m_view, event);
        wpe_event_unref(event);
    }

    void WpeSurface::pointerButton(QMouseEvent *event, bool pressed) {
        m_pointer = event->position();
        if (pressed) {
            forceActiveFocus(Qt::MouseFocusReason);
        }
        guint button = event->button() == Qt::LeftButton     ? WPE_BUTTON_PRIMARY
                       : event->button() == Qt::MiddleButton ? WPE_BUTTON_MIDDLE
                                                             : WPE_BUTTON_SECONDARY;
        if (event->button() == Qt::BackButton || event->button() == Qt::ForwardButton) {
            if (pressed) {
                emit shortcutRequested(event->button() == Qt::BackButton ? "back" : "forward");
            }
            event->accept();
            return;
        }
        const guint count =
            pressed ? wpe_view_compute_press_count(m_view, m_pointer.x(), m_pointer.y(), button, event->timestamp())
                    : 0;
        dispatch(wpe_event_pointer_button_new(
            pressed ? WPE_EVENT_POINTER_DOWN : WPE_EVENT_POINTER_UP,
            m_view,
            WPE_INPUT_SOURCE_MOUSE,
            event->timestamp(),
            modifiers(event->modifiers()),
            button,
            m_pointer.x(),
            m_pointer.y(),
            count
        ));
        event->accept();
    }

    void WpeSurface::pointerMotion(
        const QPointF &position,
        unsigned long time,
        Qt::KeyboardModifiers keys,
        WPEEventType type
    ) {
        const QPointF delta = position - m_pointer;
        m_pointer = position;
        dispatch(wpe_event_pointer_move_new(
            type,
            m_view,
            WPE_INPUT_SOURCE_MOUSE,
            time,
            modifiers(keys),
            position.x(),
            position.y(),
            delta.x(),
            delta.y()
        ));
    }

    void WpeSurface::mousePressEvent(QMouseEvent *event) {
        pointerButton(event, true);
    }
    void WpeSurface::mouseDoubleClickEvent(QMouseEvent *event) {
        pointerButton(event, true);
    }
    void WpeSurface::mouseReleaseEvent(QMouseEvent *event) {
        pointerButton(event, false);
    }
    void WpeSurface::mouseMoveEvent(QMouseEvent *event) {
        pointerMotion(event->position(), event->timestamp(), event->modifiers(), WPE_EVENT_POINTER_MOVE);
        event->accept();
    }
    void WpeSurface::hoverMoveEvent(QHoverEvent *event) {
        pointerMotion(event->position(), event->timestamp(), event->modifiers(), WPE_EVENT_POINTER_MOVE);
        event->accept();
    }
    void WpeSurface::hoverEnterEvent(QHoverEvent *event) {
        pointerMotion(event->position(), event->timestamp(), event->modifiers(), WPE_EVENT_POINTER_ENTER);
        event->accept();
    }
    void WpeSurface::hoverLeaveEvent(QHoverEvent *event) {
        pointerMotion(m_pointer, event->timestamp(), event->modifiers(), WPE_EVENT_POINTER_LEAVE);
        event->accept();
    }

    void WpeSurface::wheelEvent(QWheelEvent *event) {
        static const bool trustDeviceType = QGuiApplication::platformName() != QLatin1String("wayland");
        const WpeScrollDelta scroll = wpeScrollDelta(*event, trustDeviceType);
        dispatch(wpe_event_scroll_new(
            m_view,
            scroll.precise ? WPE_INPUT_SOURCE_TOUCHPAD : WPE_INPUT_SOURCE_MOUSE,
            event->timestamp(),
            modifiers(event->modifiers()),
            scroll.delta.x(),
            scroll.delta.y(),
            scroll.precise,
            event->phase() == Qt::ScrollEnd,
            event->position().x(),
            event->position().y()
        ));
        event->accept();
    }

    void WpeSurface::keyEvent(QKeyEvent *event, bool pressed) {
        const QString command = core::ShortcutRegistry::commandFor(event);
        if (!command.isEmpty()) {
            event->accept();
            if (pressed) {
                emit shortcutRequested(command);
            }
            return;
        }
        if (event->text().toUcs4().size() > 1 &&
            !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && m_input &&
            m_inputFocused) {
            if (pressed) {
                g_signal_emit_by_name(m_input, "committed", event->text().toUtf8().constData());
            }
            event->accept();
            return;
        }
        quint32 key = event->nativeVirtualKey();
        if (!key) {
            switch (event->key()) {
            case Qt::Key_Backspace:
                key = XKB_KEY_BackSpace;
                break;
            case Qt::Key_Tab:
                key = XKB_KEY_Tab;
                break;
            case Qt::Key_Backtab:
                key = XKB_KEY_ISO_Left_Tab;
                break;
            case Qt::Key_Return:
            case Qt::Key_Enter:
                key = XKB_KEY_Return;
                break;
            case Qt::Key_Escape:
                key = XKB_KEY_Escape;
                break;
            case Qt::Key_Insert:
                key = XKB_KEY_Insert;
                break;
            case Qt::Key_Menu:
                key = XKB_KEY_Menu;
                break;
            case Qt::Key_Delete:
                key = XKB_KEY_Delete;
                break;
            case Qt::Key_Left:
                key = XKB_KEY_Left;
                break;
            case Qt::Key_Right:
                key = XKB_KEY_Right;
                break;
            case Qt::Key_Up:
                key = XKB_KEY_Up;
                break;
            case Qt::Key_Down:
                key = XKB_KEY_Down;
                break;
            case Qt::Key_Home:
                key = XKB_KEY_Home;
                break;
            case Qt::Key_End:
                key = XKB_KEY_End;
                break;
            case Qt::Key_PageUp:
                key = XKB_KEY_Page_Up;
                break;
            case Qt::Key_PageDown:
                key = XKB_KEY_Page_Down;
                break;
            default:
                if (event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F35) {
                    key = XKB_KEY_F1 + event->key() - Qt::Key_F1;
                } else if (!event->text().isEmpty()) {
                    key = xkb_utf32_to_keysym(event->text().toUcs4().first());
                } else if (event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z) {
                    key = XKB_KEY_a + event->key() - Qt::Key_A;
                }
                break;
            }
        }
        if (!key) {
            event->ignore();
            return;
        }
        dispatch(wpe_event_keyboard_new(
            pressed ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP,
            m_view,
            WPE_INPUT_SOURCE_KEYBOARD,
            event->timestamp(),
            modifiers(event->modifiers()),
            event->nativeScanCode(),
            key
        ));
        event->accept();
    }

    void WpeSurface::keyPressEvent(QKeyEvent *event) {
        keyEvent(event, true);
    }
    void WpeSurface::keyReleaseEvent(QKeyEvent *event) {
        keyEvent(event, false);
    }
    void WpeSurface::focusInEvent(QFocusEvent *event) {
        QQuickItem::focusInEvent(event);
        wpe_view_focus_in(m_view);
    }
    void WpeSurface::focusOutEvent(QFocusEvent *event) {
        resetInput();
        wpe_view_focus_out(m_view);
        QQuickItem::focusOutEvent(event);
    }

    void WpeSurface::inputMethodEvent(QInputMethodEvent *event) {
        if (!m_input || !m_inputFocused || !hasActiveFocus()) {
            event->ignore();
            return;
        }
        if (event->replacementLength() != 0 || event->replacementStart() != 0) {
            const int start = qBound(0, m_cursorPosition + event->replacementStart(), int(m_surrounding.size()));
            const int end = qBound(start, start + qMax(0, event->replacementLength()), int(m_surrounding.size()));
            g_signal_emit_by_name(m_input, "delete-surrounding", start - m_cursorPosition, guint(end - start));
        }
        const QString preedit = event->preeditString();
        if (!m_composing && !preedit.isEmpty()) {
            m_composing = true;
            g_signal_emit_by_name(m_input, "preedit-started");
        }
        m_preedit = preedit;
        m_preeditCursor = preedit.size();
        for (const auto &attribute : event->attributes()) {
            if (attribute.type == QInputMethodEvent::Cursor) {
                m_preeditCursor = qBound(0, attribute.start, int(preedit.size()));
            }
        }
        if (m_composing) {
            g_signal_emit_by_name(m_input, "preedit-changed");
        }
        if (!event->commitString().isEmpty()) {
            g_signal_emit_by_name(m_input, "committed", event->commitString().toUtf8().constData());
        }
        if (m_composing && preedit.isEmpty()) {
            m_composing = false;
            g_signal_emit_by_name(m_input, "preedit-finished");
        }
        event->accept();
    }

    void WpeSurface::touchEvent(QTouchEvent *event) {
        for (const QEventPoint &point : event->points()) {
            WPEEventType type = WPE_EVENT_TOUCH_MOVE;
            if (event->type() == QEvent::TouchCancel) {
                type = WPE_EVENT_TOUCH_CANCEL;
            } else if (point.state() == QEventPoint::State::Pressed) {
                type = WPE_EVENT_TOUCH_DOWN;
                forceActiveFocus(Qt::MouseFocusReason);
            } else if (point.state() == QEventPoint::State::Released) {
                type = WPE_EVENT_TOUCH_UP;
            } else if (point.state() == QEventPoint::State::Stationary) {
                continue;
            }
            dispatch(wpe_event_touch_new(
                type,
                m_view,
                WPE_INPUT_SOURCE_TOUCHSCREEN,
                event->timestamp(),
                modifiers(event->modifiers()),
                point.id(),
                point.position().x(),
                point.position().y()
            ));
        }
        event->accept();
    }

    void WpeSurface::setPointerCursor(const QString &name) {
        static const QHash<QString, Qt::CursorShape> cursors{
            {"pointer", Qt::PointingHandCursor},
            {"text", Qt::IBeamCursor},
            {"wait", Qt::WaitCursor},
            {"progress", Qt::BusyCursor},
            {"crosshair", Qt::CrossCursor},
            {"move", Qt::SizeAllCursor},
            {"grab", Qt::OpenHandCursor},
            {"grabbing", Qt::ClosedHandCursor},
            {"not-allowed", Qt::ForbiddenCursor},
            {"col-resize", Qt::SplitHCursor},
            {"row-resize", Qt::SplitVCursor},
            {"ew-resize", Qt::SizeHorCursor},
            {"ns-resize", Qt::SizeVerCursor},
            {"nesw-resize", Qt::SizeBDiagCursor},
            {"nwse-resize", Qt::SizeFDiagCursor},
            {"none", Qt::BlankCursor}
        };
        setCursor(cursors.value(name, Qt::ArrowCursor));
    }

    void WpeSurface::setPointerCursor(const QImage &image, const QPoint &hotspot) {
        setCursor(QCursor(QPixmap::fromImage(image), hotspot.x(), hotspot.y()));
    }
    void WpeSurface::setInputContext(WPEInputMethodContext *context) {
        m_input = context;
    }
    void WpeSurface::setInputFocus(bool focused) {
        m_inputFocused = focused;
        if (!focused) {
            resetInput();
            m_surrounding.clear();
            m_surroundingBytes.clear();
            m_surroundingValid = false;
        }
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
    }
    void WpeSurface::setInputCursor(const QRect &rectangle) {
        m_cursor = rectangle;
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
    }
    void WpeSurface::setInputSurrounding(const char *text, unsigned length, unsigned cursor, unsigned anchor) {
        const QByteArrayView bytes(text, qsizetype(length));
        cursor = qMin(cursor, length);
        anchor = qMin(anchor, length);
        const bool textChanged = !m_surroundingValid || bytes != QByteArrayView(m_surroundingBytes);
        if (!textChanged && m_surroundingCursorByte == cursor && m_surroundingAnchorByte == anchor) {
            return;
        }
        int cursorUnits = 0;
        int anchorUnits = 0;
        int units = 0;
        for (unsigned index = 0; index < length; ++index) {
            if (index == cursor) {
                cursorUnits = units;
            }
            if (index == anchor) {
                anchorUnits = units;
            }
            const auto byte = static_cast<unsigned char>(text[index]);
            if ((byte & 0xc0) != 0x80) {
                units += byte >= 0xf0 ? 2 : 1;
            }
        }
        if (cursor == length) {
            cursorUnits = units;
        }
        if (anchor == length) {
            anchorUnits = units;
        }
        if (textChanged) {
            m_surroundingBytes = bytes.toByteArray();
            m_surrounding = QString::fromUtf8(bytes);
        }
        m_surroundingValid = true;
        m_surroundingCursorByte = cursor;
        m_surroundingAnchorByte = anchor;
        m_cursorPosition = qMin(cursorUnits, int(m_surrounding.size()));
        m_anchorPosition = qMin(anchorUnits, int(m_surrounding.size()));
        QGuiApplication::inputMethod()->update(Qt::ImQueryInput);
    }
    void WpeSurface::setInputPreedit(const QString &text, int cursor) {
        m_preedit = text;
        m_preeditCursor = cursor;
    }
    QString WpeSurface::inputPreedit() const {
        return m_preedit;
    }
    int WpeSurface::inputPreeditCursor() const {
        return m_preeditCursor;
    }
    void WpeSurface::resetInput() {
        m_preedit.clear();
        if (m_composing && m_input) {
            m_composing = false;
            g_signal_emit_by_name(m_input, "preedit-changed");
            g_signal_emit_by_name(m_input, "preedit-finished");
        }
    }

    QVariant WpeSurface::inputMethodQuery(Qt::InputMethodQuery query) const {
        switch (query) {
        case Qt::ImEnabled:
            return m_inputFocused;
        case Qt::ImCursorRectangle:
            return m_cursor;
        case Qt::ImSurroundingText:
            return m_surrounding;
        case Qt::ImCursorPosition:
            return m_cursorPosition;
        case Qt::ImAnchorPosition:
            return m_anchorPosition;
        case Qt::ImCurrentSelection:
            return m_surrounding.mid(
                qMin(m_cursorPosition, m_anchorPosition),
                qAbs(m_cursorPosition - m_anchorPosition)
            );
        case Qt::ImHints:
            return m_input && wpe_input_method_context_get_input_purpose(m_input) == WPE_INPUT_PURPOSE_PASSWORD
                       ? int(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText)
                       : int(Qt::ImhNone);
        default:
            return QQuickItem::inputMethodQuery(query);
        }
    }

    WPEView *WpeSurface::nativeView() const {
        return m_view;
    }
    QPoint WpeSurface::pointerPosition() const {
        return m_pointer.toPoint();
    }
    bool WpeSurface::accelerated() const {
        return m_accelerated;
    }
    QString WpeSurface::renderingDiagnostic() const {
        return m_renderingDiagnostic;
    }
    quint64 WpeSurface::textureImportCount() const {
        return m_textureImports.load();
    }
    quint64 WpeSurface::frameImportCount() const {
        return m_frameImports.load();
    }

}

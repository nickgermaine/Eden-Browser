from wpe.patch_support import replace_once


GRAPHICS = "Source/WebCore/platform/graphics/"
DMABUF_BUFFER = GRAPHICS + "DMABufBuffer.h"
VIDEO_FRAME_API = "Source/WebCore/platform/VideoFrame.h"
VIDEO_FRAME = GRAPHICS + "gstreamer/VideoFrameGStreamer.cpp"
GBM_CONTEXT_HEADER = GRAPHICS + "gbm/GraphicsContextGLTextureMapperGBM.h"
GBM_CONTEXT = GRAPHICS + "gbm/GraphicsContextGLTextureMapperGBM.cpp"
TEXTURE_MAPPER_CONTEXT_HEADER = GRAPHICS + "texmap/GraphicsContextGLTextureMapperANGLE.h"
TEXTURE_MAPPER_CONTEXT = GRAPHICS + "texmap/GraphicsContextGLTextureMapperANGLE.cpp"
REMOTE_GL_MESSAGES = "Source/WebKit/GPUProcess/graphics/RemoteGraphicsContextGL.messages.in"
REMOTE_GL_HEADER = "Source/WebKit/GPUProcess/graphics/RemoteGraphicsContextGL.h"
REMOTE_GL_GBM = "Source/WebKit/GPUProcess/graphics/RemoteGraphicsContextGLGBM.cpp"
REMOTE_GL_PROXY_HEADER = "Source/WebKit/WebProcess/GPU/graphics/RemoteGraphicsContextGLProxy.h"
REMOTE_GL_PROXY_GBM = "Source/WebKit/WebProcess/GPU/graphics/gbm/RemoteGraphicsContextGLProxyGBM.cpp"
MEDIA_STREAM_SOURCE = "Source/WebCore/platform/mediastream/gstreamer/GStreamerMediaStreamSource.cpp"
VIDEO_PACKETIZER = "Source/WebCore/platform/mediastream/gstreamer/GStreamerVideoRTPPacketizer.cpp"
VIDEO_ENCODER_BIN = "Source/WebCore/platform/gstreamer/VideoEncoderPrivateGStreamer.cpp"
VIDEO_ENCODER_BIN_HEADER = "Source/WebCore/platform/gstreamer/VideoEncoderPrivateGStreamer.h"
VIDEO_ENCODER = GRAPHICS + "gstreamer/VideoEncoderGStreamer.cpp"
VIDEO_ENCODER_HEADER = GRAPHICS + "gstreamer/VideoEncoderGStreamer.h"


RELEASE_QUEUE = r"""
class DMABufBufferReleaseQueue final : public ThreadSafeRefCounted<DMABufBufferReleaseQueue> {
public:
    static Ref<DMABufBufferReleaseQueue> create() { return adoptRef(*new DMABufBufferReleaseQueue); }

    void add(uint64_t bufferID)
    {
        Locker locker { m_lock };
        m_releasedBufferIDs.append(bufferID);
    }

    Vector<uint64_t> take()
    {
        Locker locker { m_lock };
        return std::exchange(m_releasedBufferIDs, { });
    }

private:
    DMABufBufferReleaseQueue() = default;

    Lock m_lock;
    Vector<uint64_t> m_releasedBufferIDs WTF_GUARDED_BY_LOCK(m_lock);
};
"""


def dmabuf_buffer_patch(text):
    text = replace_once(
        text,
        '#include "DMABufBufferAttributes.h"\n#include <wtf/ThreadSafeRefCounted.h>\n',
        '#include "DMABufBufferAttributes.h"\n#include <wtf/Lock.h>\n#include <wtf/ThreadSafeRefCounted.h>\n',
    )
    return replace_once(
        text,
        "    std::unique_ptr<CoordinatedPlatformLayerBuffer> m_buffer;\n};\n",
        "    std::unique_ptr<CoordinatedPlatformLayerBuffer> m_buffer;\n};\n" + RELEASE_QUEUE,
    )


def video_frame_api_patch(text):
    text = replace_once(
        text,
        "#if USE(AVFOUNDATION) && PLATFORM(COCOA)\nclass VideoFrameCV;\n#endif\n",
        "#if USE(AVFOUNDATION) && PLATFORM(COCOA)\nclass VideoFrameCV;\n#endif\n"
        "#if USE(GSTREAMER) && USE(GBM)\nclass DMABufBuffer;\nclass DMABufBufferReleaseQueue;\n#endif\n",
    )
    return replace_once(
        text,
        "    static RefPtr<VideoFrame> createI420A(std::span<const uint8_t>, size_t width, size_t height, const ComputedPlaneLayout&, const ComputedPlaneLayout&, const ComputedPlaneLayout&, const ComputedPlaneLayout&, PlatformVideoColorSpace&&);\n",
        "    static RefPtr<VideoFrame> createI420A(std::span<const uint8_t>, size_t width, size_t height, const ComputedPlaneLayout&, const ComputedPlaneLayout&, const ComputedPlaneLayout&, const ComputedPlaneLayout&, PlatformVideoColorSpace&&);\n"
        "#if USE(GSTREAMER) && USE(GBM)\n"
        "    WEBCORE_EXPORT static RefPtr<VideoFrame> createFromDMABufBuffer(DMABufBuffer&, DMABufBufferReleaseQueue&);\n"
        "    WEBCORE_EXPORT static Vector<uint64_t> preferredDMABufModifiers(bool hasAlpha);\n"
        "#endif\n",
    )


DMABUF_VIDEO_FRAME = r"""#if USE(GBM)
struct DMABufVideoFrameRelease {
    Ref<DMABufBufferReleaseQueue> queue;
    uint64_t bufferID { 0 };
};

static GstAllocator* dmabufVideoFrameAllocator()
{
    static GstAllocator* allocator = [] {
        auto* allocator = gst_dmabuf_allocator_new();
        GST_OBJECT_FLAG_SET(allocator, GST_OBJECT_FLAG_MAY_BE_LEAKED);
        return allocator;
    }();
    return allocator;
}

Vector<uint64_t> VideoFrame::preferredDMABufModifiers(bool hasAlpha)
{
#if GST_CHECK_VERSION(1, 24, 0)
    static std::once_flag onceFlag;
    static NeverDestroyed<std::array<Vector<uint64_t>, 2>> modifiers;
    std::call_once(onceFlag, [] {
        ensureVideoFrameDebugCategoryInitialized();
        GRefPtr factory = adoptGRef(gst_element_factory_find("vapostproc"));
        if (!factory)
            return;
        auto addModifier = [](const char* drmFormat) {
            guint64 modifier = DRM_FORMAT_MOD_INVALID;
            auto fourcc = drmFormat ? gst_video_dma_drm_fourcc_from_string(drmFormat, &modifier) : 0;
            if (modifier == DRM_FORMAT_MOD_LINEAR || modifier == DRM_FORMAT_MOD_INVALID)
                return;
            if (fourcc == DRM_FORMAT_ARGB8888 && !modifiers.get()[1].contains(modifier))
                modifiers.get()[1].append(modifier);
            if (fourcc == DRM_FORMAT_XRGB8888 && !modifiers.get()[0].contains(modifier))
                modifiers.get()[0].append(modifier);
        };
        for (auto* item = gst_element_factory_get_static_pad_templates(factory.get()); item; item = item->next) {
            auto* padTemplate = static_cast<GstStaticPadTemplate*>(item->data);
            if (padTemplate->direction != GST_PAD_SINK)
                continue;
            GRefPtr caps = adoptGRef(gst_static_caps_get(&padTemplate->static_caps));
            for (unsigned index = 0; index < gst_caps_get_size(caps.get()); ++index) {
                auto* features = gst_caps_get_features(caps.get(), index);
                if (!features || !gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_DMABUF))
                    continue;
                auto* value = gst_structure_get_value(gst_caps_get_structure(caps.get(), index), "drm-format");
                if (value && G_VALUE_HOLDS_STRING(value))
                    addModifier(g_value_get_string(value));
                else if (value && GST_VALUE_HOLDS_LIST(value)) {
                    for (unsigned entry = 0; entry < gst_value_list_get_size(value); ++entry) {
                        auto* format = gst_value_list_get_value(value, entry);
                        if (G_VALUE_HOLDS_STRING(format))
                            addModifier(g_value_get_string(format));
                    }
                }
            }
        }
        GST_DEBUG("The video post-processor imports %zu opaque and %zu translucent DMA-BUF layouts", modifiers.get()[0].size(), modifiers.get()[1].size());
    });
    return modifiers.get()[hasAlpha ? 1 : 0];
#else
    UNUSED_PARAM(hasAlpha);
    return { };
#endif
}

RefPtr<VideoFrame> VideoFrame::createFromDMABufBuffer(DMABufBuffer& dmabuf, DMABufBufferReleaseQueue& releaseQueue)
{
#if GST_CHECK_VERSION(1, 24, 0)
    ensureVideoFrameDebugCategoryInitialized();
    const auto& attributes = dmabuf.attributes();
    if (attributes.size.isEmpty() || attributes.fds.size() != 1 || attributes.offsets.size() != 1 || attributes.strides.size() != 1)
        return nullptr;

    GstVideoFormat format;
    switch (attributes.fourcc.value) {
    case DRM_FORMAT_ARGB8888:
        format = GST_VIDEO_FORMAT_BGRA;
        break;
    case DRM_FORMAT_XRGB8888:
        format = GST_VIDEO_FORMAT_BGRx;
        break;
    default:
        GST_WARNING("DMA-BUF video frames in format %" GST_FOURCC_FORMAT " are not supported", GST_FOURCC_ARGS(attributes.fourcc.value));
        return nullptr;
    }

    int width = attributes.size.width();
    int height = attributes.size.height();
    GstVideoInfo info;
    if (!gst_video_info_set_format(&info, format, width, height))
        return nullptr;
    GST_VIDEO_INFO_FPS_N(&info) = 30;
    GST_VIDEO_INFO_FPS_D(&info) = 1;
    GstVideoInfoDmaDrm drmInfo;
    if (!gst_video_info_dma_drm_from_video_info(&drmInfo, &info, attributes.modifier))
        return nullptr;
    GRefPtr caps = adoptGRef(gst_video_info_dma_drm_to_caps(&drmInfo));
    if (!caps)
        return nullptr;

    auto fd = attributes.fds[0].duplicate();
    if (!fd)
        return nullptr;
    gsize requiredSize = static_cast<gsize>(attributes.offsets[0]) + static_cast<gsize>(attributes.strides[0]) * height;
    auto end = lseek(fd.value(), 0, SEEK_END);
    if (end > 0 && static_cast<gsize>(end) < requiredSize) {
        GST_WARNING("A %dx%d DMA-BUF video frame needs %" G_GSIZE_FORMAT " bytes but the buffer holds %" G_GINT64_FORMAT, width, height, requiredSize, static_cast<gint64>(end));
        return nullptr;
    }
    auto* memory = gst_dmabuf_allocator_alloc(dmabufVideoFrameAllocator(), fd.value(), end > 0 ? static_cast<gsize>(end) : requiredSize);
    if (!memory)
        return nullptr;
    [[maybe_unused]] int memoryFD = fd.release();
    GST_MINI_OBJECT_FLAG_SET(memory, GST_MEMORY_FLAG_READONLY);

    static GQuark releaseQuark = g_quark_from_static_string("webkit-dmabuf-video-frame-release");
    gst_mini_object_set_qdata(GST_MINI_OBJECT_CAST(memory), releaseQuark, new DMABufVideoFrameRelease { releaseQueue, dmabuf.id() }, [](gpointer data) {
        std::unique_ptr<DMABufVideoFrameRelease> release(static_cast<DMABufVideoFrameRelease*>(data));
        release->queue->add(release->bufferID);
    });
    static GQuark dmabufQuark = g_quark_from_static_string("wk-dmabuf-buffer");
    dmabuf.ref();
    gst_mini_object_set_qdata(GST_MINI_OBJECT_CAST(memory), dmabufQuark, &dmabuf, [](gpointer data) {
        static_cast<DMABufBuffer*>(data)->deref();
    });

    GRefPtr buffer = adoptGRef(gst_buffer_new());
    gst_buffer_append_memory(buffer.get(), memory);
    gsize offsets[GST_VIDEO_MAX_PLANES] = { attributes.offsets[0] };
    gint strides[GST_VIDEO_MAX_PLANES] = { static_cast<gint>(attributes.strides[0]) };
    gst_buffer_add_video_meta_full(buffer.get(), GST_VIDEO_FRAME_FLAG_NONE, format, width, height, 1, offsets, strides);
    setBufferFields(buffer.get(), MediaTime::invalidTime(), 30);
    GST_DEBUG("Wrapped DMA-BUF pool buffer %" G_GUINT64_FORMAT " as a %dx%d %s video frame with modifier 0x%" G_GINT64_MODIFIER "x", dmabuf.id(), width, height, gst_video_format_to_string(format), attributes.modifier);

    GRefPtr sample = adoptGRef(gst_sample_new(buffer.get(), caps.get(), nullptr, nullptr));
    VideoFrameGStreamer::CreateOptions options(IntSize(width, height), VideoFrameGStreamer::infoFromCaps(caps));
    return VideoFrameGStreamer::create(WTF::move(sample), options);
#else
    UNUSED_PARAM(dmabuf);
    UNUSED_PARAM(releaseQueue);
    return nullptr;
#endif
}
#endif

"""


def video_frame_patch(text):
    text = replace_once(
        text,
        "#if USE(GBM)\n#include <drm_fourcc.h>\n",
        "#if USE(GBM)\n#include <drm_fourcc.h>\n#include <gst/allocators/gstdmabuf.h>\n#include <unistd.h>\n#include <wtf/NeverDestroyed.h>\n",
    )
    return replace_once(
        text,
        "VideoFrameGStreamer::VideoFrameGStreamer(GRefPtr<GstSample>&& sample, const CreateOptions& options, PlatformVideoColorSpace&& colorSpace)\n",
        DMABUF_VIDEO_FRAME + "VideoFrameGStreamer::VideoFrameGStreamer(GRefPtr<GstSample>&& sample, const CreateOptions& options, PlatformVideoColorSpace&& colorSpace)\n",
    )


def gbm_context_header_patch(text):
    text = replace_once(
        text,
        '#include "GraphicsContextGLTextureMapperANGLE.h"\n#include "GraphicsLayerContentsDisplayDelegate.h"\n',
        '#include "DMABufBuffer.h"\n#include "GraphicsContextGLTextureMapperANGLE.h"\n#include "GraphicsLayerContentsDisplayDelegate.h"\n',
    )
    text = replace_once(
        text,
        "    DMABufBuffer* displayBuffer() { return m_displayBuffer.dmabuf.get(); }\n",
        "    DMABufBuffer* displayBuffer() { return m_displayBuffer.dmabuf.get(); }\n"
        "#if ENABLE(MEDIA_STREAM) || ENABLE(WEB_CODECS)\n"
        "    struct VideoFrameBufferCopy {\n"
        "        RefPtr<DMABufBuffer> buffer;\n"
        "        bool isNewBuffer { false };\n"
        "        Vector<uint64_t> retiredBufferIDs;\n"
        "    };\n"
        "    VideoFrameBufferCopy copySurfaceBufferToVideoFrameBuffer(SurfaceBuffer, std::span<const uint64_t> releasedBufferIDs, std::span<const uint64_t> preferredModifiers);\n"
        "#if USE(GSTREAMER)\n"
        "    RefPtr<VideoFrame> surfaceBufferToVideoFrame(SurfaceBuffer) final;\n"
        "#endif\n"
        "#endif\n",
    )
    text = replace_once(
        text,
        "    struct DrawingBuffer {\n"
        "        RefPtr<DMABufBuffer> dmabuf;\n"
        "        EGLImageKHR image { nullptr };\n"
        "    };\n",
        "    struct DrawingBuffer {\n"
        "        RefPtr<DMABufBuffer> dmabuf;\n"
        "        EGLImageKHR image { nullptr };\n"
        "        GCGLuint readTexture { 0 };\n"
        "    };\n",
    )
    return replace_once(
        text,
        "    DrawingBuffer m_drawingBuffer;\n    DrawingBuffer m_displayBuffer;\n",
        "    DrawingBuffer m_drawingBuffer;\n    DrawingBuffer m_displayBuffer;\n"
        "#if ENABLE(MEDIA_STREAM) || ENABLE(WEB_CODECS)\n"
        "    struct VideoFrameBuffer {\n"
        "        RefPtr<DMABufBuffer> dmabuf;\n"
        "        EGLImageKHR image { nullptr };\n"
        "        GCGLuint texture { 0 };\n"
        "        bool isInUse { false };\n"
        "        bool wasDelivered { false };\n"
        "    };\n"
        "    VideoFrameBuffer createVideoFrameBuffer(const IntSize&, uint32_t fourcc, std::span<const uint64_t> modifiers);\n"
        "    void destroyVideoFrameBuffer(VideoFrameBuffer&);\n"
        "    void freeVideoFrameBuffers();\n"
        "    std::optional<size_t> acquireVideoFrameBuffer(const IntSize&, uint32_t fourcc, std::span<const uint64_t> preferredModifiers, Vector<uint64_t>& retiredBufferIDs);\n"
        "    GCGLuint videoFrameSourceTexture(SurfaceBuffer);\n"
        "    void publishVideoFrameBufferContents(const VideoFrameBuffer&);\n"
        "\n"
        "    Vector<VideoFrameBuffer> m_videoFrameBuffers;\n"
        "    bool m_canCopyVideoFrames { false };\n"
        "    bool m_hasReportedVideoFramePoolExhaustion { false };\n"
        "    bool m_videoFrameModifiersAreUnusable { false };\n"
        "#if USE(GSTREAMER)\n"
        "    RefPtr<DMABufBufferReleaseQueue> m_videoFrameReleaseQueue;\n"
        "#endif\n"
        "#endif\n",
    )


VIDEO_FRAME_POOL = r"""#if ENABLE(MEDIA_STREAM) || ENABLE(WEB_CODECS)
static constexpr size_t maximumVideoFrameBuffers = 6;

GraphicsContextGLTextureMapperGBM::VideoFrameBuffer GraphicsContextGLTextureMapperGBM::createVideoFrameBuffer(const IntSize& size, uint32_t fourcc, std::span<const uint64_t> modifiers)
{
    auto gbmDevice = DRMDeviceManager::singleton().mainGBMDevice(DRMDeviceManager::NodeType::Render);
    if (!gbmDevice || modifiers.empty())
        return { };

    bool isLinear = modifiers.size() == 1 && modifiers[0] == DRM_FORMAT_MOD_LINEAR;
    struct gbm_bo* bo = gbm_bo_create_with_modifiers2(gbmDevice->device(), size.width(), size.height(), fourcc, modifiers.data(), modifiers.size(), GBM_BO_USE_RENDERING);
    if (!bo && isLinear)
        bo = gbm_bo_create(gbmDevice->device(), size.width(), size.height(), fourcc, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!bo)
        return { };

    auto dmaBufAttributes = DMABufBufferAttributes::fromGBMBufferObject(bo);
    gbm_bo_destroy(bo);
    if (!dmaBufAttributes || dmaBufAttributes->fds.size() != 1)
        return { };
    if (isLinear)
        dmaBufAttributes->modifier = DRM_FORMAT_MOD_LINEAR;
    else if (std::ranges::find(modifiers, dmaBufAttributes->modifier) == modifiers.end())
        return { };

    Ref dmabuf = DMABufBuffer::create(WTF::move(*dmaBufAttributes));
    auto enableModifiers = PlatformDisplay::sharedDisplay().eglExtensions().EXT_image_dma_buf_import_modifiers
        ? DMABufBufferAttributes::EnableModifiers::Yes : DMABufBufferAttributes::EnableModifiers::No;
    auto eglAttributes = DMABufBuffer::buildEGLImageAttributes(dmabuf->attributes(), enableModifiers);
    if (!eglAttributes)
        return { };

    auto image = EGL_CreateImageKHR(m_displayObj, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, eglAttributes->span().data());
    if (!image)
        return { };

    GCGLuint texture = 0;
    {
        ScopedRestoreTextureBinding restoreBinding(TEXTURE_BINDING_2D, TEXTURE_2D);
        GL_GenTextures(1, &texture);
        GL_BindTexture(GL_TEXTURE_2D, texture);
        GL_EGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    }
    return { WTF::move(dmabuf), image, texture };
}

void GraphicsContextGLTextureMapperGBM::destroyVideoFrameBuffer(VideoFrameBuffer& buffer)
{
    if (buffer.texture)
        GL_DeleteTextures(1, &buffer.texture);
    if (buffer.image)
        EGL_DestroyImageKHR(m_displayObj, buffer.image);
    buffer = { };
}

void GraphicsContextGLTextureMapperGBM::freeVideoFrameBuffers()
{
    for (auto& buffer : m_videoFrameBuffers)
        destroyVideoFrameBuffer(buffer);
    m_videoFrameBuffers.clear();
}

std::optional<size_t> GraphicsContextGLTextureMapperGBM::acquireVideoFrameBuffer(const IntSize& size, uint32_t fourcc, std::span<const uint64_t> preferredModifiers, Vector<uint64_t>& retiredBufferIDs)
{
    for (size_t index = m_videoFrameBuffers.size(); index--;) {
        auto& buffer = m_videoFrameBuffers[index];
        const auto& attributes = buffer.dmabuf->attributes();
        if (attributes.size == size && attributes.fourcc.value == fourcc)
            continue;
        retiredBufferIDs.append(buffer.dmabuf->id());
        destroyVideoFrameBuffer(buffer);
        m_videoFrameBuffers.removeAt(index);
    }

    for (size_t index = 0; index < m_videoFrameBuffers.size(); ++index) {
        if (!m_videoFrameBuffers[index].isInUse)
            return index;
    }

    if (m_videoFrameBuffers.size() >= maximumVideoFrameBuffers) {
        if (!m_hasReportedVideoFramePoolExhaustion) {
            RELEASE_LOG(WebGL, "All %zu WebGL video frame buffers are held by consumers, copying frames through the CPU until one is released", maximumVideoFrameBuffers);
            m_hasReportedVideoFramePoolExhaustion = true;
        }
        return std::nullopt;
    }

    VideoFrameBuffer buffer;
    if (!preferredModifiers.empty() && !m_videoFrameModifiersAreUnusable) {
        buffer = createVideoFrameBuffer(size, fourcc, preferredModifiers);
        if (!buffer.dmabuf) {
            RELEASE_LOG(WebGL, "None of the %zu DMA-BUF layouts the video post-processor imports can hold a %dx%d WebGL video frame, using linear buffers", preferredModifiers.size(), size.width(), size.height());
            m_videoFrameModifiersAreUnusable = true;
        }
    }
    if (!buffer.dmabuf) {
        const std::array<uint64_t, 1> linearModifier { DRM_FORMAT_MOD_LINEAR };
        buffer = createVideoFrameBuffer(size, fourcc, linearModifier);
    }
    if (!buffer.dmabuf) {
        RELEASE_LOG_ERROR(WebGL, "Unable to allocate a %dx%d DMA-BUF for WebGL video frames, copying them through the CPU", size.width(), size.height());
        m_canCopyVideoFrames = false;
        return std::nullopt;
    }
    m_videoFrameBuffers.append(WTF::move(buffer));
    return m_videoFrameBuffers.size() - 1;
}

GCGLuint GraphicsContextGLTextureMapperGBM::videoFrameSourceTexture(SurfaceBuffer source)
{
    if (source == SurfaceBuffer::DisplayBuffer && m_displayBuffer.image) {
        if (!m_displayBuffer.readTexture) {
            ScopedRestoreTextureBinding restoreBinding(TEXTURE_BINDING_2D, TEXTURE_2D);
            GL_GenTextures(1, &m_displayBuffer.readTexture);
            GL_BindTexture(GL_TEXTURE_2D, m_displayBuffer.readTexture);
            GL_EGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_displayBuffer.image);
        }
        return m_displayBuffer.readTexture;
    }

    if (!m_drawingBuffer.dmabuf)
        return 0;
    if (contextAttributes().antialias)
        resolveMultisamplingIfNecessary();
    return m_preserveDrawingBufferTexture ? m_preserveDrawingBufferTexture : m_texture;
}

void GraphicsContextGLTextureMapperGBM::publishVideoFrameBufferContents(const VideoFrameBuffer& buffer)
{
    auto fence = createExportedFence();
#if defined(DMA_BUF_IOCTL_IMPORT_SYNC_FILE)
    if (fence) {
        struct dma_buf_import_sync_file request { DMA_BUF_SYNC_WRITE, fence.value() };
        int result;
        do
            result = ioctl(buffer.dmabuf->attributes().fds[0].value(), DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &request);
        while (result == -1 && (errno == EINTR || errno == EAGAIN));
        if (!result)
            return;
    }
#else
    UNUSED_PARAM(buffer);
#endif
    GL_Finish();
}

auto GraphicsContextGLTextureMapperGBM::copySurfaceBufferToVideoFrameBuffer(SurfaceBuffer source, std::span<const uint64_t> releasedBufferIDs, std::span<const uint64_t> preferredModifiers) -> VideoFrameBufferCopy
{
    VideoFrameBufferCopy copy;
    for (auto bufferID : releasedBufferIDs) {
        for (auto& buffer : m_videoFrameBuffers) {
            if (buffer.dmabuf->id() == bufferID)
                buffer.isInUse = false;
        }
    }

    if (!m_canCopyVideoFrames || !makeContextCurrent())
        return copy;

    auto size = getInternalFramebufferSize();
    if (size.isEmpty())
        return copy;

    updateErrors();
    auto sourceTexture = videoFrameSourceTexture(source);
    if (!sourceTexture)
        return copy;

    const auto& attributes = contextAttributes();
    auto index = acquireVideoFrameBuffer(size, attributes.alpha ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888, preferredModifiers, copy.retiredBufferIDs);
    if (!index)
        return copy;

    auto& buffer = m_videoFrameBuffers[*index];
    bool shouldUnmultiply = attributes.alpha && attributes.premultipliedAlpha;
    GL_CopySubTextureCHROMIUM(sourceTexture, 0, GL_TEXTURE_2D, buffer.texture, 0, 0, 0, 0, 0, size.width(), size.height(), GL_TRUE, GL_FALSE, shouldUnmultiply ? GL_TRUE : GL_FALSE);
    if (auto error = GL_GetError(); error != GL_NO_ERROR) {
        RELEASE_LOG_ERROR(WebGL, "Copying a WebGL surface into a video frame buffer failed with GL error 0x%x, copying frames through the CPU", error);
        m_canCopyVideoFrames = false;
        return copy;
    }

    publishVideoFrameBufferContents(buffer);
    buffer.isInUse = true;
    copy.buffer = buffer.dmabuf;
    copy.isNewBuffer = !std::exchange(buffer.wasDelivered, true);
    return copy;
}

#if USE(GSTREAMER)
RefPtr<VideoFrame> GraphicsContextGLTextureMapperGBM::surfaceBufferToVideoFrame(SurfaceBuffer source)
{
    if (!m_videoFrameReleaseQueue)
        m_videoFrameReleaseQueue = DMABufBufferReleaseQueue::create();
    auto releasedBufferIDs = m_videoFrameReleaseQueue->take();
    auto preferredModifiers = VideoFrame::preferredDMABufModifiers(contextAttributes().alpha);
    auto copy = copySurfaceBufferToVideoFrameBuffer(source, releasedBufferIDs.span(), preferredModifiers.span());
    if (!copy.buffer)
        return nullptr;
    RefPtr frame = VideoFrame::createFromDMABufBuffer(*copy.buffer, *m_videoFrameReleaseQueue);
    if (!frame)
        m_videoFrameReleaseQueue->add(copy.buffer->id());
    return frame;
}
#endif
#endif

"""


def gbm_context_patch(text):
    text = replace_once(
        text,
        "#include <drm_fourcc.h>\n#include <wtf/unix/UnixFileDescriptor.h>\n",
        "#include <drm_fourcc.h>\n#include <wtf/unix/UnixFileDescriptor.h>\n"
        "#if ENABLE(MEDIA_STREAM) || ENABLE(WEB_CODECS)\n"
        "#include <algorithm>\n#include <array>\n#include <errno.h>\n#include <linux/dma-buf.h>\n#include <sys/ioctl.h>\n"
        "#if USE(GSTREAMER)\n#include \"VideoFrame.h\"\n#endif\n"
        "#endif\n",
    )
    text = replace_once(
        text,
        "GraphicsContextGLTextureMapperGBM::~GraphicsContextGLTextureMapperGBM()\n"
        "{\n"
        "    freeDrawingBuffers();\n"
        "}\n",
        "GraphicsContextGLTextureMapperGBM::~GraphicsContextGLTextureMapperGBM()\n"
        "{\n"
        "    if (!makeContextCurrent()) {\n"
        "        m_drawingBuffer.readTexture = 0;\n"
        "        m_displayBuffer.readTexture = 0;\n"
        "#if ENABLE(MEDIA_STREAM) || ENABLE(WEB_CODECS)\n"
        "        for (auto& buffer : m_videoFrameBuffers)\n"
        "            buffer.texture = 0;\n"
        "#endif\n"
        "    }\n"
        "#if ENABLE(MEDIA_STREAM) || ENABLE(WEB_CODECS)\n"
        "    freeVideoFrameBuffers();\n"
        "#endif\n"
        "    freeDrawingBuffers();\n"
        "}\n",
    )
    text = replace_once(
        text,
        "    if (!enableExtensionsImpl({ \"GL_OES_EGL_image\"_s }))\n"
        "        return false;\n",
        "    if (!enableExtensionsImpl({ \"GL_OES_EGL_image\"_s }))\n"
        "        return false;\n"
        "#if ENABLE(MEDIA_STREAM) || ENABLE(WEB_CODECS)\n"
        "    m_canCopyVideoFrames = supportsExtensionImpl(\"GL_CHROMIUM_copy_texture\"_s);\n"
        "#endif\n",
    )
    text = replace_once(
        text,
        "    auto destroyBuffer = [this](DrawingBuffer& buffer) {\n"
        "        if (!buffer.image)\n"
        "            return;\n",
        "    auto destroyBuffer = [this](DrawingBuffer& buffer) {\n"
        "        if (buffer.readTexture) {\n"
        "            GL_DeleteTextures(1, &buffer.readTexture);\n"
        "            buffer.readTexture = 0;\n"
        "        }\n"
        "        if (!buffer.image)\n"
        "            return;\n",
    )
    return replace_once(
        text,
        "#if ENABLE(WEBXR)\nGCGLExternalImage GraphicsContextGLTextureMapperGBM::createExternalImage(",
        VIDEO_FRAME_POOL + "#if ENABLE(WEBXR)\nGCGLExternalImage GraphicsContextGLTextureMapperGBM::createExternalImage(",
    )


def texture_mapper_context_header_patch(text):
    return replace_once(
        text,
        "    RefPtr<VideoFrame> surfaceBufferToVideoFrame(SurfaceBuffer) final;\n",
        "    RefPtr<VideoFrame> surfaceBufferToVideoFrame(SurfaceBuffer) override;\n",
    )


def texture_mapper_context_patch(text):
    text = replace_once(
        text,
        '#include "VideoFrame.h"\n#if USE(GSTREAMER)\n#include "VideoFrameContentHint.h"\n#include "VideoFrameGStreamer.h"\n#endif\n',
        '#include "VideoFrame.h"\n',
    )
    return replace_once(
        text,
        "RefPtr<VideoFrame> GraphicsContextGLTextureMapperANGLE::surfaceBufferToVideoFrame(SurfaceBuffer)\n"
        "{\n"
        "#if USE(GSTREAMER)\n"
        "    if (auto pixelBuffer = readCompositedResults()) {\n"
        "        VideoFrameGStreamer::CreateOptions options;\n"
        "        options.rotation = VideoFrameGStreamer::Rotation::UpsideDown;\n"
        "        options.isMirrored = true;\n"
        "        return VideoFrameGStreamer::createFromPixelBuffer(pixelBuffer.releaseNonNull(), { }, 30, options);\n"
        "    }\n"
        "#endif\n"
        "    return nullptr;\n"
        "}\n",
        "RefPtr<VideoFrame> GraphicsContextGLTextureMapperANGLE::surfaceBufferToVideoFrame(SurfaceBuffer)\n"
        "{\n"
        "    return nullptr;\n"
        "}\n",
    )


def remote_gl_messages_patch(text):
    return replace_once(
        text,
        "    void SurfaceBufferToVideoFrame(enum:bool WebCore::GraphicsContextGLSurfaceBuffer buffer) -> (struct std::optional<WebKit::RemoteVideoFrameProxyProperties> properties) Synchronous\n",
        "    void SurfaceBufferToVideoFrame(enum:bool WebCore::GraphicsContextGLSurfaceBuffer buffer) -> (struct std::optional<WebKit::RemoteVideoFrameProxyProperties> properties) Synchronous\n"
        "#if USE(GBM) && USE(GSTREAMER)\n"
        "    void CopySurfaceBufferToVideoFrameBuffer(enum:bool WebCore::GraphicsContextGLSurfaceBuffer buffer, Vector<uint64_t> releasedBufferIDs, Vector<uint64_t> preferredModifiers) -> (uint64_t bufferID, std::optional<WebCore::DMABufBufferAttributes> dmabufAttributes, Vector<uint64_t> retiredBufferIDs) Synchronous NotStreamEncodableReply\n"
        "#endif\n",
    )


def remote_gl_header_patch(text):
    return replace_once(
        text,
        "#elif USE(GBM)\n"
        "    virtual void prepareForDisplay(CompletionHandler<void(uint64_t, std::optional<WebCore::DMABufBuffer::Attributes>&&, UnixFileDescriptor&&)>&&) = 0;\n",
        "#elif USE(GBM)\n"
        "    virtual void prepareForDisplay(CompletionHandler<void(uint64_t, std::optional<WebCore::DMABufBuffer::Attributes>&&, UnixFileDescriptor&&)>&&) = 0;\n"
        "#if (ENABLE(MEDIA_STREAM) || ENABLE(WEB_CODECS)) && USE(GSTREAMER)\n"
        "    virtual void copySurfaceBufferToVideoFrameBuffer(WebCore::GraphicsContextGL::SurfaceBuffer, Vector<uint64_t>&& releasedBufferIDs, Vector<uint64_t>&& preferredModifiers, CompletionHandler<void(uint64_t, std::optional<WebCore::DMABufBuffer::Attributes>&&, Vector<uint64_t>&&)>&&) = 0;\n"
        "#endif\n",
    )


REMOTE_GL_GBM_COPY = r"""#if (ENABLE(MEDIA_STREAM) || ENABLE(WEB_CODECS)) && USE(GSTREAMER)
void RemoteGraphicsContextGLGBM::copySurfaceBufferToVideoFrameBuffer(WebCore::GraphicsContextGL::SurfaceBuffer buffer, Vector<uint64_t>&& releasedBufferIDs, Vector<uint64_t>&& preferredModifiers, CompletionHandler<void(uint64_t, std::optional<WebCore::DMABufBuffer::Attributes>&&, Vector<uint64_t>&&)>&& completionHandler)
{
    assertIsCurrent(workQueue());
    if (!m_context) {
        completionHandler(0, std::nullopt, { });
        return;
    }

    auto copy = m_context->copySurfaceBufferToVideoFrameBuffer(buffer, releasedBufferIDs.span(), preferredModifiers.span());
    if (!copy.buffer) {
        completionHandler(0, std::nullopt, WTF::move(copy.retiredBufferIDs));
        return;
    }

    std::optional<WebCore::DMABufBuffer::Attributes> attributes;
    if (copy.isNewBuffer) {
        const auto& source = copy.buffer->attributes();
        attributes = WebCore::DMABufBuffer::Attributes { source.size, source.fourcc, source.fds.map([](const auto& fd) { return fd.duplicate(); }), source.offsets, source.strides, source.modifier };
    }
    completionHandler(copy.buffer->id(), WTF::move(attributes), WTF::move(copy.retiredBufferIDs));
}
#endif

"""


def remote_gl_gbm_patch(text):
    text = replace_once(
        text,
        "    void prepareForDisplay(CompletionHandler<void(uint64_t, std::optional<WebCore::DMABufBuffer::Attributes>&&, UnixFileDescriptor&&)>&&) final;\n",
        "    void prepareForDisplay(CompletionHandler<void(uint64_t, std::optional<WebCore::DMABufBuffer::Attributes>&&, UnixFileDescriptor&&)>&&) final;\n"
        "#if (ENABLE(MEDIA_STREAM) || ENABLE(WEB_CODECS)) && USE(GSTREAMER)\n"
        "    void copySurfaceBufferToVideoFrameBuffer(WebCore::GraphicsContextGL::SurfaceBuffer, Vector<uint64_t>&&, Vector<uint64_t>&&, CompletionHandler<void(uint64_t, std::optional<WebCore::DMABufBuffer::Attributes>&&, Vector<uint64_t>&&)>&&) final;\n"
        "#endif\n",
    )
    return replace_once(
        text,
        "Ref<RemoteGraphicsContextGL> RemoteGraphicsContextGL::create(",
        REMOTE_GL_GBM_COPY + "Ref<RemoteGraphicsContextGL> RemoteGraphicsContextGL::create(",
    )


def remote_gl_proxy_header_patch(text):
    return replace_once(
        text,
        "    RefPtr<WebCore::VideoFrame> surfaceBufferToVideoFrame(SurfaceBuffer) final;\n",
        "    RefPtr<WebCore::VideoFrame> surfaceBufferToVideoFrame(SurfaceBuffer) override;\n",
    )


REMOTE_GL_PROXY_GBM_FRAME = r"""#if (ENABLE(MEDIA_STREAM) || ENABLE(WEB_CODECS)) && USE(GSTREAMER)
RefPtr<VideoFrame> RemoteGraphicsContextGLProxyGBM::surfaceBufferToVideoFrame(SurfaceBuffer buffer)
{
    if (isContextLost())
        return nullptr;

    auto releasedBufferIDs = m_videoFrameReleaseQueue->take();
    auto preferredModifiers = VideoFrame::preferredDMABufModifiers(contextAttributes().alpha);
    auto sendResult = sendSync(Messages::RemoteGraphicsContextGL::CopySurfaceBufferToVideoFrameBuffer(buffer, releasedBufferIDs, preferredModifiers));
    if (!sendResult.succeeded()) {
        markContextLost();
        return nullptr;
    }

    auto [bufferID, bufferAttributes, retiredBufferIDs] = sendResult.takeReply();
    for (auto retiredBufferID : retiredBufferIDs)
        m_videoFrameBuffers.remove(retiredBufferID);
    if (!bufferID)
        return nullptr;

    if (bufferAttributes)
        m_videoFrameBuffers.set(bufferID, DMABufBuffer::create(bufferID, WTF::move(*bufferAttributes)));

    RefPtr<VideoFrame> frame;
    if (RefPtr dmabuf = m_videoFrameBuffers.get(bufferID))
        frame = VideoFrame::createFromDMABufBuffer(*dmabuf, m_videoFrameReleaseQueue);
    if (!frame)
        m_videoFrameReleaseQueue->add(bufferID);
    return frame;
}
#endif

"""


def remote_gl_proxy_gbm_patch(text):
    text = replace_once(
        text,
        "#include <WebCore/TextureMapperFlags.h>\n",
        "#include <WebCore/TextureMapperFlags.h>\n#include <WebCore/VideoFrame.h>\n",
    )
    text = replace_once(
        text,
        "    void prepareForDisplay() final;\n",
        "    void prepareForDisplay() final;\n"
        "#if (ENABLE(MEDIA_STREAM) || ENABLE(WEB_CODECS)) && USE(GSTREAMER)\n"
        "    RefPtr<VideoFrame> surfaceBufferToVideoFrame(SurfaceBuffer) final;\n"
        "#endif\n",
    )
    text = replace_once(
        text,
        "    RefPtr<DMABufBuffer> m_drawingBuffer;\n    RefPtr<DMABufBuffer> m_displayBuffer;\n};\n",
        "    RefPtr<DMABufBuffer> m_drawingBuffer;\n    RefPtr<DMABufBuffer> m_displayBuffer;\n"
        "#if (ENABLE(MEDIA_STREAM) || ENABLE(WEB_CODECS)) && USE(GSTREAMER)\n"
        "    HashMap<uint64_t, Ref<DMABufBuffer>> m_videoFrameBuffers;\n"
        "    const Ref<DMABufBufferReleaseQueue> m_videoFrameReleaseQueue { DMABufBufferReleaseQueue::create() };\n"
        "#endif\n"
        "};\n",
    )
    return replace_once(
        text,
        "Ref<RemoteGraphicsContextGLProxy> RemoteGraphicsContextGLProxy::platformCreate(",
        REMOTE_GL_PROXY_GBM_FRAME + "Ref<RemoteGraphicsContextGLProxy> RemoteGraphicsContextGLProxy::platformCreate(",
    )


VIDEO_ACCELERATION_HELPERS = r"""static std::optional<IntSize> minimumEncoderInputSize(GstElement* encoder)
{
    GRefPtr pad = adoptGRef(gst_element_get_static_pad(encoder, "sink"));
    GRefPtr caps = pad ? adoptGRef(gst_pad_query_caps(pad.get(), nullptr)) : nullptr;
    if (!caps || !gst_caps_get_size(caps.get()))
        return std::nullopt;
    auto minimum = [&](const char* field) -> std::optional<int> {
        auto* value = gst_structure_get_value(gst_caps_get_structure(caps.get(), 0), field);
        if (value && GST_VALUE_HOLDS_INT_RANGE(value))
            return gst_value_get_int_range_min(value);
        return std::nullopt;
    };
    auto width = minimum("width");
    auto height = minimum("height");
    if (!width || !height)
        return std::nullopt;
    return IntSize { *width, *height };
}

static void videoEncoderScaleInputWithVideoAcceleration(WebKitVideoEncoder* self, GstEvent* event)
{
    auto* priv = self->priv;
    GstCaps* caps = nullptr;
    gst_event_parse_caps(event, &caps);
    GRefPtr inputCapsFilter = adoptGRef(gst_bin_get_by_name(GST_BIN_CAST(self), "input-capsfilter"));
    if (!caps || !gst_caps_get_size(caps) || !inputCapsFilter)
        return;

    GRefPtr<GstCaps> filterCaps;
    g_object_get(inputCapsFilter.get(), "caps", &filterCaps.outPtr(), nullptr);
    if (!filterCaps || !gst_caps_get_size(filterCaps.get()))
        return;

    auto* structure = gst_caps_get_structure(caps, 0);
    auto width = gstStructureGet<int>(structure, "width"_s);
    auto height = gstStructureGet<int>(structure, "height"_s);
    filterCaps = adoptGRef(gst_caps_make_writable(filterCaps.leakRef()));
    auto* filterStructure = gst_caps_get_structure(filterCaps.get(), 0);
    if (priv->scaleResolutionDownBy > 1.0 && width && height) {
        double scale = 1 / priv->scaleResolutionDownBy;
        if (auto minimum = minimumEncoderInputSize(priv->encoder.get()))
            scale = std::max({ scale, static_cast<double>(minimum->width()) / *width, static_cast<double>(minimum->height()) / *height });
        int scaledWidth = std::max(2, static_cast<int>(std::lround(*width * scale)) & ~1);
        int scaledHeight = std::max(2, static_cast<int>(std::lround(*height * scale)) & ~1);
        gst_structure_set(filterStructure, "width", G_TYPE_INT, scaledWidth, "height", G_TYPE_INT, scaledHeight, nullptr);
        priv->hasScaledInputCaps = true;
    } else if (priv->hasScaledInputCaps) {
        gst_structure_remove_fields(filterStructure, "width", "height", nullptr);
        priv->hasScaledInputCaps = false;
    } else
        return;
    GST_DEBUG_OBJECT(self, "Scaling hardware encoder input to %" GST_PTR_FORMAT, filterCaps.get());
    g_object_set(inputCapsFilter.get(), "caps", filterCaps.get(), nullptr);
}

static void videoEncoderRefreshPostProcessorPool(WebKitVideoEncoder* self, GstCaps* caps)
{
    if (!self->priv->postProcessor)
        return;
    GRefPtr pad = adoptGRef(gst_element_get_static_pad(self->priv->postProcessor.get(), "sink"));
    GRefPtr query = adoptGRef(gst_query_new_allocation(caps, TRUE));
    if (!pad || !gst_pad_query(pad.get(), query.get()))
        GST_DEBUG_OBJECT(self, "The video post-processor answered no allocation query for %" GST_PTR_FORMAT, caps);
}

static bool encoderAcceptsFrameSize(GstElementFactory* factory, GstCaps* encodedCaps)
{
    if (!encodedCaps || !gst_caps_get_size(encodedCaps))
        return true;
    auto* structure = gst_caps_get_structure(encodedCaps, 0);
    auto width = gstStructureGet<int>(structure, "width"_s);
    auto height = gstStructureGet<int>(structure, "height"_s);
    if (!width || !height)
        return true;

    GUniquePtr<GstStructure> size(gst_structure_new("video/x-raw", "width", G_TYPE_INT, *width, "height", G_TYPE_INT, *height, nullptr));
    for (auto* item = gst_element_factory_get_static_pad_templates(factory); item; item = item->next) {
        auto* padTemplate = static_cast<GstStaticPadTemplate*>(item->data);
        if (padTemplate->direction != GST_PAD_SINK)
            continue;
        GRefPtr templateCaps = adoptGRef(gst_static_caps_get(&padTemplate->static_caps));
        for (unsigned index = 0; index < gst_caps_get_size(templateCaps.get()); ++index) {
            auto* templateStructure = gst_caps_get_structure(templateCaps.get(), index);
            GUniquePtr<GstStructure> bounds(gst_structure_new_empty("video/x-raw"));
            for (const char* field : { "width", "height" }) {
                if (auto* value = gst_structure_get_value(templateStructure, field))
                    gst_structure_set_value(bounds.get(), field, value);
            }
            if (gst_structure_can_intersect(size.get(), bounds.get()))
                return true;
        }
    }
    return false;
}

"""


def video_encoder_bin_patch(text):
    text = replace_once(text, "#include <gst/video/gstvideoencoder.h>\n", "#include <algorithm>\n#include <cmath>\n#include <gst/video/gstvideoencoder.h>\n#include <wtf/glib/GUniquePtr.h>\n")
    text = replace_once(
        text,
        "    double scaleResolutionDownBy;\n    bool enableVideoFlip;\n};\n",
        "    double scaleResolutionDownBy;\n    bool enableVideoFlip;\n    bool acceptsDMABuf;\n    bool hasScaledInputCaps;\n    GRefPtr<GstElement> postProcessor;\n};\n",
    )
    text = replace_once(
        text,
        "static bool videoEncoderSetEncoder(WebKitVideoEncoder* self, EncoderId encoderId, GRefPtr<GstCaps>&& inputCaps, GRefPtr<GstCaps>&& encodedCaps, bool useAnnexB)\n",
        VIDEO_ACCELERATION_HELPERS + "static bool videoEncoderSetEncoder(WebKitVideoEncoder* self, EncoderId encoderId, GRefPtr<GstCaps>&& inputCaps, GRefPtr<GstCaps>&& encodedCaps, bool useAnnexB)\n",
    )
    text = replace_once(
        text,
        "    auto videoConvert = createVideoConvertScaleElement();\n"
        "    if (!videoConvert)\n"
        "        return false;\n",
        "    GRefPtr<GstElement> videoConvert;\n"
        "    priv->acceptsDMABuf = false;\n"
        "    priv->hasScaledInputCaps = false;\n"
        "    priv->postProcessor = nullptr;\n"
        "    if (encoderUsesVideoAcceleration(encoderId)) {\n"
        "        videoConvert = makeGStreamerElement(\"vapostproc\"_s);\n"
        "        if (videoConvert) {\n"
        "            priv->acceptsDMABuf = true;\n"
        "            priv->postProcessor = videoConvert;\n"
        "            GRefPtr videoAccelerationCaps = adoptGRef(gst_caps_copy(inputCaps.get()));\n"
        "            gst_caps_set_features(videoAccelerationCaps.get(), 0, gst_caps_features_new(\"memory:VAMemory\", nullptr));\n"
        "            gst_structure_remove_field(gst_caps_get_structure(videoAccelerationCaps.get(), 0), \"format\");\n"
        "            g_object_set(inputCapsFilter, \"caps\", videoAccelerationCaps.get(), nullptr);\n"
        "            inputCaps = WTF::move(videoAccelerationCaps);\n"
        "            GST_INFO_OBJECT(self, \"Feeding %s through the video post-processor\", encoderDefinition->name.characters());\n"
        "        }\n"
        "    }\n"
        "    if (!videoConvert)\n"
        "        videoConvert = createVideoConvertScaleElement();\n"
        "    if (!videoConvert)\n"
        "        return false;\n",
    )
    text = replace_once(
        text,
        "    if (priv->enableVideoFlip) {\n",
        "    if (priv->enableVideoFlip && !priv->acceptsDMABuf) {\n",
    )
    text = replace_once(
        text,
        "        if (gst_element_factory_can_src_any_caps(encoder.factory.get(), caps.get())) {\n",
        "        if (gst_element_factory_can_src_any_caps(encoder.factory.get(), caps.get()) && encoderAcceptsFrameSize(encoder.factory.get(), caps.get())) {\n",
    )
    text = replace_once(
        text,
        "void videoEncoderSetBitRateAllocation(WebKitVideoEncoder* self, RefPtr<WebKitVideoEncoderBitRateAllocation>&& allocation)\n",
        "bool videoEncoderAcceptsDMABuf(WebKitVideoEncoder* self)\n"
        "{\n"
        "    return self->priv->acceptsDMABuf;\n"
        "}\n"
        "\n"
        "void videoEncoderSetBitRateAllocation(WebKitVideoEncoder* self, RefPtr<WebKitVideoEncoderBitRateAllocation>&& allocation)\n",
    )
    text = replace_once(
        text,
        "        if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {\n"
        "            auto self = WEBKIT_VIDEO_ENCODER(parent);\n",
        "        if (GST_EVENT_TYPE(event) == GST_EVENT_TAG) {\n"
        "            auto self = WEBKIT_VIDEO_ENCODER(parent);\n"
        "            GstTagList* tags = nullptr;\n"
        "            gst_event_parse_tag(event, &tags);\n"
        "            GUniqueOutPtr<char> orientation;\n"
        "            if (self->priv->postProcessor && self->priv->enableVideoFlip && tags && gst_tag_list_get_string(tags, GST_TAG_IMAGE_ORIENTATION, &orientation.outPtr()) && g_strcmp0(orientation.get(), \"rotate-0\")) {\n"
        "                GST_INFO_OBJECT(self, \"Asking the video post-processor to apply orientation %s\", orientation.get());\n"
        "                gst_util_set_object_arg(G_OBJECT(self->priv->postProcessor.get()), \"video-direction\", \"auto\");\n"
        "            }\n"
        "        }\n"
        "\n"
        "        if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {\n"
        "            auto self = WEBKIT_VIDEO_ENCODER(parent);\n",
    )
    return replace_once(
        text,
        "            auto self = WEBKIT_VIDEO_ENCODER(parent);\n"
        "            auto scaleResolutionDownBy = self->priv->scaleResolutionDownBy;\n",
        "            auto self = WEBKIT_VIDEO_ENCODER(parent);\n"
        "            if (self->priv->acceptsDMABuf) {\n"
        "                videoEncoderScaleInputWithVideoAcceleration(self, event);\n"
        "                GstCaps* caps = nullptr;\n"
        "                gst_event_parse_caps(event, &caps);\n"
        "                GRefPtr<GstCaps> inputCaps = caps;\n"
        "                bool handled = gst_pad_event_default(pad, parent, event);\n"
        "                if (handled && inputCaps)\n"
        "                    videoEncoderRefreshPostProcessorPool(self, inputCaps.get());\n"
        "                return handled;\n"
        "            }\n"
        "            auto scaleResolutionDownBy = self->priv->scaleResolutionDownBy;\n",
    )


def video_encoder_bin_header_patch(text):
    return replace_once(
        text,
        "bool videoEncoderSetCodec(WebKitVideoEncoder*, const WebCore::CodecConfig&, const WebCore::IntSize&, std::optional<double> frameRate = std::nullopt, bool enableVideoFlip = false);\n",
        "bool videoEncoderSetCodec(WebKitVideoEncoder*, const WebCore::CodecConfig&, const WebCore::IntSize&, std::optional<double> frameRate = std::nullopt, bool enableVideoFlip = false);\n"
        "bool videoEncoderAcceptsDMABuf(WebKitVideoEncoder*);\n",
    )


def video_encoder_header_patch(text):
    return replace_once(
        text,
        "    Ref<GenericPromise> setBitRateAllocation(RefPtr<WebKitVideoEncoderBitRateAllocation>&&, double frameRate);\n",
        "    Ref<GenericPromise> setBitRateAllocation(RefPtr<WebKitVideoEncoderBitRateAllocation>&&, double frameRate);\n"
        "    bool acceptsDMABuf() const;\n",
    )


ENCODER_FRAME_MEMORY = r"""static bool encoderTakesFrameMemory(VideoFrameGStreamer::MemoryType memoryType, bool acceptsDMABuf)
{
    switch (memoryType) {
    case VideoFrameGStreamer::MemoryType::System:
        return true;
#if USE(GSTREAMER_GL)
    case VideoFrameGStreamer::MemoryType::GL:
        return false;
#if USE(GBM)
    case VideoFrameGStreamer::MemoryType::DMABuf:
        return acceptsDMABuf;
#endif
#endif
    case VideoFrameGStreamer::MemoryType::Unsupported:
        break;
    }
    UNUSED_PARAM(acceptsDMABuf);
    return false;
}

bool GStreamerInternalVideoEncoder::acceptsDMABuf() const
{
    return m_isInitialized && videoEncoderAcceptsDMABuf(WEBKIT_VIDEO_ENCODER(m_harness->element()));
}

"""


def video_encoder_patch(text):
    text = replace_once(
        text,
        "    bool isClosed() const { return m_isClosed; }\n",
        "    bool isClosed() const { return m_isClosed; }\n    bool acceptsDMABuf() const;\n",
    )
    text = replace_once(
        text,
        "void GStreamerVideoEncoder::reset()\n",
        "bool GStreamerVideoEncoder::acceptsDMABuf() const\n"
        "{\n"
        "    return m_internalEncoder->acceptsDMABuf();\n"
        "}\n"
        "\n"
        "void GStreamerVideoEncoder::reset()\n",
    )
    text = replace_once(
        text,
        "bool GStreamerInternalVideoEncoder::encode(VideoEncoder::RawFrame&& rawFrame, bool shouldGenerateKeyFrame)\n",
        ENCODER_FRAME_MEMORY + "bool GStreamerInternalVideoEncoder::encode(VideoEncoder::RawFrame&& rawFrame, bool shouldGenerateKeyFrame)\n",
    )
    return replace_once(
        text,
        "    auto& gstVideoFrame = downcast<VideoFrameGStreamer>(rawFrame.frame.get());\n"
        "    GRefPtr sample = gstVideoFrame.sample();\n",
        "    auto& gstVideoFrame = downcast<VideoFrameGStreamer>(rawFrame.frame.get());\n"
        "    GRefPtr sample = gstVideoFrame.sample();\n"
        "    if (!encoderTakesFrameMemory(gstVideoFrame.memoryType(), acceptsDMABuf())) {\n"
        "        IntSize size { static_cast<int>(m_config.width), static_cast<int>(m_config.height) };\n"
        "        if (size.isEmpty())\n"
        "            size = gstVideoFrame.presentationSize();\n"
        "        sample = gstVideoFrame.convert(GST_VIDEO_FORMAT_I420, size);\n"
        "        if (!sample) {\n"
        "            GST_WARNING_OBJECT(m_harness->element(), \"Unable to bring a %dx%d frame into system memory for encoding\", size.width(), size.height());\n"
        "            return false;\n"
        "        }\n"
        "    }\n",
    )


DOWNSTREAM_MEMORY = r"""    std::optional<GstVideoFormat> systemMemoryFormatForDownstream(VideoFrameGStreamer& frame, bool needsResize)
    {
        if (frame.memoryType() == VideoFrameGStreamer::MemoryType::System || m_consumerIsVideoPlayer)
            return std::nullopt;

        auto* caps = gst_sample_get_caps(frame.sample().get());
        if (!caps || !gst_caps_get_size(caps))
            return std::nullopt;

        auto frameFormat = static_cast<GstVideoFormat>(frame.pixelFormat());
        if (m_needsDownstreamMemoryCheck.exchange(false) || !m_checkedDownstreamCaps || !gst_caps_is_equal(m_checkedDownstreamCaps.get(), caps)) {
            m_checkedDownstreamCaps = caps;
            GRefPtr pad = adoptGRef(gst_element_get_static_pad(m_src.get(), "src"));
            GRefPtr allowed = adoptGRef(gst_pad_peer_query_caps(pad.get(), nullptr));
            bool isExplicit = allowed && !gst_caps_is_any(allowed.get()) && !gst_caps_is_empty(allowed.get());
            if (isExplicit && gst_caps_can_intersect(allowed.get(), caps))
                m_downstreamSystemMemoryFormat = std::nullopt;
            else {
                m_downstreamSystemMemoryFormat = GST_VIDEO_FORMAT_I420;
                if (isExplicit) {
                    auto* structure = gst_caps_get_structure(caps, 0);
                    for (auto format : { GST_VIDEO_FORMAT_I420, frameFormat }) {
                        GRefPtr candidate = adoptGRef(gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, gst_video_format_to_string(format), nullptr));
                        auto* candidateStructure = gst_caps_get_structure(candidate.get(), 0);
                        for (const char* field : { "width", "height" }) {
                            if (auto* value = gst_structure_get_value(structure, field))
                                gst_structure_set_value(candidateStructure, field, value);
                        }
                        if (gst_caps_can_intersect(allowed.get(), candidate.get())) {
                            m_downstreamSystemMemoryFormat = format;
                            break;
                        }
                    }
                }
            }
            GST_DEBUG_OBJECT(m_src.get(), "Downstream %s %" GST_PTR_FORMAT, m_downstreamSystemMemoryFormat ? "needs system memory instead of" : "takes", caps);
        }

        if (!m_downstreamSystemMemoryFormat && needsResize)
            return frameFormat;
        return m_downstreamSystemMemoryFormat;
    }

"""


def media_stream_source_patch(text):
    text = replace_once(text, "#include <wtf/CheckedRef.h>\n", "#include <atomic>\n#include <wtf/CheckedRef.h>\n")
    text = replace_once(
        text,
        "        auto gstVideoFrame = static_cast<VideoFrameGStreamer*>(&videoFrame);\n"
        "        GRefPtr<GstSample> sample = gstVideoFrame->sample();\n"
        "\n"
        "        // Video encoders require a multiple of two frame size. At least x264enc does anyway.\n"
        "        if (!m_consumerIsVideoPlayer && !m_isIncomingVideoSource && (captureSize.width() % 2 || captureSize.height() % 2)) {\n"
        "            captureSize.setWidth(roundUpToMultipleOf(2, captureSize.width()));\n"
        "            captureSize.setHeight(roundUpToMultipleOf(2, captureSize.height()));\n"
        "            sample = gstVideoFrame->resizedSample(captureSize);\n"
        "        }\n",
        "        auto gstVideoFrame = static_cast<VideoFrameGStreamer*>(&videoFrame);\n"
        "        GRefPtr<GstSample> sample = gstVideoFrame->sample();\n"
        "\n"
        "        bool needsEvenSize = !m_consumerIsVideoPlayer && !m_isIncomingVideoSource && (captureSize.width() % 2 || captureSize.height() % 2);\n"
        "        if (needsEvenSize) {\n"
        "            captureSize.setWidth(roundUpToMultipleOf(2, captureSize.width()));\n"
        "            captureSize.setHeight(roundUpToMultipleOf(2, captureSize.height()));\n"
        "        }\n"
        "        if (auto format = systemMemoryFormatForDownstream(*gstVideoFrame, needsEvenSize))\n"
        "            sample = gstVideoFrame->convert(*format, captureSize);\n"
        "        else if (needsEvenSize)\n"
        "            sample = gstVideoFrame->resizedSample(captureSize);\n"
        "        if (!sample) {\n"
        "            GST_WARNING_OBJECT(m_src.get(), \"Dropping a %dx%d video frame that could not be prepared for downstream\", captureSize.width(), captureSize.height());\n"
        "            return;\n"
        "        }\n",
    )
    text = replace_once(
        text,
        "    void audioSamplesAvailable(const MediaTime&, const PlatformAudioData& audioData, const AudioStreamDescription&, size_t) final\n",
        DOWNSTREAM_MEMORY + "    void audioSamplesAvailable(const MediaTime&, const PlatformAudioData& audioData, const AudioStreamDescription&, size_t) final\n",
    )
    text = replace_once(
        text,
        "        if (!trackSource.isIncomingAudioSource() && !trackSource.isIncomingVideoSource())\n            return;\n\n        connectIncomingTrack();\n    }\n",
        "        if (m_isVideoTrack) {\n"
        "            m_reconfigurePadProbeHandle = PadProbeHandle<InternalSource>::create(*this, adoptGRef(gst_element_get_static_pad(m_src.get(), \"src\")), GST_PAD_PROBE_TYPE_EVENT_UPSTREAM, [](const auto& self, const auto&, auto info) -> GstPadProbeReturn {\n"
        "                if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) == GST_EVENT_RECONFIGURE)\n"
        "                    self->m_needsDownstreamMemoryCheck = true;\n"
        "                return GST_PAD_PROBE_OK;\n"
        "            });\n"
        "        }\n"
        "\n"
        "        if (!trackSource.isIncomingAudioSource() && !trackSource.isIncomingVideoSource())\n            return;\n\n        connectIncomingTrack();\n    }\n",
    )
    return replace_once(
        text,
        "    RefPtr<PadProbeHandle<InternalSource>> m_upstreamQueryPadProbeHandle;\n};\n",
        "    RefPtr<PadProbeHandle<InternalSource>> m_upstreamQueryPadProbeHandle;\n"
        "    RefPtr<PadProbeHandle<InternalSource>> m_reconfigurePadProbeHandle;\n"
        "    std::atomic<bool> m_needsDownstreamMemoryCheck { true };\n"
        "    GRefPtr<GstCaps> m_checkedDownstreamCaps;\n"
        "    std::optional<GstVideoFormat> m_downstreamSystemMemoryFormat;\n"
        "};\n",
    )


def video_packetizer_patch(text):
    return replace_once(
        text,
        "            GRefPtr caps = adoptGRef(gst_caps_new_simple(\"video/x-raw\", \"framerate\", GST_TYPE_FRACTION, numerator, denominator, nullptr));\n",
        "            GRefPtr caps = adoptGRef(gst_caps_new_simple(\"video/x-raw\", \"framerate\", GST_TYPE_FRACTION, numerator, denominator, nullptr));\n"
        "            gst_caps_set_features(caps.get(), 0, gst_caps_features_new_any());\n",
    )


PATCHES = {
    DMABUF_BUFFER: dmabuf_buffer_patch,
    VIDEO_FRAME_API: video_frame_api_patch,
    VIDEO_FRAME: video_frame_patch,
    GBM_CONTEXT_HEADER: gbm_context_header_patch,
    GBM_CONTEXT: gbm_context_patch,
    TEXTURE_MAPPER_CONTEXT_HEADER: texture_mapper_context_header_patch,
    TEXTURE_MAPPER_CONTEXT: texture_mapper_context_patch,
    REMOTE_GL_MESSAGES: remote_gl_messages_patch,
    REMOTE_GL_HEADER: remote_gl_header_patch,
    REMOTE_GL_GBM: remote_gl_gbm_patch,
    REMOTE_GL_PROXY_HEADER: remote_gl_proxy_header_patch,
    REMOTE_GL_PROXY_GBM: remote_gl_proxy_gbm_patch,
    VIDEO_ENCODER_BIN: video_encoder_bin_patch,
    VIDEO_ENCODER_BIN_HEADER: video_encoder_bin_header_patch,
    VIDEO_ENCODER: video_encoder_patch,
    VIDEO_ENCODER_HEADER: video_encoder_header_patch,
    MEDIA_STREAM_SOURCE: media_stream_source_patch,
    VIDEO_PACKETIZER: video_packetizer_patch,
}

from wpe.patch_support import replace_once


CAPTURER = "Source/WebCore/platform/mediastream/gstreamer/GStreamerVideoCapturer.cpp"
VIDEO_CAPTURE_SOURCE_BASE = "Source/WebCore/platform/mediastream/RealtimeVideoCaptureSource.cpp"
MOCK_VIDEO_SOURCE = "Source/WebCore/platform/mediastream/gstreamer/MockRealtimeVideoSourceGStreamer.cpp"
CAPTURER_HEADER = "Source/WebCore/platform/mediastream/gstreamer/GStreamerVideoCapturer.h"


SAMPLE_WITH_VIDEO_META = r'''static GQuark pipeWireFrameGateQuark()
{
    static GQuark quark = g_quark_from_static_string("webkit-pipewire-frame-gate");
    return quark;
}

struct GatedPipeWireBuffer {
    Ref<PipeWireFrameGate> gate;
    GRefPtr<GstBuffer> buffer;
};

static GRefPtr<GstSample> sampleReleasedThroughGate(GRefPtr<GstSample>&& sample, PipeWireFrameGate& gate)
{
    auto* buffer = gst_sample_get_buffer(sample.get());
    if (!buffer)
        return WTF::move(sample);

    GRefPtr gated = adoptGRef(gst_buffer_copy_region(buffer, static_cast<GstBufferCopyFlags>(GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_META | GST_BUFFER_COPY_MEMORY), 0, static_cast<gsize>(-1)));
    Vector<GstMeta*> parents;
    gpointer state = nullptr;
    while (auto* meta = gst_buffer_iterate_meta_filtered(gated.get(), &state, GST_PARENT_BUFFER_META_API_TYPE))
        parents.append(meta);
    for (auto* parent : parents)
        gst_buffer_remove_meta(gated.get(), parent);

    gst_mini_object_set_qdata(GST_MINI_OBJECT_CAST(gated.get()), pipeWireFrameGateQuark(), new GatedPipeWireBuffer { gate, GRefPtr<GstBuffer>(buffer) }, [](gpointer data) {
        std::unique_ptr<GatedPipeWireBuffer> gatedBuffer(static_cast<GatedPipeWireBuffer*>(data));
        gatedBuffer->gate->release(WTF::move(gatedBuffer->buffer));
    });
    sample = adoptGRef(gst_sample_make_writable(sample.leakRef()));
    gst_sample_set_buffer(sample.get(), gated.get());
    return WTF::move(sample);
}

static GRefPtr<GstSample> sampleWithVideoMeta(GRefPtr<GstSample>&& sample)
{
    auto* buffer = gst_sample_get_buffer(sample.get());
    auto* caps = gst_sample_get_caps(sample.get());
    if (!buffer || !caps || gst_buffer_get_video_meta(buffer))
        return WTF::move(sample);

    GstVideoInfo info;
#if GST_CHECK_VERSION(1, 24, 0)
    if (gst_video_is_dma_drm_caps(caps)) {
        GstVideoInfoDmaDrm drmInfo;
        if (!gst_video_info_dma_drm_from_caps(&drmInfo, caps) || !gst_video_info_dma_drm_to_video_info(&drmInfo, &info))
            return WTF::move(sample);
    } else
#endif
    if (!gst_video_info_from_caps(&info, caps))
        return WTF::move(sample);

    GRefPtr writableBuffer = adoptGRef(gst_buffer_make_writable(gst_buffer_ref(buffer)));
    gst_buffer_add_parent_buffer_meta(writableBuffer.get(), buffer);
    gst_buffer_add_video_meta_full(writableBuffer.get(), GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_INFO_FORMAT(&info), GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info), GST_VIDEO_INFO_N_PLANES(&info), info.offset, info.stride);
    sample = adoptGRef(gst_sample_make_writable(sample.leakRef()));
    gst_sample_set_buffer(sample.get(), writableBuffer.get());
    return WTF::move(sample);
}

static IntSize sampleResolution(const GRefPtr<GstSample>& sample)
{
    auto resolution = getVideoResolutionFromCaps(gst_sample_get_caps(sample.get()));
    return resolution ? IntSize(*resolution) : IntSize { };
}

static GRefPtr<GstSample> sampleWithCropRectangle(const GRefPtr<GstSample>& sample, const GstVideoInfo& info, GstBuffer* timingSource, unsigned x, unsigned y, unsigned width, unsigned height)
{
    auto* sourceBuffer = gst_sample_get_buffer(sample.get());
    GRefPtr writableBuffer = adoptGRef(gst_buffer_make_writable(gst_buffer_ref(sourceBuffer)));
    gst_buffer_add_parent_buffer_meta(writableBuffer.get(), sourceBuffer);
    GST_BUFFER_PTS(writableBuffer.get()) = GST_BUFFER_PTS(timingSource);
    GST_BUFFER_DTS(writableBuffer.get()) = GST_BUFFER_DTS(timingSource);
    GST_BUFFER_DURATION(writableBuffer.get()) = GST_BUFFER_DURATION(timingSource);

    auto* videoMeta = gst_buffer_get_video_meta(writableBuffer.get());
    unsigned planes = GST_VIDEO_INFO_N_PLANES(&info);
    gsize offsets[GST_VIDEO_MAX_PLANES] = { };
    gint strides[GST_VIDEO_MAX_PLANES] = { };
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN;
    for (unsigned plane = 0; plane < planes; ++plane) {
        offsets[plane] = videoMeta ? videoMeta->offset[plane] : GST_VIDEO_INFO_PLANE_OFFSET(&info, plane);
        strides[plane] = videoMeta ? videoMeta->stride[plane] : GST_VIDEO_INFO_PLANE_STRIDE(&info, plane);
        gint components[GST_VIDEO_MAX_COMPONENTS];
        gst_video_format_info_component(info.finfo, plane, components);
        auto componentX = GST_VIDEO_FORMAT_INFO_SCALE_WIDTH(info.finfo, components[0], x);
        auto componentY = GST_VIDEO_FORMAT_INFO_SCALE_HEIGHT(info.finfo, components[0], y);
        offsets[plane] += static_cast<gsize>(componentY) * strides[plane] + static_cast<gsize>(componentX) * GST_VIDEO_FORMAT_INFO_PSTRIDE(info.finfo, components[0]);
    }
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END;
    auto flags = videoMeta ? videoMeta->flags : GST_VIDEO_FRAME_FLAG_NONE;
    if (videoMeta)
        gst_buffer_remove_meta(writableBuffer.get(), GST_META_CAST(videoMeta));
    if (auto* cropMeta = gst_buffer_get_video_crop_meta(writableBuffer.get()))
        gst_buffer_remove_meta(writableBuffer.get(), GST_META_CAST(cropMeta));
    gst_buffer_add_video_meta_full(writableBuffer.get(), flags, GST_VIDEO_INFO_FORMAT(&info), width, height, planes, offsets, strides);

    GRefPtr caps = adoptGRef(gst_caps_copy(gst_sample_get_caps(sample.get())));
    gst_caps_set_simple(caps.get(), "width", G_TYPE_INT, static_cast<int>(width), "height", G_TYPE_INT, static_cast<int>(height), nullptr);
    return adoptGRef(gst_sample_new(writableBuffer.get(), caps.get(), gst_sample_get_segment(sample.get()), nullptr));
}

static GRefPtr<GstSample> sampleWithCropApplied(GRefPtr<GstSample>&& sample)
{
    auto* buffer = gst_sample_get_buffer(sample.get());
    auto* caps = gst_sample_get_caps(sample.get());
    auto* crop = buffer && caps ? gst_buffer_get_video_crop_meta(buffer) : nullptr;
    if (!crop)
        return WTF::move(sample);

    GstVideoInfo info;
    bool isLinear = true;
#if GST_CHECK_VERSION(1, 24, 0)
    if (gst_video_is_dma_drm_caps(caps)) {
        GstVideoInfoDmaDrm drmInfo;
        if (!gst_video_info_dma_drm_from_caps(&drmInfo, caps) || !gst_video_info_dma_drm_to_video_info(&drmInfo, &info))
            return WTF::move(sample);
        isLinear = !drmInfo.drm_modifier;
    } else
#endif
    if (!gst_video_info_from_caps(&info, caps))
        return WTF::move(sample);

    unsigned frameWidth = GST_VIDEO_INFO_WIDTH(&info);
    unsigned frameHeight = GST_VIDEO_INFO_HEIGHT(&info);
    if (!crop->width || !crop->height || crop->x + crop->width > frameWidth || crop->y + crop->height > frameHeight) {
        GST_WARNING("Ignoring a %ux%u crop at %u,%u outside a %ux%u frame", crop->width, crop->height, crop->x, crop->y, frameWidth, frameHeight);
        return WTF::move(sample);
    }

    unsigned alignmentX = 1;
    unsigned alignmentY = 1;
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN;
    for (unsigned component = 0; component < GST_VIDEO_INFO_N_COMPONENTS(&info); ++component) {
        alignmentX = std::max(alignmentX, 1u << GST_VIDEO_FORMAT_INFO_W_SUB(info.finfo, component));
        alignmentY = std::max(alignmentY, 1u << GST_VIDEO_FORMAT_INFO_H_SUB(info.finfo, component));
    }
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END;
    unsigned x = crop->x - crop->x % alignmentX;
    unsigned y = crop->y - crop->y % alignmentY;
    unsigned width = crop->width + crop->x - x;
    unsigned height = crop->height + crop->y - y;

    if (isLinear && !GST_VIDEO_FORMAT_INFO_IS_TILED(info.finfo))
        return sampleWithCropRectangle(sample, info, buffer, x, y, width, height);

    GRefPtr systemMemoryCaps = adoptGRef(gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRA", "width", G_TYPE_INT, static_cast<int>(frameWidth), "height", G_TYPE_INT, static_cast<int>(frameHeight), nullptr));
    auto converted = GStreamerVideoFrameConverter::singleton().convert(sample, systemMemoryCaps);
    GstVideoInfo convertedInfo;
    if (!converted || !gst_video_info_from_caps(&convertedInfo, gst_sample_get_caps(converted.get()))) {
        GST_WARNING("Could not bring a %ux%u frame into system memory to crop it", frameWidth, frameHeight);
        return WTF::move(sample);
    }
    return sampleWithCropRectangle(converted, convertedInfo, buffer, crop->x, crop->y, crop->width, crop->height);
}

void GStreamerVideoCapturer::updateDisplayFrameSize(const IntSize& sourceSize, const GRefPtr<GstSample>& sample)
{
    auto frameSize = sampleResolution(sample);
    if (frameSize.isEmpty() || (frameSize == m_displayFrameSize && sourceSize == m_displaySourceSize))
        return;

    m_displayFrameSize = frameSize;
    m_displaySourceSize = sourceSize;
    GST_DEBUG_OBJECT(pipeline(), "Display frames are %dx%d inside %dx%d buffers", frameSize.width(), frameSize.height(), sourceSize.width(), sourceSize.height());
    callOnMainThread([weakThis = ThreadSafeWeakPtr<GStreamerCapturer> { *this }, caps = GRefPtr<GstCaps>(gst_sample_get_caps(sample.get()))] {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis)
            return;
        protectedThis->forEachObserver([&caps](auto& observer) {
            observer.sourceCapsChanged(caps.get());
        });
    });
}

void GStreamerVideoCapturer::handleSample(GRefPtr<GstSample>&& capturedSample)
{
    auto sourceSize = isCapturingDisplay() ? sampleResolution(capturedSample) : IntSize { };
    if (m_frameGate)
        capturedSample = sampleReleasedThroughGate(WTF::move(capturedSample), *m_frameGate);
    auto sample = sampleWithCropApplied(sampleWithVideoMeta(WTF::move(capturedSample)));
    if (isCapturingDisplay())
        updateDisplayFrameSize(sourceSize, sample);
'''


CAMERA_OUTPUT_CAPS = 'video/x-raw, format=(string)I420, pixel-aspect-ratio=(fraction)1/1'


CAPTURE_DEVICE_MODE = r'''struct CaptureDeviceMode {
    String mimeType;
    String format;
    int width { 0 };
    int height { 0 };
    double frameRate { 0 };
};

static std::optional<CaptureDeviceMode> bestCaptureDeviceMode(const GstCaps* deviceCaps, int requestedWidth, int requestedHeight, double requestedFrameRate)
{
    static constexpr int maximumPreferredUncompressedWidth = 640;
    static constexpr int maximumPreferredUncompressedHeight = 480;
    bool prefersCompressed = requestedWidth > maximumPreferredUncompressedWidth || requestedHeight > maximumPreferredUncompressedHeight;
    double requestedAspectRatio = static_cast<double>(requestedWidth) / requestedHeight;

    auto rank = [&](const CaptureDeviceMode& mode) {
        bool covers = mode.width >= requestedWidth && mode.height >= requestedHeight;
        bool reachesFrameRate = mode.frameRate + 0.5 >= requestedFrameRate;
        bool matchesAspectRatio = std::abs(static_cast<double>(mode.width) / mode.height - requestedAspectRatio) <= requestedAspectRatio * 0.02;
        int64_t area = static_cast<int64_t>(mode.width) * mode.height;
        bool isCompressed = mode.mimeType == "image/jpeg"_s;
        return std::tuple { !covers, !reachesFrameRate, !matchesAspectRatio, covers ? area : -area, isCompressed != prefersCompressed, -mode.frameRate };
    };

    std::optional<CaptureDeviceMode> best;
    for (unsigned index = 0; index < gst_caps_get_size(deviceCaps); ++index) {
        auto* features = gst_caps_get_features(deviceCaps, index);
        if (features && !gst_caps_features_is_any(features) && !gst_caps_features_is_equal(features, GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY))
            continue;

        auto* structure = gst_caps_get_structure(deviceCaps, index);
        CaptureDeviceMode mode;
        if (gst_structure_has_name(structure, "video/x-raw")) {
            mode.mimeType = "video/x-raw"_s;
            mode.format = gstStructureGetString(structure, "format"_s).span();
        } else if (gst_structure_has_name(structure, "image/jpeg"))
            mode.mimeType = "image/jpeg"_s;
        else
            continue;

        auto width = getMaxIntValueFromStructure(structure, "width"_s);
        auto height = getMaxIntValueFromStructure(structure, "height"_s);
        if (!width || !height || *width <= 0 || *height <= 0)
            continue;

        mode.width = *width;
        mode.height = *height;
        mode.frameRate = getMaxFractionValueFromStructure(structure, "framerate"_s).value_or(0);
        if (!best || rank(mode) < rank(*best))
            best = WTF::move(mode);
    }
    return best;
}

void GStreamerVideoCapturer::reconfigure()
'''


SELECT_CAPTURE_DEVICE_MODE = r'''    const GstStructure* capsStruct = gst_caps_get_structure(m_caps.get(), 0);
    int requestedWidth = gstStructureGet<int>(capsStruct, "width"_s).value_or(1920);
    int requestedHeight = gstStructureGet<int>(capsStruct, "height"_s).value_or(1080);
    double requestedFrameRate = 24;
    int numerator = 0;
    int denominator = 1;
    if (gst_structure_get_fraction(capsStruct, "framerate", &numerator, &denominator) && numerator > 0)
        gst_util_fraction_to_double(numerator, denominator, &requestedFrameRate);

    if (requestedWidth <= 0 || requestedHeight <= 0)
        return;

    if (m_aspectRatioCrop)
        g_object_set(m_aspectRatioCrop.get(), "aspect-ratio", requestedWidth, requestedHeight, nullptr);

    GST_DEBUG_OBJECT(m_pipeline.get(), "Searching best video capture device mode for resolution %dx%d@%.3f", requestedWidth, requestedHeight, requestedFrameRate);

    auto mode = bestCaptureDeviceMode(deviceCaps.get(), requestedWidth, requestedHeight, requestedFrameRate);
    if (!mode) {
        GST_WARNING_OBJECT(m_pipeline.get(), "No usable video capture device mode in %" GST_PTR_FORMAT, deviceCaps.get());
        return;
    }

    GRefPtr caps = adoptGRef(gst_caps_new_simple(mode->mimeType.ascii().data(), "width", G_TYPE_INT, mode->width, "height", G_TYPE_INT, mode->height, nullptr));
    if (!mode->format.isEmpty())
        gst_caps_set_simple(caps.get(), "format", G_TYPE_STRING, mode->format.ascii().data(), nullptr);

    GST_INFO_OBJECT(m_pipeline.get(), "Setting video capture device caps to %" GST_PTR_FORMAT, caps.get());
    g_object_set(m_videoSrcMIMETypeFilter.get(), "caps", caps.get(), nullptr);
'''


CROP_TO_REQUESTED_ASPECT_RATIO = r'''    m_aspectRatioCrop = gst_element_factory_make("aspectratiocrop", nullptr);
    GRefPtr<GstElement> decodedVideoSink = videoConvert;
    if (m_aspectRatioCrop) {
        gst_bin_add(GST_BIN_CAST(bin), m_aspectRatioCrop.get());
        gst_element_link(m_aspectRatioCrop.get(), videoConvert.get());
        decodedVideoSink = m_aspectRatioCrop;
    } else
        GST_WARNING("aspectratiocrop is unavailable, camera frames are scaled without cropping");

    GRefPtr sinkPad = adoptGRef(gst_element_get_static_pad(decodedVideoSink.get(), "sink"));
'''


def capturer_patch(text):
    text = replace_once(
        text,
        "#include <gst/app/gstappsink.h>\n",
        "#include <gst/app/gstappsink.h>\n#include <gst/video/video.h>\n#include <tuple>\n",
    )
    text = replace_once(
        text,
        "#include \"VideoFrameGStreamer.h\"\n",
        "#include \"GStreamerVideoFrameConverter.h\"\n#include \"VideoFrameGStreamer.h\"\n",
    )
    text = replace_once(
        text,
        "    if (!gst_check_version(1, 28, 0)) {\n"
        "        // https://gitlab.freedesktop.org/gstreamer/gst-plugins-base/issues/97#note_56575\n"
        '        g_object_set(videorate, "drop-only", TRUE, "average-period", UINT64_C(1), nullptr);\n'
        "    }\n",
        '    g_object_set(videorate, "drop-only", TRUE, "average-period", UINT64_C(1), nullptr);\n',
    )
    selection_start = text.index("    struct MimeTypeSelector {\n")
    selection_end = text.index('    g_object_set(m_videoSrcMIMETypeFilter.get(), "caps", caps.get(), nullptr);\n', selection_start)
    selection_end += len('    g_object_set(m_videoSrcMIMETypeFilter.get(), "caps", caps.get(), nullptr);\n')
    if text.count("    struct MimeTypeSelector {\n") != 1:
        raise RuntimeError("GStreamerVideoCapturer::reconfigure changed upstream")
    text = text[:selection_start] + SELECT_CAPTURE_DEVICE_MODE + text[selection_end:]
    text = replace_once(
        text,
        "void GStreamerVideoCapturer::reconfigure()\n",
        CAPTURE_DEVICE_MODE,
    )
    text = replace_once(
        text,
        '    : GStreamerCapturer(WTF::move(device), adoptGRef(gst_caps_new_empty_simple("video/x-raw")))\n',
        '    : GStreamerCapturer(WTF::move(device), adoptGRef(gst_caps_from_string("' + CAMERA_OUTPUT_CAPS + '")))\n',
    )
    text = replace_once(
        text,
        '    GRefPtr sinkPad = adoptGRef(gst_element_get_static_pad(videoConvert.get(), "sink"));\n',
        CROP_TO_REQUESTED_ASPECT_RATIO,
    )
    text = replace_once(
        text,
        "    if (disconnectSignals)\n        m_videoSrcMIMETypeFilter = nullptr;\n",
        "    if (disconnectSignals) {\n        m_videoSrcMIMETypeFilter = nullptr;\n        m_aspectRatioCrop = nullptr;\n    }\n",
    )
    text = replace_once(
        text,
        "    GStreamerCapturer::setupPipeline();\n",
        "    GStreamerCapturer::setupPipeline();\n"
        '    g_object_set(m_sink.get(), "sync", FALSE, nullptr);\n',
    )
    text = replace_once(
        text,
        "GStreamerVideoCapturer::GStreamerVideoCapturer(const PipeWireCaptureDevice& device)\n"
        "    : GStreamerCapturer(device)\n"
        "{\n",
        "GStreamerVideoCapturer::GStreamerVideoCapturer(const PipeWireCaptureDevice& device)\n"
        "    : GStreamerCapturer(device)\n"
        "    , m_frameGate(PipeWireFrameGate::create())\n"
        "{\n",
    )
    text = replace_once(
        text,
        "void GStreamerVideoCapturer::tearDown(bool disconnectSignals)\n"
        "{\n"
        "    GStreamerCapturer::tearDown(disconnectSignals);\n",
        "void GStreamerVideoCapturer::tearDown(bool disconnectSignals)\n"
        "{\n"
        "    if (RefPtr frameGate = m_frameGate) {\n"
        "        frameGate->close([&] {\n"
        "            GStreamerCapturer::tearDown(disconnectSignals);\n"
        "        });\n"
        "    } else\n"
        "        GStreamerCapturer::tearDown(disconnectSignals);\n",
    )
    text = replace_once(
        text,
        "    auto size = this->size();\n    VideoFrameGStreamer::CreateOptions options(WTF::move(size));\n",
        "    auto size = isCapturingDisplay() ? sampleResolution(sample) : this->size();\n    VideoFrameGStreamer::CreateOptions options(WTF::move(size));\n",
    )
    return replace_once(
        text,
        "void GStreamerVideoCapturer::handleSample(GRefPtr<GstSample>&& sample)\n{\n",
        SAMPLE_WITH_VIDEO_META,
    )


PIPEWIRE_FRAME_GATE = r"""class PipeWireFrameGate final : public ThreadSafeRefCounted<PipeWireFrameGate> {
public:
    static Ref<PipeWireFrameGate> create() { return adoptRef(*new PipeWireFrameGate); }

    void release(GRefPtr<GstBuffer>&& buffer)
    {
        if (m_closeLock.tryLock()) {
            Locker locker { AdoptLock, m_closeLock };
            buffer = nullptr;
            return;
        }
        Locker locker { m_pendingLock };
        m_pending.append(WTF::move(buffer));
    }

    void close(NOESCAPE const Function<void()>& closeSource)
    {
        Locker locker { m_closeLock };
        closeSource();
        Vector<GRefPtr<GstBuffer>> pending;
        {
            Locker pendingLocker { m_pendingLock };
            pending = std::exchange(m_pending, { });
        }
    }

private:
    PipeWireFrameGate() = default;

    Lock m_closeLock;
    Lock m_pendingLock;
    Vector<GRefPtr<GstBuffer>> m_pending WTF_GUARDED_BY_LOCK(m_pendingLock);
};

"""


def capturer_header_patch(text):
    text = replace_once(
        text,
        "class VideoFrameGStreamer;\n\n",
        "class VideoFrameGStreamer;\n\n" + PIPEWIRE_FRAME_GATE,
    )
    text = replace_once(
        text,
        "#include \"IntSize.h\"\n",
        "#include \"IntSize.h\"\n#include <wtf/Function.h>\n#include <wtf/Lock.h>\n#include <wtf/ThreadSafeRefCounted.h>\n#include <wtf/Vector.h>\n",
    )
    text = replace_once(
        text,
        "    void handleSample(GRefPtr<GstSample>&&);\n",
        "    void handleSample(GRefPtr<GstSample>&&);\n    void updateDisplayFrameSize(const IntSize&, const GRefPtr<GstSample>&);\n",
    )
    return replace_once(
        text,
        "    IntSize m_size;\n};\n",
        "    IntSize m_size;\n    IntSize m_displaySourceSize;\n    IntSize m_displayFrameSize;\n    RefPtr<PipeWireFrameGate> m_frameGate;\n    GRefPtr<GstElement> m_aspectRatioCrop;\n};\n",
    )


def video_capture_source_base_patch(text):
    return replace_once(
        text,
        "    m_currentPreset = match->encodingPreset;\n    auto newSize = match->encodingPreset->size();\n",
        "    m_currentPreset = match->encodingPreset;\n    auto newSize = match->requestedSize.isEmpty() ? match->encodingPreset->size() : match->requestedSize;\n",
    )


def mock_video_source_patch(text):
    return replace_once(
        text,
        "    MockRealtimeVideoSource::setSizeFrameRateAndZoom(constraints);\n\n"
        "    if (!constraints.width || !constraints.height)\n"
        "        return;\n\n"
        "    m_capturer->setSize({ *constraints.width, *constraints.height });\n",
        "    MockRealtimeVideoSource::setSizeFrameRateAndZoom(constraints);\n\n"
        "    if (!size().isEmpty())\n"
        "        m_capturer->setSize(size());\n",
    )


PATCHES = {
    CAPTURER: capturer_patch,
    CAPTURER_HEADER: capturer_header_patch,
    VIDEO_CAPTURE_SOURCE_BASE: video_capture_source_base_patch,
    MOCK_VIDEO_SOURCE: mock_video_source_patch,
}

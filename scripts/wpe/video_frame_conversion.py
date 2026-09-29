from wpe.patch_support import replace_once


GRAPHICS = "Source/WebCore/platform/graphics/gstreamer/"
CONVERTER = GRAPHICS + "GStreamerVideoFrameConverter.cpp"
CONVERTER_HEADER = GRAPHICS + "GStreamerVideoFrameConverter.h"
VIDEO_FRAME = GRAPHICS + "VideoFrameGStreamer.cpp"
VIDEO_FRAME_HEADER = GRAPHICS + "VideoFrameGStreamer.h"
WEBGL_CONTEXT = "Source/WebCore/html/canvas/WebGLRenderingContextBase.cpp"
REMOTE_GL_PROXY = "Source/WebKit/WebProcess/GPU/graphics/RemoteGraphicsContextGLProxy.cpp"
REMOTE_GL = "Source/WebKit/GPUProcess/graphics/RemoteGraphicsContextGL.cpp"
REMOTE_GL_PROXY_GBM = "Source/WebKit/WebProcess/GPU/graphics/gbm/RemoteGraphicsContextGLProxyGBM.cpp"
WEBCODECS = "Source/WebCore/Modules/webcodecs/"
FRAME_IDL = WEBCODECS + "WebCodecsVideoFrame.idl"
FRAME_HEADER = WEBCODECS + "WebCodecsVideoFrame.h"
FRAME_IMPLEMENTATION = WEBCODECS + "WebCodecsVideoFrame.cpp"
FRAME_ALGORITHMS = WEBCODECS + "WebCodecsVideoFrameAlgorithms.cpp"


def converter_header_patch(text):
    text = replace_once(
        text,
        "#include <wtf/Forward.h>\n",
        "#include <wtf/Forward.h>\n#include <wtf/Lock.h>\n#include <wtf/MonotonicTime.h>\n#include <wtf/Vector.h>\n",
    )
    return replace_span(
        text,
        "    class Pipeline {\n",
        "    std::unique_ptr<RunLoop::Timer> m_releaseUnusedDMABufMemoryPipelineTimer;\n#endif\n};\n",
        CONVERTER_MEMBERS,
    )


CONVERTER_MEMBERS = r"""    class Pipeline {
        WTF_MAKE_TZONE_ALLOCATED(Pipeline);
    public:
        Pipeline();
        ~Pipeline();

        GRefPtr<GstSample> run(const GRefPtr<GstSample>&, GstCaps*);

    private:
        GRefPtr<GstElement> m_pipeline;
        GRefPtr<GstElement> m_src;
        GRefPtr<GstElement> m_sink;
    };

#if USE(GSTREAMER_GL)
    class GPUPipeline {
        WTF_MAKE_TZONE_ALLOCATED(GPUPipeline);
    public:
        static std::unique_ptr<GPUPipeline> create(GRefPtr<GstCaps>&& inputCaps, GRefPtr<GstCaps>&& outputCaps);
        ~GPUPipeline();

        bool matches(GstCaps* inputCaps, GstCaps* outputCaps) const;
        bool isUsable() const { return m_isUsable; }
        MonotonicTime failureTime() const { return m_failureTime; }
        GRefPtr<GstSample> run(GstBuffer*);

        MonotonicTime lastUse;

    private:
        GPUPipeline(GRefPtr<GstCaps>&&, GRefPtr<GstCaps>&&);
        bool initialize();
        void tearDown();

        GRefPtr<GstCaps> m_inputCaps;
        GRefPtr<GstCaps> m_outputCaps;
        GRefPtr<GstElement> m_pipeline;
        GRefPtr<GstElement> m_sink;
        GRefPtr<GstPad> m_sourcePad;
        bool m_isUsable { false };
        MonotonicTime m_failureTime;
    };

    GRefPtr<GstSample> convertOnGPU(const GRefPtr<GstSample>&, GstCaps*) WTF_REQUIRES_LOCK(m_lock);
#endif

    Pipeline& ensurePipeline() WTF_REQUIRES_LOCK(m_lock);
    void scheduleReleaseOfUnusedPipelines();
    void releaseUnusedPipelinesTimerFired();

    Lock m_lock;
    std::unique_ptr<Pipeline> m_systemMemoryPipeline WTF_GUARDED_BY_LOCK(m_lock);
    MonotonicTime m_systemMemoryPipelineLastUse WTF_GUARDED_BY_LOCK(m_lock);
#if USE(GSTREAMER_GL)
    Vector<std::unique_ptr<GPUPipeline>> m_gpuPipelines WTF_GUARDED_BY_LOCK(m_lock);
#endif
    RunLoop::Timer m_releaseUnusedPipelinesTimer;
};
"""


SYSTEM_MEMORY_CONVERSION = r'''static bool isSystemMemoryRawVideo(GstCaps* caps)
{
    if (!caps || gst_caps_get_size(caps) != 1)
        return false;

    auto* features = gst_caps_get_features(caps, 0);
    if (features && !gst_caps_features_is_equal(features, GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY))
        return false;

    auto* structure = gst_caps_get_structure(caps, 0);
    auto* format = gst_structure_get_string(structure, "format");
    return gst_structure_has_name(structure, "video/x-raw") && format && g_strcmp0(format, "DMA_DRM");
}

struct SystemMemoryVideoConverter {
    GstVideoInfo inputInfo;
    GstVideoInfo outputInfo;
    GUniquePtr<GstVideoConverter> converter;
};

static GRefPtr<GstSample> convertInSystemMemory(const GRefPtr<GstSample>& sample, GstCaps* destinationCaps)
{
    auto* inputCaps = gst_sample_get_caps(sample.get());
    auto* inputBuffer = gst_sample_get_buffer(sample.get());
    if (!inputBuffer || !isSystemMemoryRawVideo(inputCaps) || !isSystemMemoryRawVideo(destinationCaps))
        return nullptr;

    GstVideoInfo inputInfo;
    GstVideoInfo outputInfo;
    if (!gst_video_info_from_caps(&inputInfo, inputCaps) || !gst_video_info_from_caps(&outputInfo, destinationCaps))
        return nullptr;

    static thread_local SystemMemoryVideoConverter cache;
    if (!cache.converter || !gst_video_info_is_equal(&cache.inputInfo, &inputInfo) || !gst_video_info_is_equal(&cache.outputInfo, &outputInfo)) {
        cache.converter.reset(gst_video_converter_new(&inputInfo, &outputInfo, nullptr));
        cache.inputInfo = inputInfo;
        cache.outputInfo = outputInfo;
    }
    if (!cache.converter)
        return nullptr;

    GstVideoFrame inputFrame;
    if (!gst_video_frame_map(&inputFrame, &inputInfo, inputBuffer, GST_MAP_READ))
        return nullptr;

    GRefPtr outputBuffer = adoptGRef(gst_buffer_new_allocate(nullptr, GST_VIDEO_INFO_SIZE(&outputInfo), nullptr));
    GstVideoFrame outputFrame;
    bool mapped = outputBuffer && gst_video_frame_map(&outputFrame, &outputInfo, outputBuffer.get(), GST_MAP_WRITE);
    if (mapped) {
        gst_video_converter_frame(cache.converter.get(), &inputFrame, &outputFrame);
        gst_video_frame_unmap(&outputFrame);
    }
    gst_video_frame_unmap(&inputFrame);
    if (!mapped)
        return nullptr;

    gst_buffer_copy_into(outputBuffer.get(), inputBuffer, GST_BUFFER_COPY_TIMESTAMPS, 0, -1);
    return adoptGRef(gst_sample_new(outputBuffer.get(), destinationCaps, gst_sample_get_segment(sample.get()), nullptr));
}

static GRefPtr<GstSample> convertMappedInSystemMemory(const GRefPtr<GstSample>& sample, GstCaps* destinationCaps)
{
    auto* caps = gst_sample_get_caps(sample.get());
    auto* buffer = gst_sample_get_buffer(sample.get());
    if (!caps || !buffer)
        return nullptr;

    GstVideoInfo info;
#if GST_CHECK_VERSION(1, 24, 0)
    if (gst_video_is_dma_drm_caps(caps)) {
        GstVideoInfoDmaDrm drmInfo;
        if (!gst_video_info_dma_drm_from_caps(&drmInfo, caps) || drmInfo.drm_modifier || !gst_video_info_dma_drm_to_video_info(&drmInfo, &info))
            return nullptr;
    } else
#endif
    if (!gst_video_info_from_caps(&info, caps))
        return nullptr;

    GRefPtr systemMemoryCaps = adoptGRef(gst_video_info_to_caps(&info));
    GRefPtr systemMemorySample = adoptGRef(gst_sample_new(buffer, systemMemoryCaps.get(), gst_sample_get_segment(sample.get()), nullptr));
    return convertInSystemMemory(systemMemorySample, destinationCaps);
}

#if USE(GSTREAMER_GL)
static constexpr size_t maximumGPUPipelines = 4;
static constexpr Seconds gpuPipelineRetryDelay = 5_s;

static bool isGPUMemory(GstCaps* caps)
{
    if (!caps || !gst_caps_get_size(caps))
        return false;
    auto* features = gst_caps_get_features(caps, 0);
    return features && (gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_DMABUF) || gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_GL_MEMORY));
}

static GRefPtr<GstCaps> capsWithoutFrameRate(GstCaps* caps)
{
    GRefPtr result = adoptGRef(gst_caps_copy(caps));
    for (unsigned index = 0; index < gst_caps_get_size(result.get()); ++index)
        gst_structure_remove_fields(gst_caps_get_structure(result.get(), index), "framerate", "max-framerate", nullptr);
    return result;
}

static bool isGLColorConvertOutputFormat(const char* format)
{
    static constexpr std::array<ASCIILiteral, 12> formats { "I420"_s, "YV12"_s, "NV12"_s, "NV21"_s, "Y444"_s, "Y42B"_s, "A420"_s, "AYUV"_s, "RGBA"_s, "BGRA"_s, "RGBx"_s, "BGRx"_s };
    if (!format)
        return false;
    auto name = CStringView::unsafeFromUTF8(format);
    return std::ranges::any_of(formats, [&](auto candidate) { return name == candidate; });
}

GStreamerVideoFrameConverter::GPUPipeline::GPUPipeline(GRefPtr<GstCaps>&& inputCaps, GRefPtr<GstCaps>&& outputCaps)
    : m_inputCaps(WTF::move(inputCaps))
    , m_outputCaps(WTF::move(outputCaps))
{
}

std::unique_ptr<GStreamerVideoFrameConverter::GPUPipeline> GStreamerVideoFrameConverter::GPUPipeline::create(GRefPtr<GstCaps>&& inputCaps, GRefPtr<GstCaps>&& outputCaps)
{
    std::unique_ptr<GPUPipeline> pipeline(new GPUPipeline(WTF::move(inputCaps), WTF::move(outputCaps)));
    pipeline->m_isUsable = pipeline->initialize();
    if (pipeline->m_isUsable)
        GST_INFO("Built a GPU pipeline converting %" GST_PTR_FORMAT " to %" GST_PTR_FORMAT, pipeline->m_inputCaps.get(), pipeline->m_outputCaps.get());
    else {
        GST_WARNING("Unable to build a GPU pipeline converting %" GST_PTR_FORMAT " to %" GST_PTR_FORMAT, pipeline->m_inputCaps.get(), pipeline->m_outputCaps.get());
        pipeline->m_failureTime = MonotonicTime::now();
        pipeline->tearDown();
    }
    return pipeline;
}

GStreamerVideoFrameConverter::GPUPipeline::~GPUPipeline()
{
    tearDown();
}

void GStreamerVideoFrameConverter::GPUPipeline::tearDown()
{
    m_isUsable = false;
    if (auto pad = std::exchange(m_sourcePad, nullptr))
        gst_pad_set_active(pad.get(), FALSE);
    if (auto pipeline = std::exchange(m_pipeline, nullptr))
        gst_element_set_state(pipeline.get(), GST_STATE_NULL);
    m_sink = nullptr;
}

bool GStreamerVideoFrameConverter::GPUPipeline::matches(GstCaps* inputCaps, GstCaps* outputCaps) const
{
    return gst_caps_is_equal(m_inputCaps.get(), inputCaps) && gst_caps_is_equal(m_outputCaps.get(), outputCaps);
}

bool GStreamerVideoFrameConverter::GPUPipeline::initialize()
{
    auto* outputStructure = gst_caps_get_structure(m_outputCaps.get(), 0);
    auto width = gstStructureGet<int>(outputStructure, "width"_s);
    auto height = gstStructureGet<int>(outputStructure, "height"_s);
    auto* format = gst_structure_get_string(outputStructure, "format");
    if (!width || !height || *width <= 0 || *height <= 0 || !format)
        return false;

    GRefPtr<GstElement> upload = makeGStreamerElement("glupload"_s);
    GRefPtr<GstElement> inputConvert = makeGStreamerElement("glcolorconvert"_s);
    GRefPtr<GstElement> scale = makeGStreamerElement("glcolorscale"_s);
    GRefPtr<GstElement> outputConvert = makeGStreamerElement("glcolorconvert"_s);
    GRefPtr<GstElement> glCapsFilter = makeGStreamerElement("capsfilter"_s);
    GRefPtr<GstElement> download = makeGStreamerElement("gldownload"_s);
    GRefPtr<GstElement> videoConvert = makeGStreamerElement("videoconvert"_s);
    m_sink = makeGStreamerElement("appsink"_s);
    m_pipeline = gst_pipeline_new(nullptr);
    if (!upload || !inputConvert || !scale || !outputConvert || !glCapsFilter || !download || !videoConvert || !m_sink || !m_pipeline)
        return false;

    g_object_set(m_sink.get(), "sync", FALSE, "async", FALSE, "enable-last-sample", FALSE, "max-buffers", 1, "caps", m_outputCaps.get(), nullptr);
    GRefPtr glCaps = adoptGRef(gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, isGLColorConvertOutputFormat(format) ? format : "RGBA", "width", G_TYPE_INT, *width, "height", G_TYPE_INT, *height, "texture-target", G_TYPE_STRING, "2D", nullptr));
    if (auto* colorimetry = gst_structure_get_string(outputStructure, "colorimetry"))
        gst_caps_set_simple(glCaps.get(), "colorimetry", G_TYPE_STRING, colorimetry, nullptr);
    gst_caps_set_features(glCaps.get(), 0, gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_GL_MEMORY, nullptr));
    g_object_set(glCapsFilter.get(), "caps", glCaps.get(), nullptr);

    gst_bin_add_many(GST_BIN_CAST(m_pipeline.get()), upload.get(), inputConvert.get(), scale.get(), outputConvert.get(), glCapsFilter.get(), download.get(), videoConvert.get(), m_sink.get(), nullptr);
    if (!gst_element_link_many(upload.get(), inputConvert.get(), scale.get(), outputConvert.get(), glCapsFilter.get(), download.get(), videoConvert.get(), m_sink.get(), nullptr))
        return false;

    static ASCIILiteral displayContextType = ASCIILiteral::fromLiteralUnsafe(GST_GL_DISPLAY_CONTEXT_TYPE);
    if (!setGstElementGLContext(m_pipeline.get(), displayContextType) || !setGstElementGLContext(m_pipeline.get(), "gst.gl.app_context"_s))
        return false;

    GRefPtr bus = adoptGRef(gst_pipeline_get_bus(GST_PIPELINE_CAST(m_pipeline.get())));
    gst_bus_set_flushing(bus.get(), TRUE);

    m_sourcePad = gst_pad_new("src", GST_PAD_SRC);
    GRefPtr uploadSinkPad = adoptGRef(gst_element_get_static_pad(upload.get(), "sink"));
    if (gst_pad_link(m_sourcePad.get(), uploadSinkPad.get()) != GST_PAD_LINK_OK || !gst_pad_set_active(m_sourcePad.get(), TRUE))
        return false;

    if (gst_element_set_state(m_pipeline.get(), GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        return false;

    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    return gst_pad_push_event(m_sourcePad.get(), gst_event_new_stream_start("webkit-video-frame-converter"))
        && gst_pad_push_event(m_sourcePad.get(), gst_event_new_caps(m_inputCaps.get()))
        && gst_pad_push_event(m_sourcePad.get(), gst_event_new_segment(&segment));
}

GRefPtr<GstSample> GStreamerVideoFrameConverter::GPUPipeline::run(GstBuffer* buffer)
{
    if (!m_isUsable)
        return nullptr;

    auto result = gst_pad_push(m_sourcePad.get(), gst_buffer_ref(buffer));
    GRefPtr<GstSample> sample;
    if (result == GST_FLOW_OK)
        sample = adoptGRef(gst_app_sink_try_pull_sample(GST_APP_SINK_CAST(m_sink.get()), 0));
    auto* outputBuffer = sample ? gst_sample_get_buffer(sample.get()) : nullptr;
    bool downloaded = !!outputBuffer;
    for (unsigned index = 0; downloaded && index < gst_buffer_n_memory(outputBuffer); ++index) {
        auto* memory = gst_buffer_peek_memory(outputBuffer, index);
        GstMapInfo mapInfo;
        downloaded = gst_memory_map(memory, &mapInfo, GST_MAP_READ);
        if (downloaded)
            gst_memory_unmap(memory, &mapInfo);
    }
    if (downloaded)
        return sample;

    GST_WARNING("Converting %" GST_PTR_FORMAT " to %" GST_PTR_FORMAT " on the GPU failed: %s", m_inputCaps.get(), m_outputCaps.get(), result == GST_FLOW_OK ? "the converted frame could not be read back" : gst_flow_get_name(result));
    m_failureTime = MonotonicTime::now();
    tearDown();
    return nullptr;
}

GRefPtr<GstSample> GStreamerVideoFrameConverter::convertOnGPU(const GRefPtr<GstSample>& sample, GstCaps* destinationCaps)
{
    auto* buffer = gst_sample_get_buffer(sample.get());
    auto* destinationFeatures = gst_caps_get_size(destinationCaps) ? gst_caps_get_features(destinationCaps, 0) : nullptr;
    if (!buffer || (destinationFeatures && !gst_caps_features_is_equal(destinationFeatures, GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY)))
        return nullptr;

    scheduleReleaseOfUnusedPipelines();
    auto inputCaps = capsWithoutFrameRate(gst_sample_get_caps(sample.get()));
    auto outputCaps = capsWithoutFrameRate(destinationCaps);
    auto now = MonotonicTime::now();
    auto index = m_gpuPipelines.findIf([&](auto& pipeline) {
        return pipeline->matches(inputCaps.get(), outputCaps.get());
    });
    if (index != notFound && !m_gpuPipelines[index]->isUsable() && now - m_gpuPipelines[index]->failureTime() >= gpuPipelineRetryDelay) {
        m_gpuPipelines.removeAt(index);
        index = notFound;
    }
    if (index == notFound) {
        if (m_gpuPipelines.size() >= maximumGPUPipelines) {
            size_t oldest = 0;
            for (size_t candidate = 1; candidate < m_gpuPipelines.size(); ++candidate) {
                if (m_gpuPipelines[candidate]->lastUse < m_gpuPipelines[oldest]->lastUse)
                    oldest = candidate;
            }
            m_gpuPipelines.removeAt(oldest);
        }
        m_gpuPipelines.append(GPUPipeline::create(WTF::move(inputCaps), WTF::move(outputCaps)));
        index = m_gpuPipelines.size() - 1;
    }

    auto& pipeline = *m_gpuPipelines[index];
    pipeline.lastUse = now;
    if (auto result = pipeline.run(buffer))
        return result;
    return convertMappedInSystemMemory(sample, destinationCaps);
}
#endif

GRefPtr<GstSample> GStreamerVideoFrameConverter::convert('''


ENSURE_PIPELINE = r'''GStreamerVideoFrameConverter::GStreamerVideoFrameConverter()
    : m_releaseUnusedPipelinesTimer(RunLoop::mainSingleton(), "GStreamerVideoFrameConverter::ReleaseUnusedPipelinesTimer"_s, this, &GStreamerVideoFrameConverter::releaseUnusedPipelinesTimerFired)
{
    ensureGStreamerInitialized();
    GST_DEBUG_CATEGORY_INIT(webkit_gst_video_frame_converter_debug, "webkitvideoframeconverter", 0, "WebKit GStreamer Video Frame Converter");
    m_releaseUnusedPipelinesTimer.setPriority(RunLoopSourcePriority::ReleaseUnusedResourcesTimer);
}

void GStreamerVideoFrameConverter::scheduleReleaseOfUnusedPipelines()
{
    if (!m_releaseUnusedPipelinesTimer.isActive())
        m_releaseUnusedPipelinesTimer.startOneShot(s_releaseUnusedPipelinesTimerInterval);
}

GStreamerVideoFrameConverter::Pipeline& GStreamerVideoFrameConverter::ensurePipeline()
{
    scheduleReleaseOfUnusedPipelines();
    if (!m_systemMemoryPipeline)
        m_systemMemoryPipeline = makeUnique<Pipeline>();
    m_systemMemoryPipelineLastUse = MonotonicTime::now();
    return *m_systemMemoryPipeline;
}

'''


RELEASE_PIPELINES = r'''void GStreamerVideoFrameConverter::releaseUnusedPipelinesTimerFired()
{
    Locker locker { m_lock };
    auto idleSince = MonotonicTime::now() - s_releaseUnusedPipelinesTimerInterval;
    if (m_systemMemoryPipeline && m_systemMemoryPipelineLastUse <= idleSince)
        m_systemMemoryPipeline = nullptr;
    bool hasPipelines = !!m_systemMemoryPipeline;
#if USE(GSTREAMER_GL)
    m_gpuPipelines.removeAllMatching([&](auto& pipeline) {
        return pipeline->lastUse <= idleSince;
    });
    hasPipelines |= !m_gpuPipelines.isEmpty();
#endif
    if (hasPipelines)
        m_releaseUnusedPipelinesTimer.startOneShot(s_releaseUnusedPipelinesTimerInterval);
}

#undef GST_CAT_DEFAULT
'''


def replace_span(text, start, end, replacement):
    if text.count(start) != 1:
        raise RuntimeError("The pinned source does not match the video frame conversion patch: " + start[:80])
    begin = text.index(start)
    finish = text.index(end, begin) + len(end)
    return text[:begin] + replacement + text[finish:]


PIPELINE = r"""GStreamerVideoFrameConverter::Pipeline::Pipeline()
    : m_src(makeGStreamerElement("appsrc"_s))
    , m_sink(makeGStreamerElement("appsink"_s))
{
    g_object_set(m_sink.get(), "enable-last-sample", FALSE, "max-buffers", 1, nullptr);
    auto videoConvert = createVideoConvertScaleElement();
    RELEASE_ASSERT(videoConvert);
    m_pipeline = gst_element_factory_make("pipeline", "video-frame-converter");
    gst_bin_add_many(GST_BIN_CAST(m_pipeline.get()), m_src.get(), videoConvert.get(), m_sink.get(), nullptr);
    gst_element_link_many(m_src.get(), videoConvert.get(), m_sink.get(), nullptr);
}

GStreamerVideoFrameConverter::Pipeline::~Pipeline() = default;
"""


CONVERTED_VIDEO_META = r"""    auto structure = gst_caps_get_structure(destinationCaps.get(), 0);
    auto width = gstStructureGet<int>(structure, "width"_s);
    RELEASE_ASSERT(width && *width);
    auto height = gstStructureGet<int>(structure, "height"_s);
    RELEASE_ASSERT(height && *height);
    auto formatString = gstStructureGetString(structure, "format"_s);
    RELEASE_ASSERT(!formatString.isEmpty());
    auto format = gst_video_format_from_string(formatString.utf8());
    auto* videoMeta = gst_buffer_get_video_meta(writableBuffer.get());
    if (!videoMeta || videoMeta->format != format || videoMeta->width != static_cast<guint>(*width) || videoMeta->height != static_cast<guint>(*height)) {
        if (videoMeta)
            gst_buffer_remove_meta(writableBuffer.get(), GST_META_CAST(videoMeta));
        gst_buffer_add_video_meta(writableBuffer.get(), GST_VIDEO_FRAME_FLAG_NONE, format, *width, *height);
    }
"""

COPY_PLANE = r"""static bool copyPlane(std::span<uint8_t>& destination, const std::span<uint8_t>& source, uint64_t sourceStride, const ComputedPlaneLayout& spanPlaneLayout)
{
    uint64_t sourceOffset = spanPlaneLayout.sourceTop * sourceStride;
    sourceOffset += spanPlaneLayout.sourceLeftBytes;
    uint64_t destinationOffset = spanPlaneLayout.destinationOffset;
    uint64_t rowBytes = spanPlaneLayout.sourceWidthBytes;
    for (size_t rowIndex = 0; rowIndex < spanPlaneLayout.sourceHeight; ++rowIndex) {
        if (sourceOffset + rowBytes > source.size()) {
            GST_ERROR("Computed sourceOffset doesn't fit in source plane");
            return false;
        }
        if (destinationOffset + rowBytes > destination.size()) {
            GST_ERROR("Computed destinationOffset doesn't fit in destination plane");
            return false;
        }

        memcpySpan(destination.subspan(destinationOffset, rowBytes), source.subspan(sourceOffset, rowBytes));
        sourceOffset += sourceStride;
        destinationOffset += spanPlaneLayout.destinationStride;
    }
    return true;
}
"""


COPY_FORMATS = r"""static GstVideoFormat gstVideoFormatForCopy(VideoPixelFormat format)
{
    switch (format) {
    case VideoPixelFormat::I420:
        return GST_VIDEO_FORMAT_I420;
    case VideoPixelFormat::I420A:
        return GST_VIDEO_FORMAT_A420;
    case VideoPixelFormat::I422:
        return GST_VIDEO_FORMAT_Y42B;
    case VideoPixelFormat::I444:
        return GST_VIDEO_FORMAT_Y444;
    case VideoPixelFormat::NV12:
        return GST_VIDEO_FORMAT_NV12;
    case VideoPixelFormat::RGBA:
        return GST_VIDEO_FORMAT_RGBA;
    case VideoPixelFormat::RGBX:
        return GST_VIDEO_FORMAT_RGBx;
    case VideoPixelFormat::BGRA:
        return GST_VIDEO_FORMAT_BGRA;
    case VideoPixelFormat::BGRX:
        return GST_VIDEO_FORMAT_BGRx;
    }
    return GST_VIDEO_FORMAT_UNKNOWN;
}

static bool copiesWithoutConversion(GstVideoFormat source, GstVideoFormat target)
{
    if (source == target)
        return true;
    return (source == GST_VIDEO_FORMAT_BGRA && target == GST_VIDEO_FORMAT_BGRx) || (source == GST_VIDEO_FORMAT_RGBA && target == GST_VIDEO_FORMAT_RGBx);
}

"""


COPY_INPUT = r"""    auto targetFormat = gstVideoFormatForCopy(pixelFormat);
    GRefPtr<GstSample> inputSample;
    if (!copiesWithoutConversion(static_cast<GstVideoFormat>(self.pixelFormat()), targetFormat)) {
        const auto& info = self.info();
        inputSample = self.convert(targetFormat, { static_cast<int>(GST_VIDEO_INFO_WIDTH(&info)), static_cast<int>(GST_VIDEO_INFO_HEIGHT(&info)) });
    } else {
        switch (self.memoryType()) {
        case VideoFrameGStreamer::MemoryType::Unsupported:
            callback({ });
            return;
        case VideoFrameGStreamer::MemoryType::System:
            inputSample = self.sample();
            break;
#if USE(GSTREAMER_GL)
        case VideoFrameGStreamer::MemoryType::GL:
#if USE(GBM)
        case VideoFrameGStreamer::MemoryType::DMABuf:
#endif
            inputSample = self.downloadSample();
            break;
#endif
        }
    }

    if (!inputSample) {"""


COPY_PLANES = r"""    unsigned planeCount = GST_VIDEO_INFO_N_PLANES(inputFrame.info());
    if (computedPlaneLayout.size() != planeCount) {
        GST_WARNING("The frame has %u planes but the copy layout has %zu", planeCount, computedPlaneLayout.size());
        callback({ });
        return;
    }

    Vector<PlaneLayout> planeLayouts;
    planeLayouts.reserveInitialCapacity(planeCount);
    for (unsigned plane = 0; plane < planeCount; ++plane) {
        const auto& layout = computedPlaneLayout[plane];
        if (!copyPlane(destination, inputFrame.planeData(plane), inputFrame.planeStride(plane), layout)) {
            callback({ });
            return;
        }
        planeLayouts.append({ layout.destinationOffset, layout.destinationStride });
    }
    callback(WTF::move(planeLayouts));
}
"""


BOTTOM_UP_NATIVE_IMAGE = r"""RefPtr<VideoFrameGStreamer> VideoFrameGStreamer::createFromBottomUpNativeImage(NativeImage& image, double frameRate)
{
    ensureVideoFrameDebugCategoryInitialized();
    auto size = image.size();
    const auto& platformImage = image.platformImage();
    if (!platformImage || size.isEmpty() || platformImage->width() != size.width() || platformImage->height() != size.height())
        return nullptr;

    GST_DEBUG("Copying a %dx%d WebGL surface into a video frame through the CPU", size.width(), size.height());
    SkPixmap pixmap;
    bool canPeek = !platformImage->isTextureBacked() && platformImage->peekPixels(&pixmap) && (pixmap.colorType() == kRGBA_8888_SkColorType || pixmap.colorType() == kBGRA_8888_SkColorType) && pixmap.alphaType() != kPremul_SkAlphaType;
    auto pixelFormat = canPeek && pixmap.colorType() == kRGBA_8888_SkColorType ? PixelFormat::RGBA8 : PixelFormat::BGRA8;
    RefPtr pixelBuffer = ByteArrayPixelBuffer::tryCreate({ AlphaPremultiplication::Unpremultiplied, pixelFormat, DestinationColorSpace::SRGB() }, size);
    if (!pixelBuffer)
        return nullptr;

    size_t rowBytes = static_cast<size_t>(size.width()) * 4;
    size_t height = size.height();
    auto destination = pixelBuffer->bytes();
    if (destination.size() < rowBytes * height)
        return nullptr;

    if (canPeek) {
        auto source = unsafeMakeSpan(static_cast<const uint8_t*>(pixmap.addr()), pixmap.computeByteSize());
        for (size_t row = 0; row < height; ++row)
            memcpySpan(destination.subspan(row * rowBytes, rowBytes), source.subspan((height - 1 - row) * pixmap.rowBytes(), rowBytes));
        return createFromPixelBuffer(pixelBuffer.releaseNonNull(), { }, frameRate, { });
    }

    if (platformImage->isTextureBacked() && !PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent())
        return nullptr;
    auto info = platformImage->imageInfo().makeColorType(kBGRA_8888_SkColorType).makeAlphaType(platformImage->isOpaque() ? kOpaque_SkAlphaType : kUnpremul_SkAlphaType);
    if (!platformImage->readPixels(image.grContext(), info, destination.data(), rowBytes, 0, 0))
        return nullptr;
    Vector<uint8_t> scratch(rowBytes);
    for (size_t top = 0, bottom = height - 1; top < bottom; ++top, --bottom) {
        auto topRow = destination.subspan(top * rowBytes, rowBytes);
        auto bottomRow = destination.subspan(bottom * rowBytes, rowBytes);
        memcpySpan(scratch.mutableSpan(), bottomRow);
        memcpySpan(bottomRow, topRow);
        memcpySpan(topRow, scratch.span());
    }
    return createFromPixelBuffer(pixelBuffer.releaseNonNull(), { }, frameRate, { });
}

"""


def video_frame_header_patch(text):
    return replace_once(
        text,
        "    static RefPtr<VideoFrameGStreamer> createFromPixelBuffer(Ref<PixelBuffer>&&, const IntSize& destinationSize, double frameRate, const CreateOptions&, PlatformVideoColorSpace&& = { });\n",
        "    static RefPtr<VideoFrameGStreamer> createFromPixelBuffer(Ref<PixelBuffer>&&, const IntSize& destinationSize, double frameRate, const CreateOptions&, PlatformVideoColorSpace&& = { });\n"
        "    static RefPtr<VideoFrameGStreamer> createFromBottomUpNativeImage(NativeImage&, double frameRate);\n",
    )


def webgl_context_patch(text):
    text = replace_once(
        text,
        "#if ENABLE(VIDEO)\n#include \"VideoFrame.h\"\n#endif\n",
        "#if ENABLE(VIDEO)\n#include \"VideoFrame.h\"\n#if USE(GSTREAMER)\n#include \"VideoFrameGStreamer.h\"\n#endif\n#endif\n",
    )
    return replace_once(
        text,
        "    if (buffer == SurfaceBuffer::DrawingBuffer)\n"
        "        clearIfComposited(CallerTypeOther);\n"
        "    return protect(graphicsContextGL())->surfaceBufferToVideoFrame(toGCGLSurfaceBuffer(buffer));\n",
        "    if (buffer == SurfaceBuffer::DrawingBuffer)\n"
        "        clearIfComposited(CallerTypeOther);\n"
        "#if USE(GSTREAMER)\n"
        "    RefPtr context = graphicsContextGL();\n"
        "    if (RefPtr frame = context->surfaceBufferToVideoFrame(toGCGLSurfaceBuffer(buffer)))\n"
        "        return frame;\n"
        "    RefPtr image = context->copyNativeImageYFlipped(toGCGLSurfaceBuffer(buffer));\n"
        "    if (!image)\n"
        "        return nullptr;\n"
        "    return VideoFrameGStreamer::createFromBottomUpNativeImage(*image, 30);\n"
        "#else\n"
        "    return protect(graphicsContextGL())->surfaceBufferToVideoFrame(toGCGLSurfaceBuffer(buffer));\n"
        "#endif\n",
    )


def video_frame_patch(text):
    text = replace_once(
        text,
        "#include <wtf/glib/GMallocString.h>\n",
        "#include <wtf/RuntimeApplicationChecks.h>\n#include <wtf/glib/GMallocString.h>\n",
    )
    text = replace_once(
        text,
        "#include \"PixelBuffer.h\"\n",
        "#include \"PixelBuffer.h\"\n#include \"TypedArrayPixelBuffer.h\"\n",
    )
    text = replace_once(
        text,
        "RefPtr<VideoFrame> VideoFrame::fromNativeImage(NativeImage& image)\n",
        BOTTOM_UP_NATIVE_IMAGE + "RefPtr<VideoFrame> VideoFrame::fromNativeImage(NativeImage& image)\n",
    )
    text = replace_once(
        text,
        "    std::call_once(debugRegisteredFlag, [] {\n"
        "        GST_DEBUG_CATEGORY_INIT(webkit_video_frame_debug, \"webkitvideoframe\", 0, \"WebKit Video Frame\");\n",
        "    std::call_once(debugRegisteredFlag, [] {\n"
        "        if (isInWebProcess())\n"
        "            ensureGStreamerInitialized();\n"
        "        else\n"
        "            ensureGStreamerInitializedNonWebProcess();\n"
        "        GST_DEBUG_CATEGORY_INIT(webkit_video_frame_debug, \"webkitvideoframe\", 0, \"WebKit Video Frame\");\n",
    )
    text = replace_span(
        text,
        "static void copyPlane(std::span<uint8_t>& destination, const std::span<uint8_t>& source, uint64_t sourceStride, const ComputedPlaneLayout& spanPlaneLayout)\n",
        "\n\nvoid VideoFrame::copyTo(",
        COPY_PLANE + "\n" + COPY_FORMATS + "void VideoFrame::copyTo(",
    )
    text = replace_span(
        text,
        "    GRefPtr<GstSample> inputSample;\n    switch (self.memoryType()) {\n",
        "#endif\n    }\n\n    if (!inputSample) {",
        COPY_INPUT,
    )
    return replace_span(
        text,
        "    if (pixelFormat == VideoPixelFormat::NV12) {\n",
        "    // FIXME: Handle I422, I444, RGBX and BGRX formats.\n    callback({ });\n}\n",
        COPY_PLANES,
    )


def converter_patch(text):
    text = replace_once(
        text,
        '#include "GStreamerCommon.h"\n',
        '#include "GStreamerCommon.h"\n#include "GUniquePtrGStreamer.h"\n',
    )
    text = replace_once(
        text,
        "#include <gst/app/gstappsrc.h>\n",
        "#include <gst/app/gstappsrc.h>\n#include <gst/video/video.h>\n",
    )
    text = replace_once(
        text,
        "#include <wtf/NeverDestroyed.h>\n",
        "#include <algorithm>\n#include <wtf/NeverDestroyed.h>\n",
    )
    text = replace_once(
        text,
        "WTF_MAKE_TZONE_ALLOCATED_IMPL(GStreamerVideoFrameConverter::Pipeline);\n",
        "WTF_MAKE_TZONE_ALLOCATED_IMPL(GStreamerVideoFrameConverter::Pipeline);\n"
        "#if USE(GSTREAMER_GL)\n"
        "WTF_MAKE_TZONE_ALLOCATED_IMPL(GStreamerVideoFrameConverter::GPUPipeline);\n"
        "#endif\n",
    )
    text = replace_span(
        text,
        "GStreamerVideoFrameConverter::Pipeline::Pipeline(Type type)\n",
        "GStreamerVideoFrameConverter::Pipeline::~Pipeline() = default;\n",
        PIPELINE,
    )
    text = replace_span(
        text,
        "GRefPtr<GstSample> GStreamerVideoFrameConverter::Pipeline::run(const GRefPtr<GstSample>& sample, GstCaps* destinationCaps)\n{\n#if USE(GSTREAMER_GL)\n",
        "#endif\n\n    unsigned capsSize = gst_caps_get_size(destinationCaps);\n",
        "GRefPtr<GstSample> GStreamerVideoFrameConverter::Pipeline::run(const GRefPtr<GstSample>& sample, GstCaps* destinationCaps)\n{\n    unsigned capsSize = gst_caps_get_size(destinationCaps);\n",
    )
    text = replace_span(
        text,
        "GStreamerVideoFrameConverter::GStreamerVideoFrameConverter()\n",
        "    return *m_systemMemoryPipeline;\n}\n\n",
        ENSURE_PIPELINE,
    )
    text = replace_once(text, "GRefPtr<GstSample> GStreamerVideoFrameConverter::convert(", SYSTEM_MEMORY_CONVERSION)
    text = replace_once(
        text,
        "    auto outputSample = ensurePipeline(inputCaps).run(sample, destinationCaps.get());\n",
        "    auto outputSample = convertInSystemMemory(sample, destinationCaps.get());\n"
        "    if (!outputSample) {\n"
        "        Locker locker { m_lock };\n"
        "        auto* inputFeatures = gst_caps_get_size(inputCaps) ? gst_caps_get_features(inputCaps, 0) : nullptr;\n"
        "        bool isSystemMemory = !inputFeatures || gst_caps_features_is_equal(inputFeatures, GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY);\n"
        "#if USE(GSTREAMER_GL)\n"
        "        if (isGPUMemory(inputCaps))\n"
        "            outputSample = convertOnGPU(sample, destinationCaps.get());\n"
        "        else\n"
        "#endif\n"
        "        if (isSystemMemory)\n"
        "            outputSample = ensurePipeline().run(sample, destinationCaps.get());\n"
        "        else\n"
        "            outputSample = convertMappedInSystemMemory(sample, destinationCaps.get());\n"
        "    }\n",
    )
    text = replace_span(
        text,
        "    if (auto meta = gst_buffer_get_video_meta(writableBuffer.get()))\n",
        "    gst_buffer_add_video_meta(writableBuffer.get(), GST_VIDEO_FRAME_FLAG_NONE, format, *width, *height);\n",
        "    if (auto meta = gst_buffer_get_parent_buffer_meta(writableBuffer.get()))\n"
        "        gst_buffer_remove_meta(writableBuffer.get(), GST_META_CAST(meta));\n"
        "\n" + CONVERTED_VIDEO_META,
    )
    return replace_span(
        text,
        "void GStreamerVideoFrameConverter::releaseUnusedSystemMemoryPipelineTimerFired()\n",
        "#undef GST_CAT_DEFAULT\n",
        RELEASE_PIPELINES,
    )


PARSE_COPY_OPTIONS = r"""ExceptionOr<CombinedPlaneLayout> parseVideoFrameCopyToOptions(const WebCodecsVideoFrame& frame, const WebCodecsVideoFrame::CopyToOptions& options)
{
    ASSERT(!frame.isDetached());
    ASSERT(frame.format() || options.format);

    if (options.format && !isRGBVideoPixelFormat(*options.format))
        return Exception { ExceptionCode::NotSupportedError, "copyTo() can only convert to RGBA, RGBX, BGRA or BGRX"_s };
    if (options.colorSpace && *options.colorSpace != PredefinedColorSpace::SRGB)
        return Exception { ExceptionCode::NotSupportedError, "copyTo() can only convert to the srgb color space"_s };

    auto sourceFormat = frame.format() ? *frame.format() : *options.format;
    Ref visibleRect = *frame.visibleRect();
    auto parsedRect = parseVisibleRect({ visibleRect->x(), visibleRect->y(), visibleRect->width(), visibleRect->height() }, options.rect, frame.codedWidth(), frame.codedHeight(), sourceFormat);

    if (parsedRect.hasException())
        return parsedRect.releaseException();

    return computeLayoutAndAllocationSize(parsedRect.returnValue(), options.layout, options.format ? *options.format : sourceFormat);
}
"""


def frame_idl_patch(text):
    return replace_once(
        text,
        "  DOMRectInit rect;\n  sequence<PlaneLayout> layout;\n};\n",
        "  DOMRectInit rect;\n  sequence<PlaneLayout> layout;\n  VideoPixelFormat format;\n  PredefinedColorSpace colorSpace;\n};\n",
    )


def frame_header_patch(text):
    text = replace_once(
        text,
        "#include <WebCore/PlaneLayout.h>\n",
        "#include <WebCore/PlaneLayout.h>\n#include <WebCore/PredefinedColorSpace.h>\n",
    )
    return replace_once(
        text,
        "        std::optional<DOMRectInit> rect;\n        std::optional<Vector<PlaneLayout>> layout;\n    };\n",
        "        std::optional<DOMRectInit> rect;\n        std::optional<Vector<PlaneLayout>> layout;\n        std::optional<VideoPixelFormat> format;\n        std::optional<PredefinedColorSpace> colorSpace;\n    };\n",
    )


def frame_implementation_patch(text):
    text = replace_once(
        text,
        "    if (!m_data.format)\n        return Exception { ExceptionCode::NotSupportedError,  \"VideoFrame has no format\"_s };\n\n    auto layoutOrException = parseVideoFrameCopyToOptions(*this, options);\n",
        "    if (!m_data.format && !options.format)\n        return Exception { ExceptionCode::NotSupportedError,  \"VideoFrame has no format\"_s };\n\n    auto layoutOrException = parseVideoFrameCopyToOptions(*this, options);\n",
    )
    text = replace_once(
        text,
        "    if (!m_data.format) {\n        promise.reject(Exception { ExceptionCode::NotSupportedError,  \"VideoFrame has no format\"_s });\n        return;\n    }\n\n    auto combinedLayoutOrException = parseVideoFrameCopyToOptions(*this, options);\n",
        "    if (!m_data.format && !options.format) {\n        promise.reject(Exception { ExceptionCode::NotSupportedError,  \"VideoFrame has no format\"_s });\n        return;\n    }\n\n    auto combinedLayoutOrException = parseVideoFrameCopyToOptions(*this, options);\n",
    )
    return replace_once(
        text,
        "    protect(m_data.internalFrame)->copyTo(buffer, *m_data.format, WTF::move(combinedLayout.computedLayouts),",
        "    auto copyFormat = options.format ? *options.format : *m_data.format;\n    protect(m_data.internalFrame)->copyTo(buffer, copyFormat, WTF::move(combinedLayout.computedLayouts),",
    )


def frame_algorithms_patch(text):
    text = replace_once(
        text,
        '#include "VideoColorSpace.h"\n',
        '#include "PredefinedColorSpace.h"\n#include "VideoColorSpace.h"\n',
    )
    return replace_span(
        text,
        "ExceptionOr<CombinedPlaneLayout> parseVideoFrameCopyToOptions(const WebCodecsVideoFrame& frame, const WebCodecsVideoFrame::CopyToOptions& options)\n",
        "    return computeLayoutAndAllocationSize(parsedRect.returnValue(), options.layout, *frame.format());\n}\n",
        PARSE_COPY_OPTIONS,
    )


def remote_gl_proxy_local_frame(text):
    return replace_once(
        text,
        "    ASSERT(isMainRunLoop());\n"
        "    if (isContextLost())\n"
        "        return nullptr;\n"
        "    auto sendResult = sendSync(Messages::RemoteGraphicsContextGL::SurfaceBufferToVideoFrame(buffer));\n",
        "    ASSERT(isMainRunLoop());\n"
        "    if (isContextLost())\n"
        "        return nullptr;\n"
        "#if USE(GSTREAMER)\n"
        "    UNUSED_PARAM(buffer);\n"
        "    return nullptr;\n"
        "#else\n"
        "    auto sendResult = sendSync(Messages::RemoteGraphicsContextGL::SurfaceBufferToVideoFrame(buffer));\n",
    )


def remote_gl_proxy_patch(text):
    text = remote_gl_proxy_local_frame(text)
    return replace_once(
        text,
        "    return RemoteVideoFrameProxy::create(WebProcess::singleton().ensureGPUProcessConnection().connection(), protect(protect(WebProcess::singleton().ensureGPUProcessConnection())->videoFrameObjectHeapProxy()), WTF::move(*result));\n}\n",
        "    return RemoteVideoFrameProxy::create(WebProcess::singleton().ensureGPUProcessConnection().connection(), protect(protect(WebProcess::singleton().ensureGPUProcessConnection())->videoFrameObjectHeapProxy()), WTF::move(*result));\n#endif\n}\n",
    )


def remote_gl_proxy_gbm_patch(text):
    return replace_once(
        text,
        "    if (!m_displayBuffer)\n        return;\n\n    OptionSet<TextureMapperFlags> flags = TextureMapperFlags::ShouldFlipTexture;\n",
        "    if (!m_displayBuffer)\n        return;\n\n    m_hasPreparedForDisplay = true;\n    OptionSet<TextureMapperFlags> flags = TextureMapperFlags::ShouldFlipTexture;\n",
    )


def remote_gl_patch(text):
    return replace_once(
        text,
        "    std::optional<WebKit::RemoteVideoFrameProxy::Properties> result;\n"
        "    if (auto videoFrame = protect(m_context)->surfaceBufferToVideoFrame(buffer))\n"
        "        result = m_videoFrameObjectHeap->add(videoFrame.releaseNonNull());\n"
        "    completionHandler(WTF::move(result));\n",
        "    std::optional<WebKit::RemoteVideoFrameProxy::Properties> result;\n"
        "#if USE(GSTREAMER)\n"
        "    UNUSED_PARAM(buffer);\n"
        "#else\n"
        "    if (auto videoFrame = protect(m_context)->surfaceBufferToVideoFrame(buffer))\n"
        "        result = m_videoFrameObjectHeap->add(videoFrame.releaseNonNull());\n"
        "#endif\n"
        "    completionHandler(WTF::move(result));\n",
    )


PATCHES = {
    CONVERTER_HEADER: converter_header_patch,
    CONVERTER: converter_patch,
    VIDEO_FRAME: video_frame_patch,
    FRAME_IDL: frame_idl_patch,
    FRAME_HEADER: frame_header_patch,
    FRAME_IMPLEMENTATION: frame_implementation_patch,
    FRAME_ALGORITHMS: frame_algorithms_patch,
    REMOTE_GL_PROXY: remote_gl_proxy_patch,
    REMOTE_GL: remote_gl_patch,
    REMOTE_GL_PROXY_GBM: remote_gl_proxy_gbm_patch,
    VIDEO_FRAME_HEADER: video_frame_header_patch,
    WEBGL_CONTEXT: webgl_context_patch,
}

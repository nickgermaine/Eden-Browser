from wpe.patch_support import replace_once


GRAPHICS = "Source/WebCore/platform/graphics/gstreamer/"
MEDIASTREAM = "Source/WebCore/platform/mediastream/gstreamer/"
MIXER = GRAPHICS + "GStreamerAudioMixer.cpp"
MIXER_HEADER = GRAPHICS + "GStreamerAudioMixer.h"
AUDIO_SINK = GRAPHICS + "WebKitAudioSinkGStreamer.cpp"
AUDIO_SINK_HEADER = GRAPHICS + "WebKitAudioSinkGStreamer.h"
PLAYER = GRAPHICS + "MediaPlayerPrivateGStreamer.cpp"
PLAYER_HEADER = GRAPHICS + "MediaPlayerPrivateGStreamer.h"
CAPTURER = MEDIASTREAM + "GStreamerAudioCapturer.cpp"
CAPTURER_HEADER = MEDIASTREAM + "GStreamerAudioCapturer.h"
CAPTURE_SOURCE = MEDIASTREAM + "GStreamerAudioCaptureSource.cpp"


def replace_every(text, before, after, count):
    if text.count(before) != count:
        raise RuntimeError("The pinned source does not match the voice processing patch: " + before[:100])
    return text.replace(before, after)


def mixer_header_patch(text):
    text = replace_once(
        text,
        "#include <wtf/text/CStringView.h>\n",
        "#include <wtf/text/CString.h>\n#include <wtf/text/CStringView.h>\n",
    )
    text = replace_once(
        text,
        "    void configureSourcePeriodTime(CStringView sourceName, uint64_t periodTime);\n",
        "    void configureSourcePeriodTime(CStringView sourceName, uint64_t periodTime);\n"
        "    void configureCommunicationSource(CStringView sourceName);\n\n"
        "    std::optional<CString> acquireEchoReference();\n"
        "    void releaseEchoReference();\n",
    )
    text = replace_once(
        text,
        "        GRefPtr<GstElement> mixer;\n        RunLoop::Timer teardownTimer;\n",
        "        GRefPtr<GstElement> mixer;\n        GRefPtr<GstElement> echoProbe;\n        RunLoop::Timer teardownTimer;\n",
    )
    return replace_once(
        text,
        "        HashMap<GstPad*, String> m_padToDeviceId; // Reverse lookup: pad → deviceId.\n",
        "        HashMap<GstPad*, String> m_padToDeviceId; // Reverse lookup: pad → deviceId.\n"
        "        unsigned echoReferences { 0 };\n",
    )


MIXER_ADDITIONS = r'''
void GStreamerAudioMixer::configureCommunicationSource(CStringView sourceName)
{
    static constexpr guint64 communicationPeriodTime = 10 * GST_MSECOND;
    static constexpr guint64 communicationLatencyTime = 30 * GST_MSECOND;
    DataMutexLocker locker { m_streamingMembers };
    for (auto& [deviceId, mp] : locker->m_pipelines) {
        GRefPtr src = adoptGRef(gst_bin_get_by_name(GST_BIN_CAST(mp->pipeline.get()), sourceName.utf8()));
        if (!src)
            continue;

        g_object_set(src.get(), "period-time", communicationPeriodTime, "latency-time", communicationLatencyTime, nullptr);
        return;
    }
}

std::optional<CString> GStreamerAudioMixer::acquireEchoReference()
{
    if (!isAvailable())
        return std::nullopt;

    DataMutexLocker locker { m_streamingMembers };
    auto& mp = ensureMixerPipeline(locker, "default"_s, nullptr);
    if (!mp.echoProbe)
        return std::nullopt;

    locker->echoReferences++;
    if (isMainThread())
        mp.teardownTimer.stop();
    GST_DEBUG_OBJECT(mp.pipeline.get(), "Echo reference acquired, %u active.", locker->echoReferences);
    return CString(GST_ELEMENT_NAME(mp.echoProbe.get()));
}

void GStreamerAudioMixer::releaseEchoReference()
{
    DataMutexLocker locker { m_streamingMembers };
    if (!locker->echoReferences)
        return;

    if (--locker->echoReferences)
        return;

    auto it = locker->m_pipelines.find("default"_s);
    if (it == locker->m_pipelines.end())
        return;

    auto& mp = *it->value;
    GST_DEBUG_OBJECT(mp.pipeline.get(), "Last echo reference released.");
    if (!mp.mixer->numsinkpads)
        mp.teardownTimer.startOneShot(s_teardownTimeout);
}

#undef GST_CAT_DEFAULT
'''


ECHO_REFERENCE_TIMESTAMPS = r'''enum class EchoReferenceShift : int { ToClockTime = 1, ToRunningTime = 2 };

static GstPadProbeReturn shiftEchoReferenceTimestamp(GstPad* pad, GstPadProbeInfo* info, gpointer userData)
{
    auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer || !GST_BUFFER_PTS_IS_VALID(buffer))
        return GST_PAD_PROBE_OK;

    GRefPtr element = adoptGRef(gst_pad_get_parent_element(pad));
    if (!element)
        return GST_PAD_PROBE_OK;

    auto baseTime = gst_element_get_base_time(element.get());
    if (!GST_CLOCK_TIME_IS_VALID(baseTime) || !baseTime)
        return GST_PAD_PROBE_OK;

    bool toClockTime = GPOINTER_TO_INT(userData) == static_cast<int>(EchoReferenceShift::ToClockTime);
    if (!toClockTime && GST_BUFFER_PTS(buffer) < baseTime)
        return GST_PAD_PROBE_OK;

    buffer = gst_buffer_make_writable(buffer);
    GST_BUFFER_PTS(buffer) = toClockTime ? GST_BUFFER_PTS(buffer) + baseTime : GST_BUFFER_PTS(buffer) - baseTime;
    GST_PAD_PROBE_INFO_DATA(info) = buffer;
    return GST_PAD_PROBE_OK;
}

'''


def mixer_patch(text):
    text = replace_once(
        text,
        "    gst_bin_add_many(GST_BIN_CAST(pipeline.get()), mixer, audioSink, nullptr);\n"
        "    gst_element_link(mixer, audioSink);\n",
        "    gst_bin_add_many(GST_BIN_CAST(pipeline.get()), mixer, audioSink, nullptr);\n"
        "    GstElement* echoProbe = nullptr;\n"
        '    if (isGStreamerPluginAvailable("webrtcdsp"_s))\n'
        '        echoProbe = makeGStreamerElement("webrtcechoprobe"_s, makeString("webkitechoprobe-"_s, deviceId));\n'
        "    if (echoProbe) {\n"
        "        auto* echoFormat = gst_element_factory_make(\"capsfilter\", nullptr);\n"
        '        GRefPtr echoCaps = adoptGRef(gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S16LE", "layout", G_TYPE_STRING, "interleaved", "rate", G_TYPE_INT, 48000, nullptr));\n'
        '        g_object_set(echoFormat, "caps", echoCaps.get(), nullptr);\n'
        '        auto* outputConvert = makeGStreamerElement("audioconvert"_s);\n'
        '        auto* outputResample = makeGStreamerElement("audioresample"_s);\n'
        "        gst_bin_add_many(GST_BIN_CAST(pipeline.get()), echoFormat, echoProbe, outputConvert, outputResample, nullptr);\n"
        "        gst_element_link_many(mixer, echoFormat, echoProbe, outputConvert, outputResample, audioSink, nullptr);\n"
        "        GRefPtr echoClock = adoptGRef(gst_system_clock_obtain());\n"
        "        gst_pipeline_use_clock(GST_PIPELINE(pipeline.get()), echoClock.get());\n"
        '        GRefPtr echoInput = adoptGRef(gst_element_get_static_pad(echoProbe, "sink"));\n'
        "        gst_pad_add_probe(echoInput.get(), GST_PAD_PROBE_TYPE_BUFFER, shiftEchoReferenceTimestamp, GINT_TO_POINTER(static_cast<int>(EchoReferenceShift::ToClockTime)), nullptr);\n"
        '        GRefPtr echoOutput = adoptGRef(gst_element_get_static_pad(echoProbe, "src"));\n'
        "        gst_pad_add_probe(echoOutput.get(), GST_PAD_PROBE_TYPE_BUFFER, shiftEchoReferenceTimestamp, GINT_TO_POINTER(static_cast<int>(EchoReferenceShift::ToRunningTime)), nullptr);\n"
        "    } else\n"
        "        gst_element_link(mixer, audioSink);\n",
    )
    text = replace_once(
        text,
        "GStreamerAudioMixer::MixerPipeline& GStreamerAudioMixer::ensureMixerPipeline(",
        ECHO_REFERENCE_TIMESTAMPS + "GStreamerAudioMixer::MixerPipeline& GStreamerAudioMixer::ensureMixerPipeline(",
    )
    text = replace_once(
        text,
        "        GRefPtr<GstElement>(mixer),\n        RunLoop::Timer(",
        "        GRefPtr<GstElement>(mixer),\n        GRefPtr<GstElement>(echoProbe),\n        RunLoop::Timer(",
    )
    text = replace_once(
        text,
        "    RELEASE_ASSERT(it != locker->m_pipelines.end());\n\n    auto& mp = *it->value;\n    ASSERT(!mp.mixer->numsinkpads);\n",
        "    RELEASE_ASSERT(it != locker->m_pipelines.end());\n\n"
        '    if (deviceId == "default"_s && locker->echoReferences)\n'
        "        return;\n\n"
        "    auto& mp = *it->value;\n    if (mp.mixer->numsinkpads)\n        return;\n",
    )
    text = replace_once(
        text,
        "        result->value->teardownTimer.stop();\n        return *result->value;\n",
        "        if (isMainThread())\n            result->value->teardownTimer.stop();\n        return *result->value;\n",
    )
    text = replace_once(
        text,
        '#include "GStreamerCommon.h"\n',
        '#include "GStreamerCommon.h"\n#include <wtf/MainThread.h>\n',
    )
    return replace_once(text, "\n#undef GST_CAT_DEFAULT\n", MIXER_ADDITIONS)


def audio_sink_header_patch(text):
    return replace_once(
        text,
        "GstElement* webkitAudioSinkNew(const String& role, const String& deviceId = { }, const GRefPtr<GstDevice>& = { });\n",
        "enum class WebKitAudioSinkUsage : uint8_t { Default, Communication };\n\n"
        "GstElement* webkitAudioSinkNew(const String& role, const String& deviceId = { }, const GRefPtr<GstDevice>& = { }, WebKitAudioSinkUsage = WebKitAudioSinkUsage::Default);\n"
        "GstElement* createCommunicationAudioSink(const String& role, const String& deviceId = { }, const GRefPtr<GstDevice>& = { });\n",
    )


def audio_sink_patch(text):
    text = replace_once(
        text,
        "    String deviceId;\n    GRefPtr<GstDevice> device;\n};\n",
        "    String deviceId;\n    GRefPtr<GstDevice> device;\n    WebKitAudioSinkUsage usage { WebKitAudioSinkUsage::Default };\n};\n",
    )
    text = replace_once(
        text,
        '    if (!enableAudioMixer || enableAudioMixer != "1"_s)\n        return false;\n',
        '    if (sink->priv->usage != WebKitAudioSinkUsage::Communication && (!enableAudioMixer || enableAudioMixer != "1"_s))\n        return false;\n',
    )
    text = replace_every(
        text,
        "        priv->mixerPad = mixer.registerProducer(priv->interAudioSink.get(), forcedSampleRate, priv->deviceId, priv->device);\n",
        "        priv->mixerPad = mixer.registerProducer(priv->interAudioSink.get(), forcedSampleRate, priv->deviceId, priv->device);\n"
        "        if (priv->usage == WebKitAudioSinkUsage::Communication)\n"
        "            mixer.configureCommunicationSource(CStringView::unsafeFromUTF8(GST_ELEMENT_NAME(priv->interAudioSink.get())));\n",
        2,
    )
    text = replace_once(
        text,
        "GstElement* /* (transfer floating) */ webkitAudioSinkNew(const String& role, const String& deviceId, const GRefPtr<GstDevice>& device)\n",
        "GstElement* /* (transfer floating) */ webkitAudioSinkNew(const String& role, const String& deviceId, const GRefPtr<GstDevice>& device, WebKitAudioSinkUsage usage)\n",
    )
    text = replace_once(
        text,
        "    audioSink->priv->role = role;\n",
        "    audioSink->priv->role = role;\n    audioSink->priv->usage = usage;\n",
    )
    return replace_once(
        text,
        "bool webkitAudioSinkSetDevice(GstElement* element, const String& deviceId, const GRefPtr<GstDevice>& device)\n",
        "GstElement* createCommunicationAudioSink(const String& role, const String& deviceId, const GRefPtr<GstDevice>& device)\n"
        "{\n"
        "    if (auto* audioSink = webkitAudioSinkNew(role, deviceId, device, WebKitAudioSinkUsage::Communication))\n"
        "        return audioSink;\n"
        "    return createPlatformAudioSink(role, deviceId, device);\n"
        "}\n\n"
        "bool webkitAudioSinkSetDevice(GstElement* element, const String& deviceId, const GRefPtr<GstDevice>& device)\n",
    )


def player_header_patch(text):
    text = replace_once(
        text,
        "#include <wtf/text/AtomStringHash.h>\n",
        "#include <wtf/text/AtomStringHash.h>\n\nenum class WebKitAudioSinkUsage : uint8_t;\n",
    )
    return replace_once(
        text,
        "    GstElement* createAudioSink();\n",
        "    GstElement* createAudioSink();\n    GstElement* createAudioSink(WebKitAudioSinkUsage);\n",
    )


def player_patch(text):
    text = replace_once(
        text,
        '#include "VideoTrackPrivateGStreamer.h"\n',
        '#include "VideoTrackPrivateGStreamer.h"\n#include "WebKitAudioSinkGStreamer.h"\n',
    )
    text = replace_once(
        text,
        "GstElement* MediaPlayerPrivateGStreamer::createAudioSink()\n{\n",
        "GstElement* MediaPlayerPrivateGStreamer::createAudioSink()\n{\n"
        "    return createAudioSink(WebKitAudioSinkUsage::Default);\n"
        "}\n\n"
        "GstElement* MediaPlayerPrivateGStreamer::createAudioSink(WebKitAudioSinkUsage usage)\n{\n",
    )
    text = replace_once(
        text,
        "            audioSink = createPlatformAudioSink(role, resolvedId, device);\n",
        "            audioSink = usage == WebKitAudioSinkUsage::Communication ? createCommunicationAudioSink(role, resolvedId, device) : createPlatformAudioSink(role, resolvedId, device);\n",
    )
    text = replace_once(
        text,
        "    if (!audioSink)\n        audioSink = createPlatformAudioSink(role);\n",
        "    if (!audioSink)\n        audioSink = usage == WebKitAudioSinkUsage::Communication ? createCommunicationAudioSink(role) : createPlatformAudioSink(role);\n",
    )
    return replace_once(
        text,
        "    if (!m_audioSink)\n        m_audioSink = createAudioSink();\n\n"
        '    g_object_set(m_pipeline.get(), "audio-sink", m_audioSink.get(), "video-sink", createVideoSink(), nullptr);\n',
        "    if (isMediaStream) {\n"
        "        bool hasAudioSourceClient = false;\n"
        "#if ENABLE(WEB_AUDIO)\n"
        "        hasAudioSourceClient = m_audioSourceProvider && m_audioSourceProvider->client();\n"
        "#endif\n"
        "        if (!hasAudioSourceClient) {\n"
        "            if (auto* communicationSink = createAudioSink(WebKitAudioSinkUsage::Communication))\n"
        "                m_audioSink = communicationSink;\n"
        "        }\n"
        "    }\n\n"
        "    if (!m_audioSink)\n        m_audioSink = createAudioSink();\n\n"
        '    g_object_set(m_pipeline.get(), "audio-sink", m_audioSink.get(), "video-sink", createVideoSink(), nullptr);\n',
    )


def capturer_header_patch(text):
    text = replace_once(
        text,
        '#include "GStreamerCapturer.h"\n',
        '#include "GStreamerCapturer.h"\n#include <wtf/text/CString.h>\n',
    )
    text = replace_once(
        text,
        "    ~GStreamerAudioCapturer() = default;\n",
        "    ~GStreamerAudioCapturer();\n",
    )
    text = replace_once(
        text,
        "    bool setSampleRate(int);\n",
        "    bool setSampleRate(int);\n    void setVoiceProcessing(bool);\n",
    )
    return replace_once(
        text,
        "private:\n    void handleSample(GRefPtr<GstSample>&&);\n",
        "private:\n    void handleSample(GRefPtr<GstSample>&&);\n"
        "    void addVoiceProcessor(GstBin*, bool hasNoiseSuppressor);\n"
        "    bool m_voiceProcessing { false };\n"
        "    std::optional<CString> m_echoProbeName;\n",
    )


CAPTURE_GAIN_CONTROL = r'''#if USE(LIBWEBRTC)
class CaptureGainController {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(CaptureGainController);
public:
    CaptureGainController()
    {
        webrtc::AudioProcessing::Config config;
        config.gain_controller1.enabled = false;
        config.gain_controller2.enabled = true;
        config.gain_controller2.adaptive_digital.enabled = true;
        config.gain_controller2.input_volume_controller.enabled = false;
        config.gain_controller2.fixed_digital.gain_db = 0;
        m_processor = webrtc::BuiltinAudioProcessingBuilder(config).Build(webrtc::CreateEnvironment());
        if (!m_processor)
            GST_WARNING("Could not create the capture gain controller");
    }

    void setFormat(const GstCaps* caps)
    {
        GstAudioInfo info;
        if (!caps || !gst_audio_info_from_caps(&info, caps) || GST_AUDIO_INFO_FORMAT(&info) != GST_AUDIO_FORMAT_S16 || GST_AUDIO_INFO_LAYOUT(&info) != GST_AUDIO_LAYOUT_INTERLEAVED || GST_AUDIO_INFO_RATE(&info) % 100) {
            m_sampleRate = 0;
            m_channels = 0;
            GST_WARNING("The capture gain controller cannot process %" GST_PTR_FORMAT, caps);
            return;
        }
        m_sampleRate = GST_AUDIO_INFO_RATE(&info);
        m_channels = GST_AUDIO_INFO_CHANNELS(&info);
    }

    void process(GstBuffer* buffer)
    {
        if (!m_processor || !m_sampleRate || !m_channels)
            return;

        GstMapInfo map;
        if (!gst_buffer_map(buffer, &map, GST_MAP_READWRITE))
            return;

        size_t frameSamples = static_cast<size_t>(m_sampleRate / 100) * m_channels;
        auto samples = unsafeMakeSpan(reinterpret_cast<int16_t*>(map.data), map.size / sizeof(int16_t));
        webrtc::StreamConfig streamConfig(m_sampleRate, m_channels);
        size_t offset = 0;
        for (; offset + frameSamples <= samples.size(); offset += frameSamples) {
            auto frame = samples.subspan(offset, frameSamples);
            m_processor->ProcessStream(frame.data(), streamConfig, streamConfig, frame.data());
        }
        if (offset != samples.size())
            GST_WARNING("The capture gain controller skipped %zu samples outside 10 ms frames", samples.size() - offset);
        gst_buffer_unmap(buffer, &map);
    }

private:
    webrtc::scoped_refptr<webrtc::AudioProcessing> m_processor;
    int m_sampleRate { 0 };
    size_t m_channels { 0 };
};

static GstPadProbeReturn applyCaptureGain(GstPad*, GstPadProbeInfo* info, gpointer userData)
{
    auto& controller = *static_cast<CaptureGainController*>(userData);
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
        auto* event = GST_PAD_PROBE_INFO_EVENT(info);
        if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
            GstCaps* caps = nullptr;
            gst_event_parse_caps(event, &caps);
            controller.setFormat(caps);
        }
        return GST_PAD_PROBE_OK;
    }

    auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer)
        return GST_PAD_PROBE_OK;
    buffer = gst_buffer_make_writable(buffer);
    GST_PAD_PROBE_INFO_DATA(info) = buffer;
    controller.process(buffer);
    return GST_PAD_PROBE_OK;
}
#endif

'''


VOICE_PROCESSOR = r'''GStreamerAudioCapturer::~GStreamerAudioCapturer()
{
    if (m_echoProbeName)
        GStreamerAudioMixer::singleton().releaseEchoReference();
}

void GStreamerAudioCapturer::setVoiceProcessing(bool enabled)
{
    m_voiceProcessing = enabled;
    if (enabled || !m_echoProbeName)
        return;

    GStreamerAudioMixer::singleton().releaseEchoReference();
    m_echoProbeName = std::nullopt;
}

void GStreamerAudioCapturer::addVoiceProcessor(GstBin* bin, bool hasNoiseSuppressor)
{
    static constexpr int voiceProcessingSampleRate = 48000;
    if (!m_voiceProcessing)
        return;

    if (!m_echoProbeName)
        m_echoProbeName = GStreamerAudioMixer::singleton().acquireEchoReference();
    if (!m_echoProbeName) {
        GST_WARNING("Echo cancellation was requested but no echo reference is available");
        return;
    }

    GRefPtr chainInput = adoptGRef(gst_bin_find_unlinked_pad(bin, GST_PAD_SINK));
    if (!chainInput)
        return;

    auto* processor = makeGStreamerElement("webrtcdsp"_s);
    if (!processor)
        return;

    auto* processorConvert = makeGStreamerElement("audioconvert"_s);
    auto* processorResample = makeGStreamerElement("audioresample"_s);
    auto* processorFormat = gst_element_factory_make("capsfilter", nullptr);
    GRefPtr caps = adoptGRef(gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S16LE", "layout", G_TYPE_STRING, "interleaved", "rate", G_TYPE_INT, voiceProcessingSampleRate, nullptr));
    g_object_set(processorFormat, "caps", caps.get(), nullptr);
#if USE(LIBWEBRTC)
    bool usesDigitalGainControl = false;
#else
    bool usesDigitalGainControl = true;
#endif
    g_object_set(processor, "probe", m_echoProbeName->data(), "echo-cancel", TRUE, "gain-control", usesDigitalGainControl, "noise-suppression", !hasNoiseSuppressor, "high-pass-filter", TRUE, "delay-agnostic", TRUE, "extended-filter", TRUE, "voice-detection", FALSE, nullptr);
    gst_util_set_object_arg(G_OBJECT(processor), "noise-suppression-level", "high");

    gst_bin_add_many(bin, processorConvert, processorResample, processorFormat, processor, nullptr);
    gst_element_link_many(processorConvert, processorResample, processorFormat, processor, nullptr);
    GRefPtr processorOutput = adoptGRef(gst_element_get_static_pad(processor, "src"));
#if USE(LIBWEBRTC)
    gst_pad_add_probe(processorOutput.get(), static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM), applyCaptureGain, new CaptureGainController, [](gpointer data) {
        delete static_cast<CaptureGainController*>(data);
    });
#endif
    gst_pad_link(processorOutput.get(), chainInput.get());
    GST_DEBUG("Voice processing enabled with echo reference %s", m_echoProbeName->data());
}

GstElement* GStreamerAudioCapturer::createConverter()
'''


def capturer_patch(text):
    text = replace_once(
        text,
        '#include "GStreamerAudioCapturer.h"\n',
        '#include "GStreamerAudioCapturer.h"\n\n#include "GStreamerAudioMixer.h"\n#include "GStreamerCommon.h"\n',
    )
    text = replace_once(
        text,
        "#include <gst/app/gstappsink.h>\n",
        "#include <gst/app/gstappsink.h>\n#include <gst/audio/audio.h>\n",
    )
    text = replace_once(
        text,
        "#include <wtf/MediaTime.h>\n",
        "#include <wtf/MediaTime.h>\n\n"
        "#if USE(LIBWEBRTC)\n"
        "WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN\n"
        "#include <webrtc/api/audio/audio_processing.h>\n"
        "#include <webrtc/api/audio/builtin_audio_processing_builder.h>\n"
        "#include <webrtc/api/environment/environment_factory.h>\n"
        "WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END\n"
        "#endif\n",
    )
    text = replace_once(text, "GstElement* GStreamerAudioCapturer::createConverter()\n", CAPTURE_GAIN_CONTROL + VOICE_PROCESSOR)
    text = replace_once(
        text,
        "#if USE(GSTREAMER_WEBRTC)\n"
        '    if (auto audioFilter = makeGStreamerElement("audiornnoise"_s)) {\n',
        "    bool hasNoiseSuppressor = false;\n"
        "#if USE(GSTREAMER_WEBRTC)\n"
        '    if (auto audioFilter = makeGStreamerElement("audiornnoise"_s)) {\n'
        "        hasNoiseSuppressor = true;\n",
    )
    return replace_once(
        text,
        "    if (GRefPtr pad = adoptGRef(gst_bin_find_unlinked_pad(GST_BIN_CAST(bin), GST_PAD_SRC)))\n",
        "    addVoiceProcessor(GST_BIN_CAST(bin), hasNoiseSuppressor);\n\n"
        "    if (GRefPtr pad = adoptGRef(gst_bin_find_unlinked_pad(GST_BIN_CAST(bin), GST_PAD_SRC)))\n",
    )


def capture_source_patch(text):
    text = replace_once(
        text,
        "void GStreamerAudioCaptureSource::startProducingData()\n{\n    m_capturer->setupPipeline();\n",
        "void GStreamerAudioCaptureSource::startProducingData()\n{\n"
        "    m_capturer->setVoiceProcessing(echoCancellation());\n"
        "    m_capturer->setupPipeline();\n",
    )
    return replace_once(
        text,
        "    if (settings.contains(RealtimeMediaSourceSettings::Flag::SampleRate))\n        m_capturer->setSampleRate(sampleRate());\n",
        "    if (settings.contains(RealtimeMediaSourceSettings::Flag::SampleRate))\n        m_capturer->setSampleRate(sampleRate());\n\n"
        "    if (settings.contains(RealtimeMediaSourceSettings::Flag::EchoCancellation)) {\n"
        "        m_capturer->setVoiceProcessing(echoCancellation());\n"
        "        if (isProducingData()) {\n"
        "            m_capturer->stop();\n"
        "            startProducingData();\n"
        "        }\n"
        "    }\n",
    )


PATCHES = {
    MIXER_HEADER: mixer_header_patch,
    MIXER: mixer_patch,
    AUDIO_SINK_HEADER: audio_sink_header_patch,
    AUDIO_SINK: audio_sink_patch,
    PLAYER_HEADER: player_header_patch,
    PLAYER: player_patch,
    CAPTURER_HEADER: capturer_header_patch,
    CAPTURER: capturer_patch,
    CAPTURE_SOURCE: capture_source_patch,
}

from wpe.patch_support import replace_once


GLUE = "Source/WebCore/platform/mediastream/libwebrtc/gstreamer/"
OUTGOING_AUDIO = GLUE + "RealtimeOutgoingAudioSourceLibWebRTC.cpp"
OUTGOING_AUDIO_HEADER = GLUE + "RealtimeOutgoingAudioSourceLibWebRTC.h"


def outgoing_audio_header_patch(text):
    return replace_once(
        text,
        "    GUniquePtr<GstAudioConverter> m_sampleConverter;\n"
        "    GstAudioInfo m_inputStreamDescription;\n"
        "    GstAudioInfo m_outputStreamDescription;\n"
        "\n"
        "    Lock m_adapterLock;\n"
        "    GRefPtr<GstAdapter> m_adapter WTF_GUARDED_BY_LOCK(m_adapterLock);\n"
        "    Vector<uint8_t> m_audioBuffer;\n",
        "    Lock m_conversionLock;\n"
        "    GUniquePtr<GstAudioConverter> m_sampleConverter WTF_GUARDED_BY_LOCK(m_conversionLock);\n"
        "    GstAudioInfo m_inputStreamDescription WTF_GUARDED_BY_LOCK(m_conversionLock);\n"
        "    GstAudioInfo m_outputStreamDescription WTF_GUARDED_BY_LOCK(m_conversionLock);\n"
        "    Vector<uint8_t> m_convertedAudio WTF_GUARDED_BY_LOCK(m_conversionLock);\n"
        "    Vector<uint8_t> m_audioBuffer;\n",
    )


UPSTREAM_TRANSFER = '''void RealtimeOutgoingAudioSourceLibWebRTC::audioSamplesAvailable(const MediaTime&, const PlatformAudioData& audioData, const AudioStreamDescription& streamDescription, size_t /* sampleCount */)
{
    DisableMallocRestrictionsForCurrentThreadScope disableMallocRestrictions;
    auto data = static_cast<const GStreamerAudioData&>(audioData);
    auto desc = static_cast<const GStreamerAudioStreamDescription&>(streamDescription);

    if (m_sampleConverter && !gst_audio_info_is_equal(&m_inputStreamDescription, &desc.getInfo())) {
        GST_ERROR("Audio format renegotiation is not possible yet.");
        m_sampleConverter = nullptr;
    }

    if (!m_sampleConverter) {
        m_inputStreamDescription = desc.getInfo();
        m_outputStreamDescription = libwebrtcAudioFormat(LibWebRTCAudioFormat::sampleRate, desc.numberOfChannels());
#ifndef GST_DISABLE_GST_DEBUG
        GRefPtr inputCaps = adoptGRef(gst_audio_info_to_caps(&m_inputStreamDescription));
        GRefPtr outputCaps = adoptGRef(gst_audio_info_to_caps(&m_outputStreamDescription));
        GST_TRACE("Converting from %" GST_PTR_FORMAT " to %" GST_PTR_FORMAT, inputCaps.get(), outputCaps.get());
#endif
        m_sampleConverter.reset(gst_audio_converter_new(GST_AUDIO_CONVERTER_FLAG_IN_WRITABLE, &m_inputStreamDescription,
            &m_outputStreamDescription, nullptr));
    }

    {
        Locker locker { m_adapterLock };
        const auto& sample = data.getSample();
        auto* buffer = gst_sample_get_buffer(sample.get());
        gst_adapter_push(m_adapter.get(), gst_buffer_ref(buffer));
    }
    LibWebRTCProvider::callOnWebRTCSignalingThread([protectedThis = protect(*this)] {
        protectedThis->pullAudioData();
    });
}

void RealtimeOutgoingAudioSourceLibWebRTC::pullAudioData()
{
    if (!GST_AUDIO_INFO_IS_VALID(&m_inputStreamDescription) || !GST_AUDIO_INFO_IS_VALID(&m_outputStreamDescription)) {
        GST_INFO("No stream description set yet.");
        return;
    }

    size_t outChunkSampleCount = LibWebRTCAudioFormat::chunkSampleCount;
    size_t outBufferSize = outChunkSampleCount * m_outputStreamDescription.bpf;

    Locker locker { m_adapterLock };
    size_t inChunkSampleCount = gst_audio_converter_get_in_frames(m_sampleConverter.get(), outChunkSampleCount);
    size_t inBufferSize = inChunkSampleCount * m_inputStreamDescription.bpf;

    while (gst_adapter_available(m_adapter.get()) > inBufferSize) {
        GRefPtr inBuffer = adoptGRef(gst_adapter_take_buffer(m_adapter.get(), inBufferSize));
        m_audioBuffer.grow(outBufferSize);
        if (isSilenced()) {
            GST_TRACE("Audio buffer will contain silence");
            webkitGstAudioFormatFillSilence(m_outputStreamDescription.finfo, m_audioBuffer.mutableSpan().data(), outBufferSize);
        } else {
            GstMappedBuffer inMap(inBuffer, GST_MAP_READWRITE);

            gpointer in[1] = { inMap.data() };
            gpointer out[1] = { m_audioBuffer.mutableSpan().data() };
            if (!gst_audio_converter_samples(m_sampleConverter.get(), static_cast<GstAudioConverterFlags>(0), in, inChunkSampleCount, out, outChunkSampleCount)) {
                GST_ERROR("Could not convert samples.");
                return;
            }
        }

        GST_TRACE("Sending audio frames");
        sendAudioFrames(m_audioBuffer.span(), LibWebRTCAudioFormat::sampleSize, GST_AUDIO_INFO_RATE(&m_outputStreamDescription),
            GST_AUDIO_INFO_CHANNELS(&m_outputStreamDescription), outChunkSampleCount);
    }
}
'''


EDEN_TRANSFER = '''void RealtimeOutgoingAudioSourceLibWebRTC::audioSamplesAvailable(const MediaTime&, const PlatformAudioData& audioData, const AudioStreamDescription& streamDescription, size_t)
{
    DisableMallocRestrictionsForCurrentThreadScope disableMallocRestrictions;
    const auto& data = static_cast<const GStreamerAudioData&>(audioData);
    const auto& desc = static_cast<const GStreamerAudioStreamDescription&>(streamDescription);
    auto* buffer = gst_sample_get_buffer(data.getSample().get());
    if (!buffer)
        return;

    {
        Locker locker { m_conversionLock };
        if (!m_sampleConverter || !gst_audio_info_is_equal(&m_inputStreamDescription, &desc.getInfo())) {
            if (m_sampleConverter)
                GST_DEBUG("Input audio format changed, dropping %zu converted bytes", m_convertedAudio.size());
            m_convertedAudio.clear();
            m_sampleConverter = nullptr;
            m_inputStreamDescription = desc.getInfo();
            m_outputStreamDescription = libwebrtcAudioFormat(LibWebRTCAudioFormat::sampleRate, desc.numberOfChannels());
#ifndef GST_DISABLE_GST_DEBUG
            GRefPtr inputCaps = adoptGRef(gst_audio_info_to_caps(&m_inputStreamDescription));
            GRefPtr outputCaps = adoptGRef(gst_audio_info_to_caps(&m_outputStreamDescription));
            GST_DEBUG("Converting from %" GST_PTR_FORMAT " to %" GST_PTR_FORMAT, inputCaps.get(), outputCaps.get());
#endif
            m_sampleConverter.reset(gst_audio_converter_new(GST_AUDIO_CONVERTER_FLAG_NONE, &m_inputStreamDescription, &m_outputStreamDescription, nullptr));
            if (!m_sampleConverter) {
                GST_ERROR("Unable to convert the captured audio format for LibWebRTC");
                return;
            }
        }

        GstAudioBuffer captured;
        if (!gst_audio_buffer_map(&captured, &m_inputStreamDescription, buffer, GST_MAP_READ)) {
            GST_WARNING("Could not map %zu bytes of captured audio", gst_buffer_get_size(buffer));
            return;
        }
        size_t inFrames = GST_AUDIO_BUFFER_N_SAMPLES(&captured);
        size_t outFrameSize = GST_AUDIO_INFO_BPF(&m_outputStreamDescription);
        size_t outFrames = gst_audio_converter_get_out_frames(m_sampleConverter.get(), inFrames);
        size_t offset = m_convertedAudio.size();
        m_convertedAudio.grow(offset + outFrames * outFrameSize);
        auto output = m_convertedAudio.mutableSpan().subspan(offset);
        gpointer out[1] = { output.data() };
        bool converted = gst_audio_converter_samples(m_sampleConverter.get(), GST_AUDIO_CONVERTER_FLAG_NONE, captured.planes, inFrames, out, outFrames);
        gst_audio_buffer_unmap(&captured);
        if (!converted)
            GST_WARNING("Could not convert %zu captured audio frames", inFrames);
        if ((!converted || isSilenced()) && !output.empty())
            webkitGstAudioFormatFillSilence(m_outputStreamDescription.finfo, output.data(), output.size());

        size_t maximumPendingSize = maximumPendingChunks * LibWebRTCAudioFormat::chunkSampleCount * outFrameSize;
        if (m_convertedAudio.size() > maximumPendingSize) {
            size_t excess = m_convertedAudio.size() - maximumPendingSize;
            GST_WARNING("Dropping %zu bytes of captured audio that LibWebRTC has not taken", excess);
            m_convertedAudio.removeAt(0, excess);
        }
    }
    LibWebRTCProvider::callOnWebRTCSignalingThread([protectedThis = protect(*this)] {
        protectedThis->pullAudioData();
    });
}

void RealtimeOutgoingAudioSourceLibWebRTC::pullAudioData()
{
    const size_t outChunkSampleCount = LibWebRTCAudioFormat::chunkSampleCount;
    while (true) {
        int sampleRate = 0;
        size_t channelCount = 0;
        {
            Locker locker { m_conversionLock };
            if (!m_sampleConverter)
                return;

            size_t outChunkSize = outChunkSampleCount * GST_AUDIO_INFO_BPF(&m_outputStreamDescription);
            if (!outChunkSize || m_convertedAudio.size() < outChunkSize)
                return;

            m_audioBuffer.resize(outChunkSize);
            memcpySpan(m_audioBuffer.mutableSpan(), m_convertedAudio.span().first(outChunkSize));
            m_convertedAudio.removeAt(0, outChunkSize);
            sampleRate = GST_AUDIO_INFO_RATE(&m_outputStreamDescription);
            channelCount = GST_AUDIO_INFO_CHANNELS(&m_outputStreamDescription);
        }

        GST_TRACE("Sending audio frames");
        sendAudioFrames(m_audioBuffer.span(), LibWebRTCAudioFormat::sampleSize, sampleRate, channelCount, outChunkSampleCount);
    }
}
'''


def outgoing_audio_patch(text):
    text = replace_once(
        text,
        "    m_adapter = adoptGRef(gst_adapter_new()),\n    m_sampleConverter = nullptr;\n",
        "    Locker locker { m_conversionLock };\n"
        "    gst_audio_info_init(&m_inputStreamDescription);\n"
        "    gst_audio_info_init(&m_outputStreamDescription);\n",
    )
    text = replace_once(
        text,
        "RealtimeOutgoingAudioSourceLibWebRTC::~RealtimeOutgoingAudioSourceLibWebRTC()\n{\n    m_sampleConverter = nullptr;\n}\n",
        "RealtimeOutgoingAudioSourceLibWebRTC::~RealtimeOutgoingAudioSourceLibWebRTC()\n{\n"
        "    Locker locker { m_conversionLock };\n"
        "    m_sampleConverter = nullptr;\n"
        "}\n",
    )
    text = replace_once(
        text,
        "namespace WebCore {\n",
        "namespace WebCore {\n\nstatic constexpr size_t maximumPendingChunks = 50;\n",
    )
    return replace_once(text, UPSTREAM_TRANSFER, EDEN_TRANSFER)


ENCODER_FACTORY = GLUE + "GStreamerVideoEncoderFactory.cpp"
ENCODER_FACTORY_HEADER = GLUE + "GStreamerVideoEncoderFactory.h"
DECODER_FACTORY = GLUE + "GStreamerVideoDecoderFactory.cpp"
DECODER_FACTORY_HEADER = GLUE + "GStreamerVideoDecoderFactory.h"
PROVIDER = GLUE + "LibWebRTCProviderGStreamer.cpp"
VIDEO_FRAME = GLUE + "GStreamerVideoFrameLibWebRTC.cpp"
VIDEO_FRAME_HEADER = GLUE + "GStreamerVideoFrameLibWebRTC.h"
OUTGOING_VIDEO = GLUE + "RealtimeOutgoingVideoSourceLibWebRTC.cpp"
INCOMING_VIDEO = GLUE + "RealtimeIncomingVideoSourceLibWebRTC.cpp"
AUDIO_MODULE = "Source/WebCore/platform/mediastream/libwebrtc/LibWebRTCAudioModule.cpp"
AUDIO_MODULE_HEADER = "Source/WebCore/platform/mediastream/libwebrtc/LibWebRTCAudioModule.h"
VIDEO_ENCODER = "Source/WebCore/platform/graphics/gstreamer/VideoEncoderGStreamer.cpp"
VIDEO_ENCODER_HEADER = "Source/WebCore/platform/graphics/gstreamer/VideoEncoderGStreamer.h"
GSTREAMER_CHECKS = "Source/cmake/GStreamerChecks.cmake"
LIBWEBRTC_CMAKE = "Source/ThirdParty/libwebrtc/CMakeLists.txt"


def replace_span(text, start, end, replacement):
    if text.count(start) != 1:
        raise RuntimeError("The pinned source does not match the libwebrtc glue patch: " + start[:80])
    begin = text.index(start)
    finish = text.index(end, begin) + len(end)
    return text[:begin] + replacement + text[finish:]


ENCODER_IMPLEMENTATION = r'''class GStreamerEncodedImageBuffer : public webrtc::EncodedImageBufferInterface {
public:
    static webrtc::scoped_refptr<GStreamerEncodedImageBuffer> create(Vector<uint8_t>&& data)
    {
        return webrtc::make_ref_counted<GStreamerEncodedImageBuffer>(WTF::move(data));
    }

    const uint8_t* data() const override { return m_data.span().data(); }
    size_t size() const override { return m_data.size(); }

protected:
    explicit GStreamerEncodedImageBuffer(Vector<uint8_t>&& data)
        : m_data(WTF::move(data))
    {
    }

private:
    Vector<uint8_t> m_data;
};

static constexpr unsigned h264KeyFrameInterval = 3000;
static constexpr size_t maximumPendingFrames = 64;
static constexpr double maximumDeclaredFrameRate = 30;
static constexpr size_t measuredFrameIntervals = 5;
static constexpr size_t recentFrameIntervals = 3;
static constexpr double minimumRateFactor = 0.25;
static constexpr double maximumRateFactor = 4;
static constexpr double rateFactorStep = 0.1;

static std::vector<webrtc::SdpVideoFormat> h264EncoderFormats()
{
    std::vector<webrtc::SdpVideoFormat> formats;
    for (const char* profileLevelId : { "42001f", "42e01f", "4d001f" })
        formats.emplace_back("H264", webrtc::CodecParameterMap { { "level-asymmetry-allowed", "1" }, { "packetization-mode", "1" }, { "profile-level-id", profileLevelId } });
    return formats;
}

class LibWebRTCGStreamerVideoEncoder final : public webrtc::VideoEncoder {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(LibWebRTCGStreamerVideoEncoder);
public:
    explicit LibWebRTCGStreamerVideoEncoder(const webrtc::SdpVideoFormat& sdpVideoFormat)
        : m_sdpVideoFormat(sdpVideoFormat)
    {
        m_codecInfo.codecType = webrtc::kVideoCodecH264;
        auto packetizationMode = m_sdpVideoFormat.parameters.find("packetization-mode");
        bool singleNalUnit = packetizationMode != m_sdpVideoFormat.parameters.end() && packetizationMode->second == "0";
        m_codecInfo.codecSpecific.H264.packetization_mode = singleNalUnit ? webrtc::H264PacketizationMode::SingleNalUnit : webrtc::H264PacketizationMode::NonInterleaved;
        m_codecInfo.codecSpecific.H264.temporal_idx = webrtc::kNoTemporalIdx;
        m_codecInfo.codecSpecific.H264.base_layer_sync = false;
        m_codecInfo.codecSpecific.H264.idr_frame = false;
    }

    ~LibWebRTCGStreamerVideoEncoder()
    {
        Release();
    }

    int32_t InitEncode(const webrtc::VideoCodec* codecSettings, const webrtc::VideoEncoder::Settings&) final
    {
        if (!codecSettings || codecSettings->codecType != webrtc::kVideoCodecH264 || !codecSettings->width || !codecSettings->height)
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;

        if (codecSettings->numberOfSimulcastStreams > 1)
            return WEBRTC_VIDEO_CODEC_ERR_SIMULCAST_PARAMETERS_NOT_SUPPORTED;

        m_size = { static_cast<int>(codecSettings->width), static_cast<int>(codecSettings->height) };
        m_frameRate = std::clamp<double>(codecSettings->maxFramerate ? codecSettings->maxFramerate : maximumDeclaredFrameRate, 1, maximumDeclaredFrameRate);
        m_bitRate = static_cast<uint64_t>(codecSettings->startBitrate) * 1000;
        return createInternalEncoder();
    }

    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) final
    {
        m_encodedImageCallback = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() final
    {
        if (RefPtr encoder = std::exchange(m_internalEncoder, nullptr))
            encoder->close();
        m_pendingFrames.clear();
        return WEBRTC_VIDEO_CODEC_OK;
    }

    void SetRates(const webrtc::VideoEncoder::RateControlParameters& parameters) final
    {
        m_inputFrameRate = m_framesSeen >= 2 ? parameters.framerate_fps : 0;
        m_bitRate = parameters.bitrate.get_sum_bps();
        updateRateFactor();
        applyRates();
    }

    VideoEncoder::EncoderInfo GetEncoderInfo() const final
    {
        VideoEncoder::EncoderInfo info;
        info.implementation_name = "GStreamer";
        info.is_hardware_accelerated = m_isHardwareAccelerated;
        info.has_trusted_rate_controller = false;
        info.supports_simulcast = false;
        info.requested_resolution_alignment = 2;
        info.preferred_pixel_formats = { webrtc::VideoFrameBuffer::Type::kI420 };
        info.supports_native_handle = m_acceptsDMABuf;
        return info;
    }

    int32_t Encode(const webrtc::VideoFrame& frame, const std::vector<webrtc::VideoFrameType>* frameTypes) final
    {
        if (!m_internalEncoder || !m_encodedImageCallback) [[unlikely]]
            return WEBRTC_VIDEO_CODEC_UNINITIALIZED;

        bool shouldGenerateKeyFrame = false;
        if (frameTypes) {
            for (auto frameType : *frameTypes) {
                if (frameType == webrtc::VideoFrameType::kVideoFrameKey)
                    shouldGenerateKeyFrame = true;
            }
        }

        IntSize frameSize { frame.width(), frame.height() };
        bool encoderScalesFrame = m_acceptsDMABuf && frame.video_frame_buffer() && frame.video_frame_buffer()->type() == webrtc::VideoFrameBuffer::Type::kNative;
        if (!encoderScalesFrame && frameSize != m_size) {
            GST_INFO("Input frame size changed from %dx%d to %dx%d, recreating the encoder", m_size.width(), m_size.height(), frameSize.width(), frameSize.height());
            m_size = frameSize;
            if (auto result = createInternalEncoder(); result != WEBRTC_VIDEO_CODEC_OK)
                return result;
            shouldGenerateKeyFrame = true;
        }

        auto sample = convertLibWebRTCVideoFrameToGStreamerSample(frame, m_acceptsDMABuf);
        if (!sample) [[unlikely]] {
            GST_WARNING("Unable to wrap a %dx%d frame for encoding", frame.width(), frame.height());
            return WEBRTC_VIDEO_CODEC_ERROR;
        }

        noteFrameTime(frame.timestamp_us());
        if (updateRateFactor())
            applyRates();

        m_pendingFrames.append({ frame.render_time_ms(), frame.rtp_timestamp() });
        while (m_pendingFrames.size() > maximumPendingFrames)
            m_pendingFrames.removeFirst();

        VideoFrameGStreamer::CreateOptions options;
        options.presentationSize = m_size;
        auto colorSpace = colorSpaceFromLibWebRTCVideoFrame(frame);
        auto gstVideoFrame = VideoFrameGStreamer::create(WTF::move(sample), options, colorSpace.value_or(PlatformVideoColorSpace { }));
        WebCore::VideoEncoder::RawFrame rawFrame { WTF::move(gstVideoFrame), frame.render_time_ms(), { } };
        if (m_internalEncoder->encodeSync(WTF::move(rawFrame), shouldGenerateKeyFrame))
            return WEBRTC_VIDEO_CODEC_OK;

        return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
    }

private:
    struct PendingFrame {
        int64_t renderTimeMs { 0 };
        uint32_t rtpTimestamp { 0 };
    };

    int32_t createInternalEncoder()
    {
        Release();

        StringBuilder builder;
        builder.append("avc1"_s);
        auto profileLevelId = m_sdpVideoFormat.parameters.find("profile-level-id");
        if (profileLevelId != m_sdpVideoFormat.parameters.end() && !profileLevelId->second.empty())
            builder.append('.', fromStdString(profileLevelId->second));
        auto codecName = builder.toString();

        WebCore::VideoEncoder::Config config;
        config.width = m_size.width();
        config.height = m_size.height();
        config.useAnnexB = true;
        config.bitRate = encoderBitRate();
        config.frameRate = m_frameRate;
        config.isRealtime = true;

        GST_INFO("Creating %dx%d H.264 encoder for codec %s", m_size.width(), m_size.height(), codecName.ascii().data());
        auto result = GStreamerVideoEncoder::create(codecName, config, [](auto&&) { }, [this](auto&& encodedFrame) {
            notifyEncodedFrame(WTF::move(encodedFrame));
        });
        if (!result) {
            GST_ERROR("Unable to create a GStreamer H.264 encoder: %s", result.error().utf8().data());
            return WEBRTC_VIDEO_CODEC_ERROR;
        }

        m_internalEncoder = WTF::move(*result);
        m_internalEncoder->setKeyFrameIntervalSync(h264KeyFrameInterval);
        m_isHardwareAccelerated = m_internalEncoder->isHardwareAccelerated();
        m_acceptsDMABuf = m_internalEncoder->acceptsDMABuf();
        GST_INFO("The H.264 encoder is %shardware accelerated and %s DMA-BUF frames", m_isHardwareAccelerated ? "" : "not ", m_acceptsDMABuf ? "takes" : "does not take");
        m_encoderBitRate = 0;
        applyRates();
        return WEBRTC_VIDEO_CODEC_OK;
    }

    void noteFrameTime(int64_t timestampUs)
    {
        if (m_framesSeen < 2)
            ++m_framesSeen;
        if (!m_frameTimes.isEmpty() && timestampUs <= m_frameTimes.last()) {
            if (timestampUs < m_frameTimes.last())
                m_frameTimes.clear();
            else
                return;
        }
        m_frameTimes.append(timestampUs);
        if (m_frameTimes.size() > measuredFrameIntervals + 1)
            m_frameTimes.removeAt(0);
    }

    double measuredFrameRate() const
    {
        size_t available = m_frameTimes.isEmpty() ? 0 : m_frameTimes.size() - 1;
        if (available < recentFrameIntervals)
            return 0;
        std::array<int64_t, measuredFrameIntervals> intervals;
        auto medianInterval = [&](size_t count) {
            for (size_t index = 0; index < count; ++index) {
                size_t newer = m_frameTimes.size() - count + index;
                intervals[index] = m_frameTimes[newer] - m_frameTimes[newer - 1];
            }
            std::sort(intervals.begin(), intervals.begin() + count);
            return intervals[count / 2];
        };
        auto interval = medianInterval(recentFrameIntervals);
        if (available >= measuredFrameIntervals)
            interval = std::min(interval, medianInterval(measuredFrameIntervals));
        return interval > 0 ? 1000000.0 / interval : 0;
    }

    bool updateRateFactor()
    {
        double frameRate = std::max(m_inputFrameRate, measuredFrameRate());
        if (frameRate < 1)
            return false;
        double factor = std::clamp(m_frameRate / frameRate, minimumRateFactor, maximumRateFactor);
        if (std::abs(factor / m_rateFactor - 1) < rateFactorStep)
            return false;
        GST_DEBUG("Frames arrive at %.1f fps for a %.0f fps H.264 encoder, scaling its bitrate by %.2f", frameRate, m_frameRate, factor);
        m_rateFactor = factor;
        return true;
    }

    uint64_t encoderBitRate() const
    {
        return static_cast<uint64_t>(std::llround(m_bitRate * m_rateFactor));
    }

    void applyRates()
    {
        if (!m_internalEncoder || !m_bitRate)
            return;
        auto bitRate = encoderBitRate();
        if (bitRate == m_encoderBitRate)
            return;
        m_encoderBitRate = bitRate;
        m_internalEncoder->setRatesSync(bitRate, m_frameRate);
    }

    std::optional<uint32_t> takeRtpTimestamp(int64_t renderTimeMs)
    {
        while (!m_pendingFrames.isEmpty() && m_pendingFrames.first().renderTimeMs < renderTimeMs)
            m_pendingFrames.removeFirst();
        if (m_pendingFrames.isEmpty() || m_pendingFrames.first().renderTimeMs != renderTimeMs)
            return std::nullopt;
        return m_pendingFrames.takeFirst().rtpTimestamp;
    }

    void notifyEncodedFrame(WebCore::VideoEncoder::EncodedFrame&& frame)
    {
        if (!m_encodedImageCallback || frame.data.isEmpty()) [[unlikely]]
            return;

        auto rtpTimestamp = takeRtpTimestamp(frame.timestamp);
        if (!rtpTimestamp) {
            GST_WARNING("Dropping an encoded frame with no matching input frame at %" G_GINT64_FORMAT " ms", frame.timestamp);
            return;
        }

        webrtc::EncodedImage encodedImage;
        encodedImage.SetEncodedData(GStreamerEncodedImageBuffer::create(WTF::move(frame.data)));
        encodedImage._encodedWidth = m_size.width();
        encodedImage._encodedHeight = m_size.height();
        encodedImage.SetFrameType(frame.isKeyFrame ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta);
        encodedImage.SetRtpTimestamp(*rtpTimestamp);
        encodedImage.capture_time_ms_ = frame.timestamp;
        m_codecInfo.codecSpecific.H264.idr_frame = frame.isKeyFrame;
        m_encodedImageCallback->OnEncodedImage(encodedImage, &m_codecInfo);
    }

    webrtc::SdpVideoFormat m_sdpVideoFormat;
    webrtc::CodecSpecificInfo m_codecInfo;
    RefPtr<GStreamerVideoEncoder> m_internalEncoder;
    webrtc::EncodedImageCallback* m_encodedImageCallback { nullptr };
    IntSize m_size;
    double m_frameRate { maximumDeclaredFrameRate };
    double m_inputFrameRate { 0 };
    double m_rateFactor { 1 };
    unsigned m_framesSeen { 0 };
    uint64_t m_bitRate { 0 };
    uint64_t m_encoderBitRate { 0 };
    Vector<int64_t, measuredFrameIntervals + 1> m_frameTimes;
    bool m_isHardwareAccelerated { false };
    bool m_acceptsDMABuf { false };
    Deque<PendingFrame> m_pendingFrames;
};

class GStreamerH264EncoderFactory final : public webrtc::VideoEncoderFactory {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(GStreamerH264EncoderFactory);
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const final { return h264EncoderFormats(); }

    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment&, const webrtc::SdpVideoFormat& format) final
    {
        return std::make_unique<LibWebRTCGStreamerVideoEncoder>(format);
    }
};

class LibWebRTCInternalVideoEncoderFactory final : public webrtc::VideoEncoderFactory {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(LibWebRTCInternalVideoEncoderFactory);
public:
    LibWebRTCInternalVideoEncoderFactory(bool isSupportingVP9Profile0, bool isSupportingVP9Profile2, bool isSupportingAV1)
        : m_isSupportingVP9Profile0(isSupportingVP9Profile0)
        , m_isSupportingVP9Profile2(isSupportingVP9Profile2)
        , m_isSupportingAV1(isSupportingAV1)
        , m_isSupportingH264(GStreamerRegistryScanner::singleton().isCodecSupported(GStreamerRegistryScanner::Configuration::Encoding, "avc1"_s))
    {
    }

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const final
    {
        std::vector<webrtc::SdpVideoFormat> formats = webrtc::LibvpxVp8EncoderTemplateAdapter::SupportedFormats();
        if (m_isSupportingH264) {
            for (auto& format : m_h264Factory.GetSupportedFormats())
                formats.push_back(WTF::move(format));
        }
#if USE(LIBWEBRTC_AV1)
        if (m_isSupportingAV1) {
            for (auto& format : webrtc::LibaomAv1EncoderTemplateAdapter::SupportedFormats())
                formats.push_back(WTF::move(format));
        }
#endif
        for (auto& format : webrtc::SupportedVP9Codecs(true)) {
            if ((m_isSupportingVP9Profile0 && format.IsSameCodec(webrtc::SdpVideoFormat::VP9Profile0())) || (m_isSupportingVP9Profile2 && format.IsSameCodec(webrtc::SdpVideoFormat::VP9Profile2())))
                formats.push_back(WTF::move(format));
        }
        return formats;
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& environment, const webrtc::SdpVideoFormat& format) final
    {
        if (format.IsSameCodec(webrtc::SdpVideoFormat::VP8()))
            return webrtc::CreateVp8Encoder(environment);
        if (format.IsSameCodec(webrtc::SdpVideoFormat::VP9Profile0()))
            return webrtc::CreateVp9Encoder(environment, { webrtc::VP9Profile::kProfile0 });
        if (format.IsSameCodec(webrtc::SdpVideoFormat::VP9Profile2()))
            return webrtc::CreateVp9Encoder(environment, { webrtc::VP9Profile::kProfile2 });
#if USE(LIBWEBRTC_AV1)
        if (m_isSupportingAV1 && format.IsSameCodec(webrtc::SdpVideoFormat::AV1Profile0()))
            return webrtc::CreateLibaomAv1Encoder(environment);
#endif
        if (m_isSupportingH264 && equalLettersIgnoringASCIICase(fromStdString(format.name), "h264"_s))
            return m_h264Factory.Create(environment, format);
        return nullptr;
    }

private:
    bool m_isSupportingVP9Profile0;
    bool m_isSupportingVP9Profile2;
    bool m_isSupportingAV1;
    bool m_isSupportingH264;
    GStreamerH264EncoderFactory m_h264Factory;
};

std::unique_ptr<webrtc::VideoEncoder> GStreamerVideoEncoderFactory::Create(const webrtc::Environment& environment, const webrtc::SdpVideoFormat& format)
{
    if (!format.IsCodecInList(m_internalFactory->GetSupportedFormats())) {
        GST_WARNING("No encoder for %s", format.ToString().c_str());
        return nullptr;
    }
    return std::make_unique<webrtc::SimulcastEncoderAdapter>(environment, m_internalFactory.get(), nullptr, format);
}

GStreamerVideoEncoderFactory::GStreamerVideoEncoderFactory(bool isSupportingVP9Profile0, bool isSupportingVP9Profile2, bool isSupportingAV1)
{
    ensureGStreamerInitialized();
    registerWebKitGStreamerElements();

    static std::once_flag debugRegisteredFlag;
    std::call_once(debugRegisteredFlag, [] {
        GST_DEBUG_CATEGORY_INIT(webkit_webrtcenc_debug, "webkitlibwebrtcvideoencoder", 0, "WebKit WebRTC video encoder");
    });
    m_internalFactory = std::make_unique<LibWebRTCInternalVideoEncoderFactory>(isSupportingVP9Profile0, isSupportingVP9Profile2, isSupportingAV1);
}

GStreamerVideoEncoderFactory::~GStreamerVideoEncoderFactory() = default;

std::vector<webrtc::SdpVideoFormat> GStreamerVideoEncoderFactory::GetSupportedFormats() const
{
    return m_internalFactory->GetSupportedFormats();
}

'''


def encoder_factory_patch(text):
    text = replace_once(
        text,
        '#include "webrtc/modules/video_coding/include/video_error_codes.h"\n',
        '#include "webrtc/modules/video_coding/include/video_error_codes.h"\n'
        '#include "webrtc/api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"\n'
        '#include "webrtc/media/engine/simulcast_encoder_adapter.h"\n'
        '#include "webrtc/modules/video_coding/codecs/vp8/include/vp8.h"\n'
        "#if USE(LIBWEBRTC_AV1)\n"
        '#include "webrtc/api/video_codecs/video_encoder_factory_template_libaom_av1_adapter.h"\n'
        "#endif\n",
    )
    text = replace_once(text, "#include <wtf/HashMap.h>\n", "#include <algorithm>\n#include <array>\n#include <cmath>\n#include <wtf/Deque.h>\n#include <wtf/HashMap.h>\n#include <wtf/Vector.h>\n")
    return replace_span(
        text,
        "class GStreamerEncodedImageBuffer : public webrtc::EncodedImageBufferInterface {\n",
        "    return supportedFormats;\n}\n\n",
        ENCODER_IMPLEMENTATION,
    )


def encoder_factory_header_patch(text):
    text = replace_once(text, "#include <wtf/TZoneMalloc.h>\n", "#include <memory>\n#include <wtf/TZoneMalloc.h>\n")
    return replace_once(
        text,
        "    GStreamerVideoEncoderFactory(bool isSupportingVP9Profile0, bool isSupportingVP9Profile2);\n"
        "\n"
        "    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const final;\n"
        "    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment&, const webrtc::SdpVideoFormat&) final;\n"
        "\n"
        "private:\n"
        "    bool m_isSupportingVP9Profile0;\n"
        "    bool m_isSupportingVP9Profile2;\n",
        "    GStreamerVideoEncoderFactory(bool isSupportingVP9Profile0, bool isSupportingVP9Profile2, bool isSupportingAV1);\n"
        "    ~GStreamerVideoEncoderFactory();\n"
        "\n"
        "    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const final;\n"
        "    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment&, const webrtc::SdpVideoFormat&) final;\n"
        "\n"
        "private:\n"
        "    std::unique_ptr<webrtc::VideoEncoderFactory> m_internalFactory;\n",
    )


DECODER_FACTORY_IMPLEMENTATION = r'''std::unique_ptr<webrtc::VideoDecoder> GStreamerVideoDecoderFactory::Create(const webrtc::Environment& environment, const webrtc::SdpVideoFormat& format)
{
    auto formatName = fromStdString(format.name);
    if (WTF::equalLettersIgnoringASCIICase(formatName, "h264"_s))
        return std::unique_ptr<webrtc::VideoDecoder>(new H264Decoder());
    if (WTF::equalLettersIgnoringASCIICase(formatName, "vp8"_s))
        return std::make_unique<webrtc::LibvpxVp8Decoder>(environment);
    if (WTF::equalLettersIgnoringASCIICase(formatName, "vp9"_s))
        return webrtc::VP9Decoder::Create();
#if USE(LIBWEBRTC_AV1)
    if (m_isSupportingAV1 && WTF::equalLettersIgnoringASCIICase(formatName, "av1"_s))
        return webrtc::CreateDav1dDecoder(environment);
#endif

    GST_ERROR("Could not create decoder for %s", formatName.ascii().data());
    return nullptr;
}

GStreamerVideoDecoderFactory::GStreamerVideoDecoderFactory(bool isSupportingVP9Profile0, bool isSupportingVP9Profile2, bool isSupportingAV1)
    : m_isSupportingVP9Profile0(isSupportingVP9Profile0)
    , m_isSupportingVP9Profile2(isSupportingVP9Profile2)
    , m_isSupportingAV1(isSupportingAV1)
{
    ensureGStreamerInitialized();

    static std::once_flag debugRegisteredFlag;
    std::call_once(debugRegisteredFlag, [] {
        GST_DEBUG_CATEGORY_INIT(webkit_webrtcdec_debug, "webkitlibwebrtcvideodecoder", 0, "WebKit WebRTC video decoder");
    });
}

std::vector<webrtc::SdpVideoFormat> GStreamerVideoDecoderFactory::GetSupportedFormats() const
{
    std::vector<webrtc::SdpVideoFormat> formats { webrtc::SdpVideoFormat::VP8() };
    H264Decoder().addDecoderIfSupported(formats);
#if USE(LIBWEBRTC_AV1)
    if (m_isSupportingAV1) {
        for (auto& format : webrtc::Dav1dDecoderTemplateAdapter::SupportedFormats())
            formats.push_back(WTF::move(format));
    }
#endif
    if (m_isSupportingVP9Profile0)
        formats.push_back(webrtc::SdpVideoFormat::VP9Profile0());
    if (m_isSupportingVP9Profile2)
        formats.push_back(webrtc::SdpVideoFormat::VP9Profile2());
    return formats;
}

'''


def decoder_factory_patch(text):
    text = replace_once(
        text,
        '#include "webrtc/modules/video_coding/codecs/vp8/libvpx_vp8_decoder.h"\n',
        '#include "webrtc/modules/video_coding/codecs/vp8/libvpx_vp8_decoder.h"\n'
        '#include "webrtc/modules/video_coding/codecs/vp9/include/vp9.h"\n'
        "#if USE(LIBWEBRTC_AV1)\n"
        '#include "webrtc/api/video_codecs/video_decoder_factory_template_dav1d_adapter.h"\n'
        "#endif\n",
    )
    text = replace_once(text, "#include <gst/video/video.h>\n", "#include <atomic>\n#include <gst/video/video.h>\n")
    text = replace_once(
        text,
        "    GStreamerWebRTCVideoDecoder()\n        : m_requireParse(false)\n        , m_needsKeyframe(true)\n    {\n    }\n",
        "    GStreamerWebRTCVideoDecoder() = default;\n",
    )
    text = replace_once(
        text,
        '            GST_ERROR_OBJECT(pipeline(), "Could not link src to decoder.");\n            return WEBRTC_VIDEO_CODEC_ERROR;\n',
        '            GST_ERROR_OBJECT(pipeline(), "Could not link src to decoder.");\n            return false;\n',
    )
    text = replace_once(
        text,
        '            GST_ERROR_OBJECT(pipeline(), "Could not set state to PLAYING.");\n            return WEBRTC_VIDEO_CODEC_ERROR;\n',
        '            GST_ERROR_OBJECT(pipeline(), "Could not set state to PLAYING.");\n            return false;\n',
    )
    text = replace_once(
        text,
        "        GRefPtr pulledSample = adoptGRef(gst_app_sink_try_pull_sample(GST_APP_SINK_CAST(m_sink.get()), GST_SECOND));\n",
        "        GRefPtr pulledSample = adoptGRef(gst_app_sink_try_pull_sample(GST_APP_SINK_CAST(m_sink.get()), 200 * GST_MSECOND));\n",
    )
    text = replace_once(
        text,
        "        auto frame = convertGStreamerSampleToLibWebRTCVideoFrame(WTF::move(pulledSample), rtpTimestamp);\n"
        "\n"
        "        m_imageReadyCb->Decoded(frame);\n",
        "        auto frame = convertGStreamerSampleToLibWebRTCVideoFrame(WTF::move(pulledSample), rtpTimestamp);\n"
        "        if (!frame) {\n"
        '            GST_WARNING_OBJECT(pipeline(), "Unable to wrap the decoded frame, requesting a key frame");\n'
        "            m_needsKeyframe = true;\n"
        "            return WEBRTC_VIDEO_CODEC_ERROR;\n"
        "        }\n"
        "\n"
        "        if (!m_imageReadyCb)\n"
        "            return WEBRTC_VIDEO_CODEC_UNINITIALIZED;\n"
        "        m_imageReadyCb->Decoded(*frame);\n",
    )
    text = replace_once(
        text,
        "    bool m_requireParse = false;\n    bool m_needsKeyframe;\n",
        "    bool m_requireParse = false;\n    std::atomic<bool> m_needsKeyframe { true };\n",
    )
    text = replace_once(
        text,
        "    webrtc::DecodedImageCallback* m_imageReadyCb;\n",
        "    webrtc::DecodedImageCallback* m_imageReadyCb { nullptr };\n",
    )
    text = replace_once(
        text,
        "        if (codecSettings.codec_type() != webrtc::kVideoCodecH264)\n            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;\n",
        "        if (codecSettings.codec_type() != webrtc::kVideoCodecH264)\n            return false;\n",
    )
    return replace_span(
        text,
        "std::unique_ptr<webrtc::VideoDecoder> GStreamerVideoDecoderFactory::Create(",
        "    return formats;\n}\n\n",
        DECODER_FACTORY_IMPLEMENTATION,
    )


def decoder_factory_header_patch(text):
    text = replace_once(
        text,
        "    GStreamerVideoDecoderFactory(bool isSupportingVP9Profile0, bool isSupportingVP9Profile2);\n",
        "    GStreamerVideoDecoderFactory(bool isSupportingVP9Profile0, bool isSupportingVP9Profile2, bool isSupportingAV1);\n",
    )
    return replace_once(
        text,
        "    bool m_isSupportingVP9Profile2;\n",
        "    bool m_isSupportingVP9Profile2;\n    bool m_isSupportingAV1;\n",
    )


def provider_patch(text):
    text = replace_once(
        text,
        "    return makeUnique<GStreamerVideoDecoderFactory>(isSupportingVP9Profile0(), isSupportingVP9Profile2());\n",
        "    return makeUnique<GStreamerVideoDecoderFactory>(isSupportingVP9Profile0(), isSupportingVP9Profile2(), isSupportingAV1());\n",
    )
    return replace_once(
        text,
        "    return makeUnique<GStreamerVideoEncoderFactory>(isSupportingVP9Profile0(), isSupportingVP9Profile2());\n",
        "    return makeUnique<GStreamerVideoEncoderFactory>(isSupportingVP9Profile0(), isSupportingVP9Profile2(), isSupportingAV1());\n",
    )


VIDEO_FRAME_IMPLEMENTATION = r'''static GStreamerVideoFrameLibWebRTC::MemoryType memoryTypeFromCaps(GstCaps* caps)
{
    auto* features = caps && gst_caps_get_size(caps) ? gst_caps_get_features(caps, 0) : nullptr;
    if (!features || gst_caps_features_is_equal(features, GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY))
        return GStreamerVideoFrameLibWebRTC::MemoryType::System;
    if (gst_caps_features_contains(features, "memory:DMABuf"))
        return GStreamerVideoFrameLibWebRTC::MemoryType::DMABuf;
    if (gst_caps_features_contains(features, "memory:GLMemory"))
        return GStreamerVideoFrameLibWebRTC::MemoryType::GL;
    return GStreamerVideoFrameLibWebRTC::MemoryType::Other;
}

static std::optional<IntSize> sampleSize(GstCaps* caps)
{
    auto* structure = caps && gst_caps_get_size(caps) ? gst_caps_get_structure(caps, 0) : nullptr;
    if (!structure)
        return std::nullopt;
    auto width = gstStructureGet<int>(structure, "width"_s);
    auto height = gstStructureGet<int>(structure, "height"_s);
    if (!width || !height || *width <= 0 || *height <= 0)
        return std::nullopt;
    return IntSize { *width, *height };
}

static bool isRGBSample(GstCaps* caps)
{
    auto info = VideoFrameGStreamer::infoFromCaps(GRefPtr(caps));
    return GST_VIDEO_INFO_FORMAT(&info.info) != GST_VIDEO_FORMAT_UNKNOWN && GST_VIDEO_INFO_IS_RGB(&info.info);
}

static webrtc::scoped_refptr<webrtc::I420BufferInterface> wrapI420Sample(GRefPtr<GstSample>&& sample, IntPoint offset, IntSize size)
{
    auto frame = makeUnique<GstMappedFrame>(sample, GST_MAP_READ);
    if (!*frame || frame->format() != GST_VIDEO_FORMAT_I420 || offset.x() % 2 || offset.y() % 2 || offset.x() + size.width() > frame->width() || offset.y() + size.height() > frame->height())
        return nullptr;

    int strideY = frame->componentStride(GST_VIDEO_COMP_Y);
    int strideU = frame->componentStride(GST_VIDEO_COMP_U);
    int strideV = frame->componentStride(GST_VIDEO_COMP_V);
    auto planeY = frame->componentData(GST_VIDEO_COMP_Y).subspan(static_cast<size_t>(offset.y()) * strideY + offset.x());
    auto planeU = frame->componentData(GST_VIDEO_COMP_U).subspan(static_cast<size_t>(offset.y() / 2) * strideU + offset.x() / 2);
    auto planeV = frame->componentData(GST_VIDEO_COMP_V).subspan(static_cast<size_t>(offset.y() / 2) * strideV + offset.x() / 2);
    return webrtc::WrapI420Buffer(size.width(), size.height(), planeY.data(), strideY, planeU.data(), strideU, planeV.data(), strideV, [frame = frame.release()] {
        delete frame;
    });
}

GRefPtr<GstSample> convertLibWebRTCVideoFrameToGStreamerSample(const webrtc::VideoFrame& frame, bool acceptsDMABuf)
{
    auto frameBuffer = frame.video_frame_buffer();
    if (!frameBuffer) [[unlikely]]
        return nullptr;

    if (frameBuffer->type() == webrtc::VideoFrameBuffer::Type::kNative) {
        auto& nativeBuffer = static_cast<GStreamerVideoFrameLibWebRTC&>(*frameBuffer);
        switch (nativeBuffer.memoryType()) {
        case GStreamerVideoFrameLibWebRTC::MemoryType::System:
            if (!nativeBuffer.isCropped() && !nativeBuffer.isScaled())
                return nativeBuffer.sample();
            break;
        case GStreamerVideoFrameLibWebRTC::MemoryType::DMABuf:
            if (acceptsDMABuf && !nativeBuffer.isCropped())
                return nativeBuffer.sample();
            break;
        case GStreamerVideoFrameLibWebRTC::MemoryType::GL:
        case GStreamerVideoFrameLibWebRTC::MemoryType::Other:
            break;
        }
    }

    webrtc::scoped_refptr<webrtc::I420BufferInterface> i420Buffer = frameBuffer->ToI420();
    if (!i420Buffer) [[unlikely]]
        return nullptr;

    auto isTightlyPacked = [](const webrtc::I420BufferInterface& buffer) {
        auto chromaHeight = static_cast<ptrdiff_t>((buffer.height() + 1) / 2);
        auto offsetU = buffer.DataU() - buffer.DataY();
        auto offsetV = buffer.DataV() - buffer.DataY();
        return offsetU == static_cast<ptrdiff_t>(buffer.StrideY()) * buffer.height() && offsetV == offsetU + static_cast<ptrdiff_t>(buffer.StrideU()) * chromaHeight;
    };
    if (!isTightlyPacked(*i420Buffer))
        i420Buffer = webrtc::I420Buffer::Copy(*i420Buffer);

    int width = i420Buffer->width();
    int height = i420Buffer->height();
    int chromaHeight = (height + 1) / 2;
    gint strides[3] = { i420Buffer->StrideY(), i420Buffer->StrideU(), i420Buffer->StrideV() };
    gsize offsets[3] = {
        0,
        static_cast<gsize>(i420Buffer->DataU() - i420Buffer->DataY()),
        static_cast<gsize>(i420Buffer->DataV() - i420Buffer->DataY())
    };
    gsize size = offsets[2] + static_cast<gsize>(strides[2]) * chromaHeight;

    GstVideoInfo info;
    gst_video_info_set_format(&info, GST_VIDEO_FORMAT_I420, width, height);
    if (auto colorSpace = colorSpaceFromLibWebRTCVideoFrame(frame))
        fillVideoInfoColorimetryFromColorSpace(&info, *colorSpace);

    auto* data = const_cast<uint8_t*>(i420Buffer->DataY());
    auto* holder = i420Buffer.release();
    GRefPtr buffer = adoptGRef(gst_buffer_new_wrapped_full(static_cast<GstMemoryFlags>(GST_MEMORY_FLAG_NO_SHARE | GST_MEMORY_FLAG_READONLY), data, size, 0, size, holder, [](gpointer holder) {
        static_cast<webrtc::I420BufferInterface*>(holder)->Release();
    }));
    gst_buffer_add_video_meta_full(buffer.get(), GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_I420, width, height, 3, offsets, strides);
    GST_BUFFER_PTS(buffer.get()) = toGstClockTime(WTF::MediaTime(frame.render_time_ms(), G_USEC_PER_SEC * 1000));

    GRefPtr caps = adoptGRef(gst_video_info_to_caps(&info));
    return adoptGRef(gst_sample_new(buffer.get(), caps.get(), nullptr, nullptr));
}

std::optional<webrtc::VideoFrame> convertGStreamerSampleToLibWebRTCVideoFrame(GRefPtr<GstSample>&& sample, uint32_t rtpTimestamp)
{
    auto buffer = gst_sample_get_buffer(sample.get());
    if (!buffer) [[unlikely]]
        return std::nullopt;

    auto pts = GST_BUFFER_PTS(buffer);
    auto frameBuffer = GStreamerVideoFrameLibWebRTC::create(WTF::move(sample));
    if (!frameBuffer)
        return std::nullopt;

    webrtc::VideoFrame::Builder builder;
    builder.set_video_frame_buffer(frameBuffer).set_rtp_timestamp(rtpTimestamp);
    if (GST_CLOCK_TIME_IS_VALID(pts))
        builder.set_timestamp_us(GST_TIME_AS_USECONDS(pts));
    return builder.build();
}

GStreamerVideoFrameLibWebRTC::GStreamerVideoFrameLibWebRTC(GRefPtr<GstSample>&& sample, MemoryType memoryType, IntSize sourceSize, FloatRect visibleRect, IntSize scaledSize)
    : m_sample(WTF::move(sample))
    , m_memoryType(memoryType)
    , m_sourceSize(sourceSize)
    , m_visibleRect(visibleRect)
    , m_scaledSize(scaledSize)
{
}

webrtc::scoped_refptr<webrtc::VideoFrameBuffer> GStreamerVideoFrameLibWebRTC::create(GRefPtr<GstSample>&& sample)
{
    ensureDebugCategoryIsRegistered();
    auto* caps = gst_sample_get_caps(sample.get());
    auto size = sampleSize(caps);
    if (!size) {
        GST_WARNING("Unable to wrap a sample with caps %" GST_PTR_FORMAT, caps);
        return nullptr;
    }
    return webrtc::scoped_refptr<webrtc::VideoFrameBuffer>(new GStreamerVideoFrameLibWebRTC(WTF::move(sample), memoryTypeFromCaps(caps), *size, FloatRect { { }, *size }, *size));
}

webrtc::scoped_refptr<webrtc::VideoFrameBuffer> GStreamerVideoFrameLibWebRTC::createScaled(GRefPtr<GstSample>&& sample, IntSize scaledSize)
{
    ensureDebugCategoryIsRegistered();
    auto* caps = gst_sample_get_caps(sample.get());
    auto size = sampleSize(caps);
    if (!size || scaledSize.isEmpty()) {
        GST_WARNING("Unable to wrap a sample with caps %" GST_PTR_FORMAT " at %dx%d", caps, scaledSize.width(), scaledSize.height());
        return nullptr;
    }
    return webrtc::scoped_refptr<webrtc::VideoFrameBuffer>(new GStreamerVideoFrameLibWebRTC(WTF::move(sample), memoryTypeFromCaps(caps), *size, FloatRect { { }, *size }, scaledSize));
}

bool GStreamerVideoFrameLibWebRTC::isCropped() const
{
    return m_visibleRect != FloatRect { { }, m_sourceSize };
}

bool GStreamerVideoFrameLibWebRTC::isScaled() const
{
    return m_scaledSize != m_sourceSize;
}

webrtc::scoped_refptr<webrtc::VideoFrameBuffer> GStreamerVideoFrameLibWebRTC::CropAndScale(int offsetX, int offsetY, int cropWidth, int cropHeight, int scaledWidth, int scaledHeight)
{
    if ((m_memoryType != MemoryType::DMABuf && m_memoryType != MemoryType::GL) || cropWidth <= 0 || cropHeight <= 0 || scaledWidth <= 0 || scaledHeight <= 0)
        return webrtc::VideoFrameBuffer::CropAndScale(offsetX, offsetY, cropWidth, cropHeight, scaledWidth, scaledHeight);

    float scaleX = m_visibleRect.width() / m_scaledSize.width();
    float scaleY = m_visibleRect.height() / m_scaledSize.height();
    FloatRect visibleRect { m_visibleRect.x() + offsetX * scaleX, m_visibleRect.y() + offsetY * scaleY, cropWidth * scaleX, cropHeight * scaleY };
    return webrtc::scoped_refptr<webrtc::VideoFrameBuffer>(new GStreamerVideoFrameLibWebRTC(GRefPtr(m_sample), m_memoryType, m_sourceSize, visibleRect, { scaledWidth, scaledHeight }));
}

webrtc::scoped_refptr<webrtc::I420BufferInterface> GStreamerVideoFrameLibWebRTC::copyToI420()
{
    auto* caps = gst_sample_get_caps(m_sample.get());
    IntSize convertedSize = m_scaledSize;
    IntPoint offset;
    if (isCropped()) {
        float scaleX = m_scaledSize.width() / m_visibleRect.width();
        float scaleY = m_scaledSize.height() / m_visibleRect.height();
        if (scaleX > 2 || scaleY > 2) {
            auto sourceBuffer = webrtc::scoped_refptr<webrtc::VideoFrameBuffer>(new GStreamerVideoFrameLibWebRTC(GRefPtr(m_sample), m_memoryType, m_sourceSize, FloatRect { { }, m_sourceSize }, m_sourceSize));
            auto source = sourceBuffer->ToI420();
            if (!source)
                return nullptr;
            auto scaled = webrtc::I420Buffer::Create(m_scaledSize.width(), m_scaledSize.height());
            scaled->CropAndScaleFrom(*source, std::lround(m_visibleRect.x()), std::lround(m_visibleRect.y()), std::lround(m_visibleRect.width()), std::lround(m_visibleRect.height()));
            return scaled;
        }
        convertedSize = { std::max<int>(m_scaledSize.width(), std::lround(m_sourceSize.width() * scaleX)), std::max<int>(m_scaledSize.height(), std::lround(m_sourceSize.height() * scaleY)) };
        int x = std::lround(m_visibleRect.x() * scaleX) & ~1;
        int y = std::lround(m_visibleRect.y() * scaleY) & ~1;
        offset = { std::clamp(x, 0, (convertedSize.width() - m_scaledSize.width()) & ~1), std::clamp(y, 0, (convertedSize.height() - m_scaledSize.height()) & ~1) };
    }

    GRefPtr<GstSample> i420Sample;
    auto* structure = gst_caps_get_structure(caps, 0);
    if (m_memoryType == MemoryType::System && convertedSize == m_sourceSize && !g_strcmp0(gst_structure_get_string(structure, "format"), "I420"))
        i420Sample = m_sample;
    else {
        GRefPtr outputCaps = adoptGRef(gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "I420", "width", G_TYPE_INT, convertedSize.width(), "height", G_TYPE_INT, convertedSize.height(), nullptr));
        if (isRGBSample(caps))
            gst_caps_set_simple(outputCaps.get(), "colorimetry", G_TYPE_STRING, "bt709", nullptr);
        GST_TRACE("Converting a %dx%d frame to %dx%d I420", m_sourceSize.width(), m_sourceSize.height(), convertedSize.width(), convertedSize.height());
        i420Sample = GStreamerVideoFrameConverter::singleton().convert(m_sample, outputCaps);
    }
    if (!i420Sample) [[unlikely]]
        return nullptr;
    return wrapI420Sample(WTF::move(i420Sample), offset, m_scaledSize);
}

webrtc::scoped_refptr<webrtc::I420BufferInterface> GStreamerVideoFrameLibWebRTC::ToI420()
{
    ensureDebugCategoryIsRegistered();
    Locker locker { m_i420Lock };
    if (!m_i420Buffer)
        m_i420Buffer = copyToI420();
    if (m_i420Buffer)
        return m_i420Buffer;

    GST_WARNING("Unable to convert a %dx%d frame to I420, substituting a black frame", width(), height());
    auto blackBuffer = webrtc::I420Buffer::Create(std::max(width(), 2), std::max(height(), 2));
    webrtc::I420Buffer::SetBlack(blackBuffer.get());
    return blackBuffer;
}

} // namespace WebCore
'''


def video_frame_patch(text):
    text = replace_once(text, "#include <wtf/MediaTime.h>\n", "#include <algorithm>\n#include <cmath>\n#include <wtf/MediaTime.h>\n")
    text = replace_once(text, '#include "LibWebRTCVideoFrameUtilities.h"\n', '#include "LibWebRTCVideoFrameUtilities.h"\n#include "VideoFrameGStreamer.h"\n')
    return replace_span(
        text,
        "GRefPtr<GstSample> convertLibWebRTCVideoFrameToGStreamerSample(const webrtc::VideoFrame& frame)\n",
        "} // namespace WebCore\n",
        VIDEO_FRAME_IMPLEMENTATION,
    )


def video_frame_header_patch(text):
    text = replace_once(text, '#include "webrtc/rtc_base/ref_counted_object.h"\n', '#include "webrtc/rtc_base/ref_counted_object.h"\n#include "FloatRect.h"\n#include "IntSize.h"\n#include <optional>\n#include <wtf/Lock.h>\n')
    text = replace_once(
        text,
        "[[nodiscard]] GRefPtr<GstSample> convertLibWebRTCVideoFrameToGStreamerSample(const webrtc::VideoFrame&);\n",
        "[[nodiscard]] GRefPtr<GstSample> convertLibWebRTCVideoFrameToGStreamerSample(const webrtc::VideoFrame&, bool acceptsDMABuf = false);\n",
    )
    text = replace_once(
        text,
        "webrtc::VideoFrame convertGStreamerSampleToLibWebRTCVideoFrame(GRefPtr<GstSample>&&, uint32_t rtpTimestamp);\n",
        "std::optional<webrtc::VideoFrame> convertGStreamerSampleToLibWebRTCVideoFrame(GRefPtr<GstSample>&&, uint32_t rtpTimestamp);\n",
    )
    return replace_span(
        text,
        "class GStreamerVideoFrameLibWebRTC : public webrtc::RefCountedObject<webrtc::VideoFrameBuffer> {\n",
        "    GstVideoInfo m_info;\n};\n",
        VIDEO_FRAME_CLASS,
    )


VIDEO_FRAME_CLASS = r"""class GStreamerVideoFrameLibWebRTC : public webrtc::RefCountedObject<webrtc::VideoFrameBuffer> {
public:
    enum class MemoryType : uint8_t { System, DMABuf, GL, Other };

    static webrtc::scoped_refptr<webrtc::VideoFrameBuffer> create(GRefPtr<GstSample>&&);
    static webrtc::scoped_refptr<webrtc::VideoFrameBuffer> createScaled(GRefPtr<GstSample>&&, IntSize);

    const GRefPtr<GstSample>& sample() const LIFETIME_BOUND { return m_sample; }
    MemoryType memoryType() const { return m_memoryType; }
    bool isCropped() const;
    bool isScaled() const;

    webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() final;
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> CropAndScale(int offsetX, int offsetY, int cropWidth, int cropHeight, int scaledWidth, int scaledHeight) final;
    int width() const final { return m_scaledSize.width(); }
    int height() const final { return m_scaledSize.height(); }

private:
    GStreamerVideoFrameLibWebRTC(GRefPtr<GstSample>&&, MemoryType, IntSize sourceSize, FloatRect visibleRect, IntSize scaledSize);
    webrtc::VideoFrameBuffer::Type type() const final { return Type::kNative; }
    webrtc::scoped_refptr<webrtc::I420BufferInterface> copyToI420();

    GRefPtr<GstSample> m_sample;
    MemoryType m_memoryType;
    IntSize m_sourceSize;
    FloatRect m_visibleRect;
    IntSize m_scaledSize;
    Lock m_i420Lock;
    webrtc::scoped_refptr<webrtc::I420BufferInterface> m_i420Buffer WTF_GUARDED_BY_LOCK(m_i420Lock);
};
"""


BLACK_FRAME = r'''webrtc::scoped_refptr<webrtc::VideoFrameBuffer> RealtimeOutgoingVideoSourceLibWebRTC::createBlackFrame(size_t width, size_t height)
{
    GST_TRACE("Creating black video frame");
    auto buffer = webrtc::I420Buffer::Create(std::max<int>(width, 2), std::max<int>(height, 2));
    webrtc::I420Buffer::SetBlack(buffer.get());
    return buffer;
}

'''


def outgoing_video_patch(text):
    text = replace_once(
        text,
        "    frameSize.scale(this->videoFrameScaling());\n",
        "    frameSize.scale(this->videoFrameScaling());\n"
        "    if (gstFrame.memoryType() != VideoFrameGStreamer::MemoryType::System && gstFrame.memoryType() != VideoFrameGStreamer::MemoryType::Unsupported) {\n"
        "        GRefPtr nativeSample = adoptGRef(gst_sample_make_writable(GRefPtr(gstFrame.sample()).leakRef()));\n"
        "        gst_sample_set_segment(nativeSample.get(), nullptr);\n"
        "        if (auto buffer = GStreamerVideoFrameLibWebRTC::createScaled(WTF::move(nativeSample), frameSize)) {\n"
        "            GST_TRACE(\"Sending a GPU-backed %dx%d video frame\", frameSize.width(), frameSize.height());\n"
        "            sendFrame(WTF::move(buffer), colorSpace);\n"
        "            return;\n"
        "        }\n"
        "    }\n",
    )
    return replace_span(
        text,
        "webrtc::scoped_refptr<webrtc::VideoFrameBuffer> RealtimeOutgoingVideoSourceLibWebRTC::createBlackFrame(size_t width, size_t height)\n",
        "    return GStreamerVideoFrameLibWebRTC::create(gst_sample_new(buffer.get(), caps.get(), nullptr, nullptr));\n}\n\n",
        BLACK_FRAME,
    )


def incoming_video_patch(text):
    return replace_once(
        text,
        "    auto sample = convertLibWebRTCVideoFrameToGStreamerSample(frame);\n",
        "    auto sample = convertLibWebRTCVideoFrameToGStreamerSample(frame, true);\n"
        "    if (!sample) [[unlikely]] {\n"
        '        GST_WARNING("Dropping an incoming %dx%d frame that could not be wrapped", frame.width(), frame.height());\n'
        "        return;\n"
        "    }\n",
    )


def audio_module_header_patch(text):
    text = replace_once(text, "#include <wtf/CheckedPtr.h>\n", "#include <atomic>\n#include <wtf/CheckedPtr.h>\n#include <wtf/Lock.h>\n")
    text = replace_once(text, "    void pollFromSource();\n", "    void pollFromSource() WTF_REQUIRES_LOCK(m_audioTransportLock);\n")
    return replace_once(
        text,
        "    bool m_isPlaying { false };\n    webrtc::AudioTransport* m_audioTransport { nullptr };\n",
        "    std::atomic<bool> m_isPlaying { false };\n"
        "    Lock m_audioTransportLock;\n"
        "    webrtc::AudioTransport* m_audioTransport WTF_GUARDED_BY_LOCK(m_audioTransportLock) { nullptr };\n",
    )


def audio_module_patch(text):
    text = replace_once(
        text,
        '    RELEASE_LOG(WebRTC, "LibWebRTCAudioModule::RegisterAudioCallback %d", !!audioTransport);\n'
        "\n"
        "    m_audioTransport = audioTransport;\n",
        '    RELEASE_LOG(WebRTC, "LibWebRTCAudioModule::RegisterAudioCallback %d", !!audioTransport);\n'
        "\n"
        "    Locker locker { m_audioTransportLock };\n"
        "    m_audioTransport = audioTransport;\n",
    )
    text = replace_once(
        text,
        '    RELEASE_LOG(WebRTC, "LibWebRTCAudioModule::StartPlayout %d", m_isPlaying);\n'
        "\n"
        "    if (m_isPlaying)\n"
        "        return 0;\n"
        "\n"
        "    m_isPlaying = true;\n",
        '    RELEASE_LOG(WebRTC, "LibWebRTCAudioModule::StartPlayout %d", m_isPlaying.load());\n'
        "    if (m_isPlaying.exchange(true))\n"
        "        return 0;\n",
    )
    text = replace_once(
        text,
        '    RELEASE_LOG(WebRTC, "LibWebRTCAudioModule::StopPlayout %d", m_isPlaying);\n'
        "\n"
        "    m_isPlaying = false;\n",
        '    RELEASE_LOG(WebRTC, "LibWebRTCAudioModule::StopPlayout %d", m_isPlaying.load());\n'
        "\n"
        "    m_isPlaying = false;\n"
        "    if (!m_queue->isCurrent())\n"
        "        m_queue->dispatchSync([] { });\n",
    )
    text = replace_once(
        text,
        "    {\n"
        "        // For performance reasons, we forbid heap allocations while doing rendering on the webrtc audio thread.\n"
        "        ForbidMallocUseForCurrentThreadScope forbidMallocUse;\n"
        "\n"
        "        pollFromSource();\n"
        "    }\n",
        "    {\n"
        "        Locker locker { m_audioTransportLock };\n"
        "        ForbidMallocUseForCurrentThreadScope forbidMallocUse;\n"
        "        pollFromSource();\n"
        "    }\n",
    )
    return text


def video_encoder_header_patch(text):
    return replace_once(
        text,
        "    bool encodeSync(RawFrame&&, bool shouldGenerateKeyFrame);\n",
        "    bool encodeSync(RawFrame&&, bool shouldGenerateKeyFrame);\n"
        "    void setRatesSync(uint64_t bitRate, double frameRate);\n"
        "    void setKeyFrameIntervalSync(unsigned);\n"
        "    bool isHardwareAccelerated() const;\n",
    )


def video_encoder_patch(text):
    text = replace_once(
        text,
        "    void setRates(uint64_t bitRate, double frameRate);\n",
        "    void setRates(uint64_t bitRate, double frameRate);\n"
        "    void setKeyFrameInterval(unsigned);\n"
        "    bool isHardwareAccelerated() const;\n",
    )
    text = replace_once(
        text,
        "bool GStreamerVideoEncoder::encodeSync(RawFrame&& frame, bool shouldGenerateKeyFrame)\n",
        "void GStreamerVideoEncoder::setRatesSync(uint64_t bitRate, double frameRate)\n"
        "{\n"
        "    m_internalEncoder->setRates(bitRate, frameRate);\n"
        "}\n"
        "\n"
        "void GStreamerVideoEncoder::setKeyFrameIntervalSync(unsigned interval)\n"
        "{\n"
        "    m_internalEncoder->setKeyFrameInterval(interval);\n"
        "}\n"
        "\n"
        "bool GStreamerVideoEncoder::isHardwareAccelerated() const\n"
        "{\n"
        "    return m_internalEncoder->isHardwareAccelerated();\n"
        "}\n"
        "\n"
        "bool GStreamerVideoEncoder::encodeSync(RawFrame&& frame, bool shouldGenerateKeyFrame)\n",
    )
    return replace_once(
        text,
        "void GStreamerInternalVideoEncoder::flush()\n",
        "void GStreamerInternalVideoEncoder::setKeyFrameInterval(unsigned interval)\n"
        "{\n"
        '    g_object_set(m_harness->element(), "keyframe-interval", interval, nullptr);\n'
        "}\n"
        "\n"
        "bool GStreamerInternalVideoEncoder::isHardwareAccelerated() const\n"
        "{\n"
        "    GRefPtr<GstElement> encoder;\n"
        '    g_object_get(m_harness->element(), "encoder", &encoder.outPtr(), nullptr);\n'
        "    auto* factory = encoder ? gst_element_get_factory(encoder.get()) : nullptr;\n"
        "    auto* klass = factory ? gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS) : nullptr;\n"
        '    return klass && g_strstr_len(klass, -1, "Hardware");\n'
        "}\n"
        "\n"
        "void GStreamerInternalVideoEncoder::flush()\n",
    )


def gstreamer_checks_patch(text):
    return replace_once(
        text,
        "    else ()\n        SET_AND_EXPOSE_TO_BUILD(USE_LIBWEBRTC TRUE)\n    endif ()\n",
        "    else ()\n"
        "        SET_AND_EXPOSE_TO_BUILD(USE_LIBWEBRTC TRUE)\n"
        "        find_package(PkgConfig REQUIRED)\n"
        "        pkg_check_modules(LIBWEBRTC_AOM REQUIRED IMPORTED_TARGET GLOBAL aom>=3.8.0)\n"
        "        pkg_check_modules(LIBWEBRTC_DAV1D REQUIRED IMPORTED_TARGET GLOBAL dav1d>=1.2.0)\n"
        "        pkg_get_variable(LIBWEBRTC_AOM_INCLUDEDIR aom includedir)\n"
        "        pkg_get_variable(LIBWEBRTC_DAV1D_INCLUDEDIR dav1d includedir)\n"
        "        SET_AND_EXPOSE_TO_BUILD(USE_LIBWEBRTC_AV1 TRUE)\n"
        "    endif ()\n",
    )


LIBWEBRTC_AV1_BUILD = r'''
if (USE_LIBWEBRTC_AV1 AND NOT APPLE)
    set(_libwebrtc_av1_forward "${CMAKE_CURRENT_BINARY_DIR}/eden-av1-forward")
    file(MAKE_DIRECTORY "${_libwebrtc_av1_forward}/third_party/dav1d/libdav1d" "${_libwebrtc_av1_forward}/third_party/libaom/source")
    foreach (_libwebrtc_av1_link IN ITEMS "third_party/dav1d/libdav1d/include" "third_party/libaom/source/libaom")
        if (IS_SYMLINK "${_libwebrtc_av1_forward}/${_libwebrtc_av1_link}")
            file(REMOVE "${_libwebrtc_av1_forward}/${_libwebrtc_av1_link}")
        endif ()
    endforeach ()
    file(CREATE_LINK "${LIBWEBRTC_DAV1D_INCLUDEDIR}" "${_libwebrtc_av1_forward}/third_party/dav1d/libdav1d/include" SYMBOLIC)
    file(CREATE_LINK "${LIBWEBRTC_AOM_INCLUDEDIR}" "${_libwebrtc_av1_forward}/third_party/libaom/source/libaom" SYMBOLIC)
    target_sources(webrtc PRIVATE
        Source/webrtc/modules/video_coding/codecs/av1/dav1d_decoder.cc
        Source/webrtc/modules/video_coding/codecs/av1/libaom_av1_encoder.cc
        Source/webrtc/modules/video_coding/codecs/av1/libaom_speed_config_factory.cc
    )
    target_include_directories(webrtc BEFORE PRIVATE "${_libwebrtc_av1_forward}")
    target_compile_definitions(webrtc PRIVATE RTC_USE_LIBAOM_AV1_ENCODER RTC_DAV1D_IN_INTERNAL_DECODER_FACTORY)
    target_link_libraries(webrtc PkgConfig::LIBWEBRTC_AOM PkgConfig::LIBWEBRTC_DAV1D)
endif ()
'''


def libwebrtc_cmake_patch(text):
    text = replace_once(
        text,
        "    Source/webrtc/modules/audio_processing/capture_mixer/audio_content_analyzer.cc\n",
        "    Source/webrtc/modules/audio_processing/capture_mixer/audio_content_analyzer.cc\n"
        "    Source/webrtc/modules/audio_processing/capture_mixer/capture_mixer.cc\n",
    )
    return replace_once(
        text,
        "target_include_directories(webrtc ${webrtc_INCLUDE_DIRECTORIES})\n",
        "target_include_directories(webrtc ${webrtc_INCLUDE_DIRECTORIES})\n" + LIBWEBRTC_AV1_BUILD,
    )


ENCODING_PARAMETERS_IDL = "Source/WebCore/Modules/mediastream/RTCRtpEncodingParameters.idl"
ENCODING_PARAMETERS_HEADER = "Source/WebCore/Modules/mediastream/RTCRtpEncodingParameters.h"
LIBWEBRTC_UTILITIES = "Source/WebCore/Modules/mediastream/libwebrtc/LibWebRTCUtils.cpp"


def encoding_parameters_idl_patch(text):
    return replace_once(
        text,
        "    double scaleResolutionDownBy;\n",
        "    double scaleResolutionDownBy;\n    DOMString scalabilityMode;\n",
    )


def encoding_parameters_header_patch(text):
    return replace_once(
        text,
        "    std::optional<double> scaleResolutionDownBy;\n",
        "    std::optional<double> scaleResolutionDownBy;\n    String scalabilityMode;\n",
    )


def libwebrtc_utilities_patch(text):
    text = replace_once(
        text,
        "    if (rtcParameters.scale_resolution_down_by && !isAudio)\n"
        "        parameters.scaleResolutionDownBy = *rtcParameters.scale_resolution_down_by;\n",
        "    if (rtcParameters.scale_resolution_down_by && !isAudio)\n"
        "        parameters.scaleResolutionDownBy = *rtcParameters.scale_resolution_down_by;\n"
        "    if (rtcParameters.scalability_mode && !isAudio)\n"
        "        parameters.scalabilityMode = fromStdString(*rtcParameters.scalability_mode);\n",
    )
    return replace_once(
        text,
        "    if (parameters.scaleResolutionDownBy)\n"
        "        rtcParameters.scale_resolution_down_by = parameters.scaleResolutionDownBy;\n",
        "    if (parameters.scaleResolutionDownBy)\n"
        "        rtcParameters.scale_resolution_down_by = parameters.scaleResolutionDownBy;\n"
        "    if (!parameters.scalabilityMode.isNull())\n"
        "        rtcParameters.scalability_mode = parameters.scalabilityMode.utf8().toStdString();\n",
    )


WEBRTC_PROVIDER = "Source/WebCore/platform/mediastream/libwebrtc/LibWebRTCProvider.cpp"


DEBUG_LOGGING = r"""static std::optional<webrtc::LoggingSeverity> debugLoggingSeverity()
{
    auto value = StringView::fromLatin1(getenv("EDEN_WEBRTC_LOG"));
    if (equalLettersIgnoringASCIICase(value, "verbose"_s))
        return webrtc::LS_VERBOSE;
    if (equalLettersIgnoringASCIICase(value, "info"_s))
        return webrtc::LS_INFO;
    if (equalLettersIgnoringASCIICase(value, "warning"_s))
        return webrtc::LS_WARNING;
    if (equalLettersIgnoringASCIICase(value, "error"_s))
        return webrtc::LS_ERROR;
    return std::nullopt;
}

static void initializePeerConnectionFactoryAndThreads(PeerConnectionFactoryAndThreads& factoryAndThreads)
{
    ASSERT(!factoryAndThreads.networkThread);

    if (auto severity = debugLoggingSeverity()) {
        webrtc::LoggingConfig config;
        config.set_min_severity(*severity);
        config.set_debug_severity(*severity);
        config.set_log_thread(true);
        config.set_log_timestamp(true);
        config.set_log_to_stderr(true);
        if (!webrtc::InitializeLogging(WTF::move(config)))
            WTFLogAlways("EDEN_WEBRTC_LOG is ignored because libwebrtc logging was already configured");
    }
"""


def webrtc_provider_patch(text):
    text = replace_once(
        text,
        "#include <webrtc/rtc_base/physical_socket_server.h>\n",
        "#include <webrtc/rtc_base/logging.h>\n#include <webrtc/rtc_base/physical_socket_server.h>\n",
    )
    return replace_once(
        text,
        "static void initializePeerConnectionFactoryAndThreads(PeerConnectionFactoryAndThreads& factoryAndThreads)\n"
        "{\n"
        "    ASSERT(!factoryAndThreads.networkThread);\n",
        DEBUG_LOGGING,
    )


VIDEO_COMMON = GLUE + "GStreamerVideoCommon.cpp"
H264_DECODER_PROFILES = ("Baseline", "ConstrainedBaseline", "Main", "ConstrainedHigh", "High", "PredictiveHigh444")


def video_common_patch(text):
    for profile in H264_DECODER_PROFILES:
        for mode in ("1", "0"):
            text = replace_once(
                text,
                f'        createH264Format(webrtc::H264Profile::kProfile{profile}, webrtc::H264Level::kLevel3_1, "{mode}"),\n',
                "",
            )
    return text


PATCHES = {
    OUTGOING_AUDIO_HEADER: outgoing_audio_header_patch,
    OUTGOING_AUDIO: outgoing_audio_patch,
    ENCODER_FACTORY_HEADER: encoder_factory_header_patch,
    ENCODER_FACTORY: encoder_factory_patch,
    DECODER_FACTORY_HEADER: decoder_factory_header_patch,
    DECODER_FACTORY: decoder_factory_patch,
    PROVIDER: provider_patch,
    VIDEO_FRAME_HEADER: video_frame_header_patch,
    VIDEO_FRAME: video_frame_patch,
    OUTGOING_VIDEO: outgoing_video_patch,
    INCOMING_VIDEO: incoming_video_patch,
    AUDIO_MODULE_HEADER: audio_module_header_patch,
    AUDIO_MODULE: audio_module_patch,
    VIDEO_ENCODER_HEADER: video_encoder_header_patch,
    VIDEO_ENCODER: video_encoder_patch,
    GSTREAMER_CHECKS: gstreamer_checks_patch,
    ENCODING_PARAMETERS_IDL: encoding_parameters_idl_patch,
    ENCODING_PARAMETERS_HEADER: encoding_parameters_header_patch,
    LIBWEBRTC_UTILITIES: libwebrtc_utilities_patch,
    WEBRTC_PROVIDER: webrtc_provider_patch,
    VIDEO_COMMON: video_common_patch,
}

SUBTREE_PATCHES = {
    LIBWEBRTC_CMAKE: libwebrtc_cmake_patch,
}

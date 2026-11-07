from wpe.patch_support import replace_once


MEDIA = "Source/WebCore/Modules/mediastream/gstreamer/"
PLATFORM = "Source/WebCore/platform/mediastream/gstreamer/"


def endpoint_patch(text):
    text = replace_once(
        text,
        '    gst_structure_set_value(initData.get(), "encodings", &encodingsValue);',
        '''    std::optional<uint32_t> sourceSSRC;
    GValue reservedEncodings = G_VALUE_INIT;
    g_value_init(&reservedEncodings, GST_TYPE_LIST);
    auto releaseReservedEncodings = makeScopeExit([&] {
        g_value_unset(&reservedEncodings);
    });
    unsigned encodingCount = gst_value_list_get_size(&encodingsValue);
    for (unsigned i = 0; i < encodingCount; ++i) {
        auto ssrc = m_ssrcGenerator->generateSSRC();
        if (!ssrc || ssrc == std::numeric_limits<uint32_t>::max())
            return Exception { ExceptionCode::OperationError, "Unable to reserve an RTP source identifier"_s };
        const auto* encoding = gst_value_get_structure(gst_value_list_get_value(&encodingsValue, i));
        GUniquePtr<GstStructure> reservedEncoding(gst_structure_copy(encoding));
        gst_structure_set(reservedEncoding.get(), "reserved-ssrc", G_TYPE_UINT, ssrc, nullptr);
        GValue value = G_VALUE_INIT;
        g_value_init(&value, GST_TYPE_STRUCTURE);
        gst_value_set_structure(&value, reservedEncoding.get());
        gst_value_list_append_and_take_value(&reservedEncodings, &value);
        if (encodingCount == 1)
            sourceSSRC = ssrc;
    }
    gst_structure_set_value(initData.get(), "encodings", &reservedEncodings);''',
    )
    return replace_once(
        text,
        '    auto transceiver = makeUniqueRef<GStreamerRtpTransceiverBackend>(WTF::move(rtcTransceiver));',
        '    auto transceiver = makeUniqueRef<GStreamerRtpTransceiverBackend>(WTF::move(rtcTransceiver), sourceSSRC);',
    )


def transceiver_header_patch(text):
    text = replace_once(
        text,
        '    GStreamerRtpTransceiverBackend(GRefPtr<GstWebRTCRTPTransceiver>&&);',
        '    GStreamerRtpTransceiverBackend(GRefPtr<GstWebRTCRTPTransceiver>&&, std::optional<uint32_t> sourceSSRC = std::nullopt);',
    )
    return replace_once(
        text,
        'private:\n    GRefPtr<GstWebRTCRTPTransceiver> m_rtcTransceiver;',
        '''private:
    void applySourceSSRC(GstCaps*, RTCRtpTransceiverDirection);
    GRefPtr<GstWebRTCRTPTransceiver> m_rtcTransceiver;
    const std::optional<uint32_t> m_sourceSSRC;
    bool m_hasSendingSSRC { false };''',
    )


def transceiver_patch(text):
    text = replace_once(
        text,
        '''GStreamerRtpTransceiverBackend::GStreamerRtpTransceiverBackend(GRefPtr<GstWebRTCRTPTransceiver>&& rtcTransceiver)
    : m_rtcTransceiver(WTF::move(rtcTransceiver))''',
        '''GStreamerRtpTransceiverBackend::GStreamerRtpTransceiverBackend(GRefPtr<GstWebRTCRTPTransceiver>&& rtcTransceiver, std::optional<uint32_t> sourceSSRC)
    : m_rtcTransceiver(WTF::move(rtcTransceiver))
    , m_sourceSSRC(sourceSSRC)''',
    )
    text = replace_once(
        text,
        '''    GstWebRTCKind kind;
    g_object_get(m_rtcTransceiver.get(), "kind", &kind, nullptr);''',
        '''    if (m_sourceSSRC) {
        GRefPtr<GstCaps> caps;
        g_object_get(m_rtcTransceiver.get(), "codec-preferences", &caps.outPtr(), nullptr);
        if (caps) {
            caps = adoptGRef(gst_caps_make_writable(caps.leakRef()));
            applySourceSSRC(caps.get(), direction());
            g_object_set(m_rtcTransceiver.get(), "codec-preferences", caps.get(), nullptr);
        }
    }

    GstWebRTCKind kind;
    g_object_get(m_rtcTransceiver.get(), "kind", &kind, nullptr);''',
    )
    text = replace_once(
        text,
        'void GStreamerRtpTransceiverBackend::setDirection(RTCRtpTransceiverDirection direction)\n{',
        '''void GStreamerRtpTransceiverBackend::applySourceSSRC(GstCaps* caps, RTCRtpTransceiverDirection direction)
{
    if (!m_sourceSSRC || gst_caps_is_empty(caps) || gst_caps_is_any(caps))
        return;
    bool sends = direction == RTCRtpTransceiverDirection::Sendonly || direction == RTCRtpTransceiverDirection::Sendrecv;
    m_hasSendingSSRC |= sends;
    for (unsigned i = 0; i < gst_caps_get_size(caps); ++i) {
        auto* structure = gst_caps_get_structure(caps, i);
        if (m_hasSendingSSRC && !i)
            gst_structure_set(structure, "ssrc", G_TYPE_UINT, *m_sourceSSRC, nullptr);
        else
            gst_structure_remove_field(structure, "ssrc");
    }
}

void GStreamerRtpTransceiverBackend::setDirection(RTCRtpTransceiverDirection direction)
{''',
    )
    text = replace_once(
        text,
        '    g_object_set(m_rtcTransceiver.get(), "direction", gstDirection, nullptr);',
        '''    GRefPtr<GstCaps> caps;
    if (m_sourceSSRC)
        g_object_get(m_rtcTransceiver.get(), "codec-preferences", &caps.outPtr(), nullptr);
    if (caps) {
        caps = adoptGRef(gst_caps_make_writable(caps.leakRef()));
        applySourceSSRC(caps.get(), direction);
        g_object_set(m_rtcTransceiver.get(), "direction", gstDirection, "codec-preferences", caps.get(), nullptr);
    } else
        g_object_set(m_rtcTransceiver.get(), "direction", gstDirection, nullptr);''',
    )
    return replace_once(
        text,
        '    g_object_set(m_rtcTransceiver.get(), "codec-preferences", gstCodecs.get(), nullptr);',
        '''    applySourceSSRC(gstCodecs.get(), direction());
    g_object_set(m_rtcTransceiver.get(), "codec-preferences", gstCodecs.get(), nullptr);''',
    )


def source_patch(text):
    text = replace_once(
        text,
        '    m_bin = gst_bin_new(nullptr);',
        '''    m_bin = gst_bin_new(nullptr);
    gst_element_set_locked_state(m_bin.get(), TRUE);''',
    )
    text = replace_once(
        text,
        '    gst_pad_link(srcPad.get(), m_webrtcSinkPad.get());',
        '''    gst_pad_link(srcPad.get(), m_webrtcSinkPad.get());
    gst_element_set_locked_state(m_bin.get(), FALSE);''',
    )
    before = '''    for (const auto& encoding : encodings) {
        GUniquePtr<GstStructure> encodingParameters(gst_structure_copy(encoding));
        auto rid = gstStructureGetString(encodingParameters.get(), "rid"_s);'''
    after = '''    GValue reservedEncodings = G_VALUE_INIT;
    g_value_init(&reservedEncodings, GST_TYPE_LIST);
    auto oldEncodings = m_parameters ? gstStructureGetList<const GstStructure*>(m_parameters.get(), "encodings"_s) : Vector<const GstStructure*> { };
    for (unsigned i = 0; i < encodings.size(); ++i) {
        GUniquePtr<GstStructure> encodingParameters(gst_structure_copy(encodings[i]));
        if (i < oldEncodings.size()) {
            if (auto ssrc = gstStructureGet<unsigned>(oldEncodings[i], "reserved-ssrc"_s))
                gst_structure_set(encodingParameters.get(), "reserved-ssrc", G_TYPE_UINT, *ssrc, nullptr);
        }
        GValue reservedEncoding = G_VALUE_INIT;
        g_value_init(&reservedEncoding, GST_TYPE_STRUCTURE);
        gst_value_set_structure(&reservedEncoding, encodingParameters.get());
        gst_value_list_append_and_take_value(&reservedEncodings, &reservedEncoding);
        auto rid = gstStructureGetString(encodingParameters.get(), "rid"_s);'''
    text = replace_once(text, before, after)
    return replace_once(
        text,
        '''        packetizer->reconfigure(WTF::move(encodingParameters));
    }
    m_parameters = WTF::move(parameters);''',
        '''        packetizer->reconfigure(WTF::move(encodingParameters));
    }
    gst_structure_take_value(parameters.get(), "encodings", &reservedEncodings);
    m_parameters = WTF::move(parameters);''',
    )


def packetizer_patch(text):
    return replace_once(
        text,
        '    auto ssrc = ssrcGenerator->generateSSRC();',
        '''    auto reservedSSRC = encodingParameters ? gstStructureGet<unsigned>(encodingParameters.get(), "reserved-ssrc"_s) : std::nullopt;
    auto ssrc = reservedSSRC ? *reservedSSRC : ssrcGenerator->generateSSRC();''',
    )


PATCHES = {
    MEDIA + "GStreamerMediaEndpoint.cpp": endpoint_patch,
    MEDIA + "GStreamerRtpTransceiverBackend.h": transceiver_header_patch,
    MEDIA + "GStreamerRtpTransceiverBackend.cpp": transceiver_patch,
    PLATFORM + "RealtimeOutgoingMediaSourceGStreamer.cpp": source_patch,
    PLATFORM + "GStreamerAudioRTPPacketizer.cpp": packetizer_patch,
    PLATFORM + "GStreamerVideoRTPPacketizer.cpp": packetizer_patch,
}

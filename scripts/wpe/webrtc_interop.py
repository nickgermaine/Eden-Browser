from wpe.patch_support import replace_once


MEDIA = "Source/WebCore/Modules/mediastream/gstreamer/"
ENDPOINT = MEDIA + "GStreamerMediaEndpoint.cpp"
ENDPOINT_HEADER = MEDIA + "GStreamerMediaEndpoint.h"
PEER_CONNECTION = MEDIA + "GStreamerPeerConnectionBackend.cpp"
PEER_CONNECTION_HEADER = MEDIA + "GStreamerPeerConnectionBackend.h"
SENDER = MEDIA + "GStreamerRtpSenderBackend.cpp"
SENDER_HEADER = MEDIA + "GStreamerRtpSenderBackend.h"
FIND_GSTREAMER = "Source/cmake/FindGStreamer.cmake"
GSTREAMER_CHECKS = "Source/cmake/GStreamerChecks.cmake"
WEBCORE_GSTREAMER = "Source/WebCore/platform/GStreamer.cmake"
TRANSCEIVER = MEDIA + "GStreamerRtpTransceiverBackend.cpp"
OUTGOING_SOURCE = "Source/WebCore/platform/mediastream/gstreamer/RealtimeOutgoingMediaSourceGStreamer.cpp"

SENDER_SOURCE = r'''static GRefPtr<GstPad> unlinkedSinkPadForTransceiver(GstElement* webrtcBin, GstWebRTCRTPTransceiver* transceiver)
{
    for (auto pad : GstIteratorAdaptor<GstPad>(gst_element_iterate_sink_pads(webrtcBin))) {
        GRefPtr<GstWebRTCRTPTransceiver> padTransceiver;
        g_object_get(pad, "transceiver", &padTransceiver.outPtr(), nullptr);
        if (padTransceiver.get() == transceiver && !gst_pad_is_linked(pad))
            return pad;
    }
    return nullptr;
}

bool GStreamerMediaEndpoint::attachSenderSource(GStreamerRtpSenderBackend& senderBackend, MediaStreamTrack& track)
{
    if (senderBackend.hasSource() || !senderBackend.rtcSender())
        return false;

    GRefPtr<GstWebRTCRTPTransceiver> transceiver;
    forEachTransceiver(m_webrtcBin, [&](auto&& candidate) -> bool {
        GRefPtr<GstWebRTCRTPSender> sender;
        g_object_get(candidate.get(), "sender", &sender.outPtr(), nullptr);
        if (sender.get() != senderBackend.rtcSender())
            return false;
        transceiver = candidate;
        return true;
    });
    if (!transceiver)
        return false;

    GstWebRTCRTPTransceiverDirection direction;
    GstWebRTCRTPTransceiverDirection currentDirection;
    unsigned mLineIndex;
    g_object_get(transceiver.get(), "direction", &direction, "current-direction", &currentDirection, "mlineindex", &mLineIndex, nullptr);
    if (direction != GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY && direction != GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV)
        return false;

    std::optional<unsigned> boundMLineIndex;
    if (mLineIndex != std::numeric_limits<unsigned>::max())
        boundMLineIndex = mLineIndex;

    auto descriptionCaps = [&](ASCIILiteral property) -> GRefPtr<GstCaps> {
        GUniqueOutPtr<GstWebRTCSessionDescription> description;
        g_object_get(m_webrtcBin.get(), property.characters(), &description.outPtr(), nullptr);
        if (!description || !boundMLineIndex || *boundMLineIndex >= gst_sdp_message_medias_len(description->sdp))
            return nullptr;
        return capsFromSDPMedia(gst_sdp_message_get_media(description->sdp, *boundMLineIndex));
    };
    GRefPtr<GstCaps> allowedCaps;
    if (currentDirection != GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_NONE)
        allowedCaps = descriptionCaps("current-local-description"_s);
    if (!allowedCaps)
        allowedCaps = descriptionCaps("remote-description"_s);
    if (!allowedCaps)
        g_object_get(transceiver.get(), "codec-preferences", &allowedCaps.outPtr(), nullptr);
    if (!allowedCaps || gst_caps_is_empty(allowedCaps.get()))
        return false;

    auto existingSinkPad = unlinkedSinkPadForTransceiver(m_webrtcBin.get(), transceiver.get());

    auto mediaStreamId = senderBackend.mediaStreamId().isEmpty() ? track.mediaStreamId() : senderBackend.mediaStreamId();
    auto attach = [&](RealtimeOutgoingMediaSourceGStreamer& source) -> bool {
        configureSource(source, nullptr);
        source.configure(WTF::move(allowedCaps));
        auto sinkPad = WTF::move(existingSinkPad);
        if (sinkPad && !source.mediaStreamID().isEmpty() && gstObjectHasProperty(sinkPad.get(), "msid"_s))
            g_object_set(sinkPad.get(), "msid", source.mediaStreamID().ascii().data(), nullptr);
        if (!sinkPad)
            sinkPad = requestPad(source.rtpCaps(), source.mediaStreamID(), boundMLineIndex);
        if (!sinkPad) {
            source.teardown();
            return false;
        }
        source.setSinkPad(WTF::move(sinkPad));
        m_unlinkedOutgoingSources.append(&source);
        return true;
    };
    if (track.privateTrack().isAudio()) {
        auto source = RealtimeOutgoingAudioSourceGStreamer::create(m_ssrcGenerator, mediaStreamId, track);
        if (!attach(source.get()))
            return false;
        senderBackend.setSource(WTF::move(source));
    } else {
        auto source = RealtimeOutgoingVideoSourceGStreamer::create(m_ssrcGenerator, mediaStreamId, track);
        if (!attach(source.get()))
            return false;
        senderBackend.setSource(WTF::move(source));
    }
    GST_DEBUG_OBJECT(m_pipeline.get(), "Attached an outgoing source to transceiver %" GST_PTR_FORMAT, transceiver.get());
    return true;
}

void GStreamerMediaEndpoint::linkSenderSourceIfNegotiated(GStreamerRtpSenderBackend& senderBackend)
{
    if (!senderBackend.rtcSender() || fetchSignalingState(m_webrtcBin.get()) != GST_WEBRTC_SIGNALING_STATE_STABLE)
        return;

    bool isSending = false;
    forEachTransceiver(m_webrtcBin, [&](auto&& transceiver) -> bool {
        GRefPtr<GstWebRTCRTPSender> sender;
        GstWebRTCRTPTransceiverDirection currentDirection;
        g_object_get(transceiver.get(), "sender", &sender.outPtr(), "current-direction", &currentDirection, nullptr);
        if (sender.get() != senderBackend.rtcSender())
            return false;
        isSending = currentDirection == GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY || currentDirection == GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV;
        return true;
    });
    if (!isSending)
        return;

    GUniqueOutPtr<GstWebRTCSessionDescription> localDescription;
    g_object_get(m_webrtcBin.get(), "current-local-description", &localDescription.outPtr(), nullptr);
    if (localDescription)
        linkOutgoingSources(localDescription->sdp);
}

void GStreamerMediaEndpoint::attachPendingSenderSources()
{
    if (auto peerConnectionBackend = this->peerConnectionBackend())
        peerConnectionBackend->attachSourcesToSendingSenders();
}

void GStreamerMediaEndpoint::doCreateOffer('''

REMOTE_INTEROP = r'''namespace WebCore {

static constexpr size_t maximumIceCredentialSurrogates = 256;
static constexpr size_t iceCredentialSurrogateLength = 32;
static constexpr size_t minimumIceUsernameFragmentLength = 4;
static constexpr size_t minimumIcePasswordLength = 22;
static constexpr size_t maximumIceCredentialLength = 256;

enum class IceCredentialField : uint8_t { UsernameFragment, Password };

static bool isStrictIceCharacter(char character)
{
    return isASCIIAlphanumeric(character) || character == '+' || character == '/';
}

static bool isTolerableIceCharacter(char character)
{
    return isStrictIceCharacter(character) || character == '-' || character == '=' || character == '#' || character == '_';
}

static bool iceCredentialNeedsSurrogate(IceCredentialField field, std::span<const char> value)
{
    size_t minimumLength = field == IceCredentialField::UsernameFragment ? minimumIceUsernameFragmentLength : minimumIcePasswordLength;
    if (value.size() < minimumLength || value.size() > maximumIceCredentialLength)
        return false;
    bool hasTolerableCharacter = false;
    for (char character : value) {
        if (!isTolerableIceCharacter(character))
            return false;
        if (!isStrictIceCharacter(character))
            hasTolerableCharacter = true;
    }
    return hasTolerableCharacter;
}

static std::optional<IceCredentialField> iceCredentialField(const GstSDPAttribute* attribute)
{
    if (!attribute || !attribute->key || !attribute->value)
        return std::nullopt;
    auto key = unsafeSpan(attribute->key);
    if (equal(key, "ice-ufrag"_s))
        return IceCredentialField::UsernameFragment;
    if (equal(key, "ice-pwd"_s))
        return IceCredentialField::Password;
    return std::nullopt;
}

static bool spansEqual(std::span<const char> first, std::span<const char> second)
{
    return std::ranges::equal(first, second);
}

class IceCredentialSurrogates {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(IceCredentialSurrogates);
    WTF_MAKE_NONCOPYABLE(IceCredentialSurrogates);
public:
    IceCredentialSurrogates() = default;

    CString surrogateFor(std::span<const char> original)
    {
        Locker locker { m_lock };
        for (auto& entry : m_entries) {
            if (spansEqual(entry.original.span(), original))
                return CString(entry.surrogate.span());
        }
        auto surrogate = uniqueSurrogate();
        if (m_entries.size() >= maximumIceCredentialSurrogates)
            m_entries.removeFirst();
        m_entries.append({ CString(surrogate.span()), CString(original) });
        return surrogate;
    }

    std::optional<CString> originalForSurrogate(std::span<const char> value) const
    {
        Locker locker { m_lock };
        for (auto& entry : m_entries) {
            if (spansEqual(entry.surrogate.span(), value))
                return CString(entry.original.span());
        }
        return std::nullopt;
    }

private:
    struct Entry {
        CString surrogate;
        CString original;
    };

    CString uniqueSurrogate() const WTF_REQUIRES_LOCK(m_lock)
    {
        static constexpr std::array<char, 64> alphabet {
            'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
            'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
            'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
            'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'
        };
        while (true) {
            std::array<uint8_t, iceCredentialSurrogateLength> randomValues;
            cryptographicallyRandomValues(randomValues);
            std::array<char, iceCredentialSurrogateLength> characters;
            for (size_t i = 0; i < characters.size(); ++i)
                characters[i] = alphabet[randomValues[i] % alphabet.size()];
            std::span<const char> candidate { characters };
            bool isUsed = false;
            for (auto& entry : m_entries) {
                if (spansEqual(entry.surrogate.span(), candidate) || spansEqual(entry.original.span(), candidate)) {
                    isUsed = true;
                    break;
                }
            }
            if (!isUsed)
                return CString(candidate);
        }
    }

    mutable Lock m_lock;
    Deque<Entry> m_entries WTF_GUARDED_BY_LOCK(m_lock);
};

static GQuark iceCredentialSurrogatesQuark()
{
    static GQuark quark = g_quark_from_static_string("webkit-ice-credential-surrogates");
    return quark;
}

static IceCredentialSurrogates* iceCredentialSurrogates(GstWebRTCICE* ice)
{
    if (!ice)
        return nullptr;
    return static_cast<IceCredentialSurrogates*>(g_object_get_qdata(G_OBJECT(ice), iceCredentialSurrogatesQuark()));
}

static IceCredentialSurrogates* iceCredentialSurrogates(GstElement* webrtcBin)
{
    if (!webrtcBin)
        return nullptr;
    GRefPtr<GstWebRTCICE> ice;
    g_object_get(webrtcBin, "ice-agent", &ice.outPtr(), nullptr);
    return iceCredentialSurrogates(ice.get());
}

static GstWebRTCICEClass* lenientNiceParentClass;

static gboolean lenientNiceSetRemoteCredentials(GstWebRTCICE* ice, GstWebRTCICEStream* stream, const gchar* usernameFragment, const gchar* password)
{
    auto* surrogates = iceCredentialSurrogates(ice);
    if (!surrogates || !usernameFragment || !password)
        return lenientNiceParentClass->set_remote_credentials(ice, stream, usernameFragment, password);
    auto originalUsernameFragment = surrogates->originalForSurrogate(unsafeSpan(usernameFragment));
    auto originalPassword = surrogates->originalForSurrogate(unsafeSpan(password));
    return lenientNiceParentClass->set_remote_credentials(ice, stream,
        originalUsernameFragment ? originalUsernameFragment->data() : usernameFragment,
        originalPassword ? originalPassword->data() : password);
}

static void lenientNiceClassInit(gpointer klass, gpointer)
{
    lenientNiceParentClass = GST_WEBRTC_ICE_CLASS(g_type_class_peek_parent(klass));
    GST_WEBRTC_ICE_CLASS(klass)->set_remote_credentials = lenientNiceSetRemoteCredentials;
}

static GType lenientNiceType()
{
    static GType type;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        GType parentType = gst_webrtc_nice_get_type();
        GTypeQuery query;
        g_type_query(parentType, &query);
        if (!query.type || query.class_size > std::numeric_limits<guint16>::max() || query.instance_size > std::numeric_limits<guint16>::max())
            return;
        GTypeInfo info { };
        info.class_size = static_cast<guint16>(query.class_size);
        info.class_init = lenientNiceClassInit;
        info.instance_size = static_cast<guint16>(query.instance_size);
        type = g_type_register_static(parentType, "WebKitGstWebRTCLenientNice", &info, static_cast<GTypeFlags>(0));
    });
    return type;
}

static GRefPtr<GstWebRTCICE> createLenientNiceAgent(const String& name)
{
    GType type = lenientNiceType();
    if (!type)
        return nullptr;
    GRefPtr<GstWebRTCICE> agent = GST_WEBRTC_ICE(g_object_new(type, "name", name.ascii().data(), nullptr));
    g_object_set_qdata_full(G_OBJECT(agent.get()), iceCredentialSurrogatesQuark(), new IceCredentialSurrogates, [](gpointer data) {
        delete static_cast<IceCredentialSurrogates*>(data);
    });
    return agent;
}

static void replaceIceCredentialsWithSurrogates(GstSDPMessage* message, IceCredentialSurrogates& surrogates)
{
    auto surrogateForAttribute = [&](const GstSDPAttribute* attribute) -> std::optional<CString> {
        auto field = iceCredentialField(attribute);
        if (!field || !iceCredentialNeedsSurrogate(*field, unsafeSpan(attribute->value)))
            return std::nullopt;
        return surrogates.surrogateFor(unsafeSpan(attribute->value));
    };
    for (unsigned i = 0; i < gst_sdp_message_attributes_len(message); i++) {
        const auto* attribute = gst_sdp_message_get_attribute(message, i);
        if (auto surrogate = surrogateForAttribute(attribute)) {
            GstSDPAttribute replacement { };
            gst_sdp_attribute_set(&replacement, attribute->key, surrogate->data());
            gst_sdp_message_replace_attribute(message, i, &replacement);
        }
    }
    for (unsigned i = 0; i < gst_sdp_message_medias_len(message); i++) {
        auto* media = const_cast<GstSDPMedia*>(gst_sdp_message_get_media(message, i));
        for (unsigned j = 0; j < gst_sdp_media_attributes_len(media); j++) {
            const auto* attribute = gst_sdp_media_get_attribute(media, j);
            if (auto surrogate = surrogateForAttribute(attribute)) {
                GstSDPAttribute replacement { };
                gst_sdp_attribute_set(&replacement, attribute->key, surrogate->data());
                gst_sdp_media_replace_attribute(media, j, &replacement);
            }
        }
    }
}

static void restoreIceCredentials(GstSDPMessage* message, const IceCredentialSurrogates& surrogates)
{
    auto originalForAttribute = [&](const GstSDPAttribute* attribute) -> std::optional<CString> {
        if (!iceCredentialField(attribute))
            return std::nullopt;
        return surrogates.originalForSurrogate(unsafeSpan(attribute->value));
    };
    for (unsigned i = 0; i < gst_sdp_message_attributes_len(message); i++) {
        const auto* attribute = gst_sdp_message_get_attribute(message, i);
        if (auto original = originalForAttribute(attribute)) {
            GstSDPAttribute replacement { };
            gst_sdp_attribute_set(&replacement, attribute->key, original->data());
            gst_sdp_message_replace_attribute(message, i, &replacement);
        }
    }
    for (unsigned i = 0; i < gst_sdp_message_medias_len(message); i++) {
        auto* media = const_cast<GstSDPMedia*>(gst_sdp_message_get_media(message, i));
        for (unsigned j = 0; j < gst_sdp_media_attributes_len(media); j++) {
            const auto* attribute = gst_sdp_media_get_attribute(media, j);
            if (auto original = originalForAttribute(attribute)) {
                GstSDPAttribute replacement { };
                gst_sdp_attribute_set(&replacement, attribute->key, original->data());
                gst_sdp_media_replace_attribute(media, j, &replacement);
            }
        }
    }
}

static void normalizeLegacyDataChannelMedia(GstSDPMedia* media)
{
    auto mediaType = gst_sdp_media_get_media(media);
    auto protocol = gst_sdp_media_get_proto(media);
    if (!mediaType || !protocol || !equal(unsafeSpan(mediaType), "application"_s) || gst_sdp_media_formats_len(media) != 1)
        return;
    if (!equal(unsafeSpan(protocol), "DTLS/SCTP"_s) && !equal(unsafeSpan(protocol), "UDP/DTLS/SCTP"_s))
        return;
    auto format = gst_sdp_media_get_format(media, 0);
    auto port = format ? parseInteger<uint16_t>(StringView::fromLatin1(format)) : std::nullopt;
    if (!port)
        return;
    std::optional<unsigned> sctpMapIndex;
    bool hasSctpPort = false;
    for (unsigned i = 0; i < gst_sdp_media_attributes_len(media); i++) {
        const auto* attribute = gst_sdp_media_get_attribute(media, i);
        if (!attribute->key)
            continue;
        auto key = unsafeSpan(attribute->key);
        if (equal(key, "sctp-port"_s)) {
            hasSctpPort = true;
            continue;
        }
        if (!equal(key, "sctpmap"_s) || !attribute->value || sctpMapIndex)
            continue;
        auto fields = String::fromLatin1(attribute->value).split(' ');
        if (fields.size() >= 2 && parseInteger<uint16_t>(fields[0]) == port && fields[1] == "webrtc-datachannel"_s)
            sctpMapIndex = i;
    }
    if (!sctpMapIndex)
        return;
    gst_sdp_media_remove_attribute(media, *sctpMapIndex);
    gst_sdp_media_set_proto(media, "UDP/DTLS/SCTP");
    gst_sdp_media_replace_format(media, 0, "webrtc-datachannel");
    if (!hasSctpPort)
        gst_sdp_media_add_attribute(media, "sctp-port", String::number(*port).ascii().data());
}

static bool isIceCandidateOnUnusableTransport(const String& candidate)
{
    auto tokens = candidate.split(' ');
    if (tokens.size() < 8 || !equalLettersIgnoringASCIICase(tokens[6], "typ"_s))
        return false;
    return !equalLettersIgnoringASCIICase(tokens[2], "udp"_s) && !equalLettersIgnoringASCIICase(tokens[2], "tcp"_s);
}

static bool isSourceBoundToAnotherMLine(RealtimeOutgoingMediaSourceGStreamer& source, unsigned mLineIndex)
{
    const auto& pad = source.pad();
    if (!pad)
        return false;
    GRefPtr<GstWebRTCRTPTransceiver> transceiver;
    g_object_get(pad.get(), "transceiver", &transceiver.outPtr(), nullptr);
    if (!transceiver)
        return false;
    unsigned boundMLineIndex;
    g_object_get(transceiver.get(), "mlineindex", &boundMLineIndex, nullptr);
    return boundMLineIndex != std::numeric_limits<unsigned>::max() && boundMLineIndex != mLineIndex;
}

static void normalizeRemoteDescription(GstSDPMessage* message, IceCredentialSurrogates* surrogates)
{
    if (surrogates)
        replaceIceCredentialsWithSurrogates(message, *surrogates);
    for (unsigned i = 0; i < gst_sdp_message_medias_len(message); i++)
        normalizeLegacyDataChannelMedia(const_cast<GstSDPMedia*>(gst_sdp_message_get_media(message, i)));
}

GStreamerMediaEndpoint::GStreamerMediaEndpoint('''


def endpoint_patch(text):
    text = replace_once(
        text,
        "#include <wtf/text/StringToIntegerConversion.h>\n",
        "#include <wtf/text/StringToIntegerConversion.h>\n"
        "#include <wtf/CryptographicallyRandomNumber.h>\n"
        "#include <wtf/Deque.h>\n"
        "#include <wtf/Lock.h>\n"
        "#include <wtf/text/CString.h>\n",
    )
    text = replace_once(
        text,
        "#define GST_CAT_DEFAULT webkit_webrtc_endpoint_debug\n\nnamespace WebCore {\n\nGStreamerMediaEndpoint::GStreamerMediaEndpoint(",
        "#define GST_CAT_DEFAULT webkit_webrtc_endpoint_debug\n\n"
        'extern "C" GType gst_webrtc_nice_get_type(void);\n\n' + REMOTE_INTEROP,
    )
    text = replace_once(
        text,
        "#endif // USE(LIBRICE) && GST_CHECK_VERSION(1, 20, 0)\n"
        "    if (!m_webrtcBin)\n"
        "        m_webrtcBin = gst_element_factory_create(webrtcBinFactory.get(), binName.ascii().data());\n",
        "#endif // USE(LIBRICE) && GST_CHECK_VERSION(1, 20, 0)\n"
        "#if GST_CHECK_VERSION(1, 22, 0)\n"
        "    if (!m_webrtcBin && gst_check_version(1, 22, 0)) {\n"
        '        if (auto agent = createLenientNiceAgent(makeString(binName, ":ice"_s))) {\n'
        "            auto* transferredAgent = agent.leakRef();\n"
        '            m_webrtcBin = gst_element_factory_create_full(webrtcBinFactory.get(), "name", binName.ascii().data(), "ice-agent", transferredAgent, nullptr);\n'
        "            if (!m_webrtcBin)\n"
        "                gst_object_unref(transferredAgent);\n"
        "        }\n"
        "    }\n"
        "#endif\n"
        "    if (!m_webrtcBin)\n"
        "        m_webrtcBin = gst_element_factory_create(webrtcBinFactory.get(), binName.ascii().data());\n",
    )
    text = replace_once(
        text,
        "static std::optional<std::pair<RTCSdpType, String>> fetchDescription(GstElement* webrtcBin, ASCIILiteral name)\n",
        "static std::optional<std::pair<RTCSdpType, String>> fetchDescription(GstElement* webrtcBin, ASCIILiteral name, const IceCredentialSurrogates* surrogates = nullptr)\n",
    )
    text = replace_once(
        text,
        "    auto sdpString = GMallocString::unsafeAdoptFromUTF8(gst_sdp_message_as_text(description->sdp));\n",
        "    if (surrogates)\n"
        "        restoreIceCredentials(description->sdp, *surrogates);\n\n"
        "    auto sdpString = GMallocString::unsafeAdoptFromUTF8(gst_sdp_message_as_text(description->sdp));\n",
    )
    text = replace_once(
        text,
        '    if (auto currentRemoteDescription = fetchDescription(webrtcBin, "current-remote"_s)) {',
        "    auto* surrogates = iceCredentialSurrogates(webrtcBin);\n"
        '    if (auto currentRemoteDescription = fetchDescription(webrtcBin, "current-remote"_s, surrogates)) {',
    )
    text = replace_once(
        text,
        '    if (auto pendingRemoteDescription = fetchDescription(webrtcBin, "pending-remote"_s)) {',
        '    if (auto pendingRemoteDescription = fetchDescription(webrtcBin, "pending-remote"_s, surrogates)) {',
    )
    text = replace_once(
        text,
        "                g_object_get(m_webrtcBin.get(), \"ice-agent\", &ice.outPtr(), nullptr);\n"
        "                g_object_set(ice.get(), \"ice-lite\", TRUE, nullptr);\n",
        "                g_object_get(m_webrtcBin.get(), \"ice-agent\", &ice.outPtr(), nullptr);\n"
        "                if (ice && gstObjectHasProperty(GST_OBJECT_CAST(ice.get()), \"ice-lite\"_s))\n"
        "                    g_object_set(ice.get(), \"ice-lite\", TRUE, nullptr);\n",
    )
    start = text.index("void GStreamerMediaEndpoint::recycleTransceiverForSenderTrack(")
    end = text.index("void GStreamerMediaEndpoint::doCreateOffer(")
    if text.count("void GStreamerMediaEndpoint::recycleTransceiverForSenderTrack(") != 1 or end < start:
        raise RuntimeError("The pinned WebKit source does not match the sender source correction")
    text = text[:start] + SENDER_SOURCE + text[end + len("void GStreamerMediaEndpoint::doCreateOffer("):]
    text = replace_once(
        text,
        "GRefPtr<GstPad> GStreamerMediaEndpoint::requestPad(const GRefPtr<GstCaps>& allowedCaps, const String& mediaStreamID)\n",
        "GRefPtr<GstPad> GStreamerMediaEndpoint::requestPad(const GRefPtr<GstCaps>& allowedCaps, const String& mediaStreamID, std::optional<unsigned> mLineIndex)\n",
    )
    text = replace_once(
        text,
        "    auto kind = webrtcKindFromCaps(caps);\n    forEachTransceiver(m_webrtcBin, [&](auto&& transceiver) -> bool {\n        GstWebRTCKind transceiverKind;\n",
        "    auto kind = webrtcKindFromCaps(caps);\n    if (!mLineIndex) forEachTransceiver(m_webrtcBin, [&](auto&& transceiver) -> bool {\n        GstWebRTCKind transceiverKind;\n",
    )
    text = replace_once(
        text,
        "    GRefPtr sinkPad = adoptGRef(gst_element_request_pad(m_webrtcBin.get(), padTemplate, nullptr, caps.get()));\n",
        "    auto padName = mLineIndex ? makeString(\"sink_\"_s, *mLineIndex) : String();\n"
        "    GRefPtr sinkPad = adoptGRef(gst_element_request_pad(m_webrtcBin.get(), padTemplate, mLineIndex ? padName.ascii().data() : nullptr, caps.get()));\n"
        "    if (!sinkPad) {\n"
        "        GST_WARNING_OBJECT(m_pipeline.get(), \"Unable to request a sink pad for %\" GST_PTR_FORMAT, caps.get());\n"
        "        return nullptr;\n"
        "    }\n",
    )
    text = replace_once(
        text,
        "            if (source->type() != sourceType) {\n"
        "                GST_DEBUG_OBJECT(pipeline(), \"Un-matched source type\");\n"
        "                return false;\n"
        "            }\n",
        "            if (source->type() != sourceType) {\n"
        "                GST_DEBUG_OBJECT(pipeline(), \"Un-matched source type\");\n"
        "                return false;\n"
        "            }\n"
        "            if (isSourceBoundToAnotherMLine(*source, i))\n"
        "                return false;\n",
    )
    text = replace_once(
        text,
        "            auto allowedCaps = capsFromSDPMedia(media);\n"
        "            source->configure(WTF::move(allowedCaps));\n"
        "            if (!source->pad()) {\n",
        "            if (!source->pad()) {\n"
        "                auto allowedCaps = capsFromSDPMedia(media);\n"
        "                source->configure(WTF::move(allowedCaps));\n",
    )
    text = replace_once(
        text,
        "    GST_DEBUG_OBJECT(m_pipeline.get(), \"Creating %s\", type.characters());\n",
        "    GST_DEBUG_OBJECT(m_pipeline.get(), \"Creating %s\", type.characters());\n"
        "    attachPendingSenderSources();\n",
    )
    text = replace_once(
        text,
        "    RefPtr initialDescription = description;\n    if (!initialDescription) {\n",
        "    RefPtr initialDescription = description;\n    if (!initialDescription) {\n        attachPendingSenderSources();\n",
    )
    text = replace_once(
        text,
        "            if (descriptionType == DescriptionType::Remote) {\n"
        "                GUniqueOutPtr<GstWebRTCSessionDescription> currentDescription;\n",
        "            if (descriptionType == DescriptionType::Remote) {\n"
        "                normalizeRemoteDescription(message.get(), iceCredentialSurrogates(m_webrtcBin.get()));\n"
        "                GUniqueOutPtr<GstWebRTCSessionDescription> currentDescription;\n",
    )
    text = replace_once(
        text,
        "        bundlePolicy = bundlePolicyFromConfiguration(configuration);\n",
        "        bundlePolicy = bundlePolicyFromConfiguration(configuration);\n"
        "    if (bundlePolicy == GST_WEBRTC_BUNDLE_POLICY_BALANCED)\n"
        "        bundlePolicy = GST_WEBRTC_BUNDLE_POLICY_MAX_COMPAT;\n",
    )
    return replace_once(
        text,
        "    m_statsCollector->invalidateCache();\n\n    // https://gitlab.freedesktop.org/gstreamer/gstreamer/-/merge_requests/3960\n",
        UNUSABLE_TRANSPORT_CANDIDATE + "    m_statsCollector->invalidateCache();\n\n    // https://gitlab.freedesktop.org/gstreamer/gstreamer/-/merge_requests/3960\n",
    )


UNUSABLE_TRANSPORT_CANDIDATE = r'''    if (isIceCandidateOnUnusableTransport(sdp)) {
        GST_DEBUG_OBJECT(m_pipeline.get(), "Skipping ICE candidate on a transport the ICE agent cannot use: %s", sdpString.data());
        callOnMainThread([task = createSharedTask<PeerConnectionBackend::AddIceCandidateCallbackFunction>(WTF::move(callback)), weakWebrtcBin = GThreadSafeWeakPtr { m_webrtcBin.get() }]() mutable {
            auto webrtcBin = weakWebrtcBin.get();
            if (!webrtcBin)
                return;
            task->run(descriptionsFromWebRTCBin(webrtcBin.get()));
        });
        return;
    }

'''


CODEC_PREFERENCES = r'''static bool isDerivedRedundancyCodec(const String& codecName)
{
    return equalLettersIgnoringASCIICase(codecName, "rtx"_s) || equalLettersIgnoringASCIICase(codecName, "red"_s) || equalLettersIgnoringASCIICase(codecName, "ulpfec"_s) || equalLettersIgnoringASCIICase(codecName, "flexfec-03"_s);
}

static Vector<std::pair<String, String>> codecFormatParameters(const String& line)
{
    Vector<std::pair<String, String>> parameters;
    for (auto& attribute : line.split(';')) {
        auto separator = attribute.find('=');
        if (separator == notFound)
            continue;
        auto name = attribute.left(separator).trim([](char16_t character) { return isASCIIWhitespace(character); });
        auto value = attribute.substring(separator + 1).trim([](char16_t character) { return isASCIIWhitespace(character); });
        if (!name.isEmpty())
            parameters.append({ WTF::move(name), WTF::move(value) });
    }
    return parameters;
}

static unsigned codecChannelCount(const GstStructure* structure)
{
    int channels = 0;
    if (gst_structure_get_int(structure, "channels", &channels) && channels > 0)
        return channels;
    if (const char* parameters = gst_structure_get_string(structure, "encoding-params")) {
        if (auto count = parseInteger<unsigned>(StringView::fromLatin1(parameters)); count && *count)
            return *count;
    }
    return 1;
}

static String h264ProfileName(const String& profileLevelId)
{
    if (profileLevelId.length() != 6)
        return { };
    std::array<uint8_t, 3> bytes { };
    for (unsigned index = 0; index < bytes.size(); index++) {
        auto value = parseInteger<uint8_t>(StringView(profileLevelId).substring(index * 2, 2), 16);
        if (!value)
            return { };
        bytes[index] = *value;
    }
    if (const char* profile = gst_codec_utils_h264_get_profile(bytes.data(), bytes.size()))
        return String::fromLatin1(profile);
    return { };
}

static String codecParameter(const Vector<std::pair<String, String>>& parameters, ASCIILiteral name)
{
    for (auto& [key, value] : parameters) {
        if (equalIgnoringASCIICase(key, name))
            return value;
    }
    return { };
}

static String structureParameter(const GstStructure* structure, ASCIILiteral name)
{
    if (const char* value = gst_structure_get_string(structure, name.characters()))
        return String::fromLatin1(value);
    return { };
}

static bool sameCodecParameter(String requested, String current, ASCIILiteral fallback)
{
    if (requested.isNull())
        requested = fallback;
    if (current.isNull())
        current = fallback;
    return equalIgnoringASCIICase(requested, current);
}

static bool codecMatchesStructure(const RTCRtpCodec& codec, const String& mediaType, const String& codecName, const Vector<std::pair<String, String>>& parameters, const GstStructure* structure, bool compareAllParameters)
{
    if (const char* media = gst_structure_get_string(structure, "media"); media && !equalIgnoringASCIICase(StringView::fromLatin1(media), mediaType))
        return false;
    const char* encodingName = gst_structure_get_string(structure, "encoding-name");
    if (!encodingName || !equalIgnoringASCIICase(StringView::fromLatin1(encodingName), codecName))
        return false;
    int clockRate = 0;
    if (!gst_structure_get_int(structure, "clock-rate", &clockRate) || clockRate != static_cast<int>(codec.clockRate))
        return false;
    if (codec.channels && codecChannelCount(structure) != *codec.channels)
        return false;

    if (equalLettersIgnoringASCIICase(codecName, "h264"_s)) {
        if (!sameCodecParameter(codecParameter(parameters, "packetization-mode"_s), structureParameter(structure, "packetization-mode"_s), "0"_s))
            return false;
        auto requestedProfile = h264ProfileName(codecParameter(parameters, "profile-level-id"_s));
        auto currentProfile = structureParameter(structure, "profile"_s);
        if (currentProfile.isNull())
            currentProfile = h264ProfileName(structureParameter(structure, "profile-level-id"_s));
        if (!requestedProfile.isNull() && !currentProfile.isNull() && !equalIgnoringASCIICase(requestedProfile, currentProfile))
            return false;
    } else if (equalLettersIgnoringASCIICase(codecName, "vp9"_s)) {
        if (!sameCodecParameter(codecParameter(parameters, "profile-id"_s), structureParameter(structure, "profile-id"_s), "0"_s))
            return false;
    } else if (equalLettersIgnoringASCIICase(codecName, "av1"_s)) {
        if (!sameCodecParameter(codecParameter(parameters, "profile"_s), structureParameter(structure, "profile"_s), "0"_s))
            return false;
    }

    if (!compareAllParameters)
        return true;
    for (auto& [key, value] : parameters) {
        if (equalLettersIgnoringASCIICase(key, "profile-level-id"_s) || equalLettersIgnoringASCIICase(key, "profile"_s))
            continue;
        auto field = key.utf8();
        if (const char* current = gst_structure_get_string(structure, field.data()); current && !equalIgnoringASCIICase(StringView::fromLatin1(current), value))
            return false;
    }
    return true;
}

ExceptionOr<void> GStreamerRtpTransceiverBackend::setCodecPreferences(const Vector<RTCRtpCodec>& codecs)
{
    GRefPtr<GstCaps> currentCaps;
    g_object_get(m_rtcTransceiver.get(), "codec-preferences", &currentCaps.outPtr(), nullptr);
    GST_TRACE_OBJECT(m_rtcTransceiver.get(), "Current codec preferences: %" GST_PTR_FORMAT, currentCaps.get());
    if (codecs.isEmpty() || !currentCaps || gst_caps_is_empty(currentCaps.get()))
        return { };

    Vector<const GstStructure*> negotiatedCodecs;
    for (unsigned index = 0; index < gst_caps_get_size(currentCaps.get()); index++)
        negotiatedCodecs.append(gst_caps_get_structure(currentCaps.get(), index));
    const GstStructure* extensionSource = negotiatedCodecs[0];
    String msid;
    for (auto* structure : negotiatedCodecs) {
        if (const char* value = gst_structure_get_string(structure, "a-msid")) {
            msid = String::fromUTF8(value);
            break;
        }
    }

    Vector<bool> consumed;
    consumed.fill(false, negotiatedCodecs.size());
    GRefPtr gstCodecs = adoptGRef(gst_caps_new_empty());
    for (auto& codec : codecs) {
        if (!codec.mimeType.startsWithIgnoringASCIICase("video/"_s) && !codec.mimeType.startsWithIgnoringASCIICase("audio/"_s))
            return Exception { ExceptionCode::InvalidModificationError, "RTCRtpCodec bad mimeType"_s };

        auto separator = codec.mimeType.find('/');
        auto mediaType = codec.mimeType.left(separator).convertToASCIILowercase();
        auto codecName = codec.mimeType.substring(separator + 1);
        if (codecName.isEmpty())
            return Exception { ExceptionCode::InvalidModificationError, "RTCRtpCodec bad mimeType"_s };
        if (isDerivedRedundancyCodec(codecName))
            continue;

        auto parameters = codecFormatParameters(codec.sdpFmtpLine);
        std::optional<size_t> match;
        for (bool compareAllParameters : { true, false }) {
            for (size_t index = 0; index < negotiatedCodecs.size() && !match; index++) {
                if (!consumed[index] && codecMatchesStructure(codec, mediaType, codecName, parameters, negotiatedCodecs[index], compareAllParameters))
                    match = index;
            }
            if (match)
                break;
        }
        if (!match) {
            GST_DEBUG_OBJECT(m_rtcTransceiver.get(), "No negotiated payload type for %s, leaving it out of the codec preferences", codec.mimeType.utf8().data());
            continue;
        }

        consumed[*match] = true;
        auto* structure = gst_structure_copy(negotiatedCodecs[*match]);
        for (int field = 0; field < gst_structure_n_fields(extensionSource); field++) {
            const char* name = gst_structure_nth_field_name(extensionSource, field);
            if (g_str_has_prefix(name, "extmap-") && !gst_structure_has_field(structure, name))
                gst_structure_set_value(structure, name, gst_structure_get_value(extensionSource, name));
        }
        gst_caps_append_structure(gstCodecs.get(), structure);
    }

    if (gst_caps_is_empty(gstCodecs.get())) {
        GST_DEBUG_OBJECT(m_rtcTransceiver.get(), "None of the preferred codecs is negotiable, keeping the current codec preferences");
        return { };
    }

    if (!msid.isEmpty()) {
        auto* first = gst_caps_get_structure(gstCodecs.get(), 0);
        if (!gst_structure_has_field(first, "a-msid"))
            gst_structure_set(first, "a-msid", G_TYPE_STRING, msid.utf8().data(), nullptr);
    }
'''


def replace_between(text, start, end, replacement):
    if text.count(start) != 1 or text.count(end) != 1:
        raise RuntimeError("The pinned source does not match the WebRTC interop patch: " + start[:80])
    begin = text.index(start)
    finish = text.index(end, begin) + len(end)
    return text[:begin] + replacement + text[finish:]


def transceiver_patch(text):
    text = replace_once(
        text,
        '#include "RTCRtpCodec.h"\n',
        '#include "RTCRtpCodec.h"\n#include <array>\n#include <gst/pbutils/codec-utils.h>\n#include <wtf/text/StringToIntegerConversion.h>\n',
    )
    return replace_between(
        text,
        "[[nodiscard]] static inline ExceptionOr<GRefPtr<GstCaps>> toRtpCodecCapability(",
        "        gst_caps_append(gstCodecs.get(), codecCaps.leakRef());\n    }\n",
        CODEC_PREFERENCES,
    )


def find_gstreamer_patch(text):
    text = replace_once(
        text,
        "FIND_GSTREAMER_COMPONENT(GSTREAMER_WEBRTC gstreamer-webrtc-1.0 gstwebrtc-1.0)\n",
        "FIND_GSTREAMER_COMPONENT(GSTREAMER_WEBRTC gstreamer-webrtc-1.0 gstwebrtc-1.0)\n"
        "FIND_GSTREAMER_COMPONENT(GSTREAMER_WEBRTC_NICE gstreamer-webrtc-nice-1.0 gstwebrtcnice-1.0)\n",
    )
    return replace_once(
        text,
        "    GSTREAMER_WEBRTC_INCLUDE_DIRS\n    GSTREAMER_WEBRTC_LIBRARIES\n)",
        "    GSTREAMER_WEBRTC_INCLUDE_DIRS\n    GSTREAMER_WEBRTC_LIBRARIES\n"
        "    GSTREAMER_WEBRTC_NICE_INCLUDE_DIRS\n    GSTREAMER_WEBRTC_NICE_LIBRARIES\n)",
    )


def gstreamer_checks_patch(text):
    return replace_once(
        text,
        "              list(APPEND GSTREAMER_COMPONENTS webrtc)\n",
        "              list(APPEND GSTREAMER_COMPONENTS webrtc webrtc_nice)\n",
    )


def webcore_gstreamer_patch(text):
    return replace_once(
        text,
        "                ${GSTREAMER_SDP_LIBRARIES}\n                ${GSTREAMER_WEBRTC_LIBRARIES}\n",
        "                ${GSTREAMER_SDP_LIBRARIES}\n                ${GSTREAMER_WEBRTC_LIBRARIES}\n"
        "                ${GSTREAMER_WEBRTC_NICE_LIBRARIES}\n",
    )


def endpoint_header_patch(text):
    text = replace_once(
        text,
        "    void recycleTransceiverForSenderTrack(GStreamerRtpTransceiverBackend*, MediaStreamTrack&, const FixedVector<String>&);\n",
        "    bool attachSenderSource(GStreamerRtpSenderBackend&, MediaStreamTrack&);\n"
        "    void linkSenderSourceIfNegotiated(GStreamerRtpSenderBackend&);\n"
        "    void attachPendingSenderSources();\n",
    )
    return replace_once(
        text,
        "    [[nodiscard]] GRefPtr<GstPad> requestPad(const GRefPtr<GstCaps>&, const String& mediaStreamID);\n",
        "    [[nodiscard]] GRefPtr<GstPad> requestPad(const GRefPtr<GstCaps>&, const String& mediaStreamID, std::optional<unsigned> mLineIndex = std::nullopt);\n",
    )


def peer_connection_patch(text):
    text = replace_once(
        text,
        "        m_endpoint->recycleTransceiverForSenderTrack(reinterpret_cast<GStreamerRtpTransceiverBackend*>(&transceiver->backend()), track, mediaStreamIds);\n\n",
        "",
    )
    return replace_once(
        text,
        "GStreamerRtpSenderBackend::Source GStreamerPeerConnectionBackend::createSourceForTrack(MediaStreamTrack& track)\n{\n    return m_endpoint->createSourceForTrack(track);\n}\n",
        "GStreamerRtpSenderBackend::Source GStreamerPeerConnectionBackend::createSourceForTrack(MediaStreamTrack& track)\n{\n    return m_endpoint->createSourceForTrack(track);\n}\n"
        """
void GStreamerPeerConnectionBackend::attachSourcesToSendingSenders()
{
    Ref peerConnection = m_peerConnection;
    for (const auto& transceiver : peerConnection->currentTransceivers()) {
        if (transceiver->stopped())
            continue;
        auto& sender = transceiver->sender();
        RefPtr track = sender.track();
        if (!track)
            continue;
        auto& senderBackend = backendFromRTPSender(sender);
        if (senderBackend.hasSource())
            continue;
        m_endpoint->attachSenderSource(senderBackend, *track);
    }
}

void GStreamerPeerConnectionBackend::attachSourceForReplacedTrack(GStreamerRtpSenderBackend& senderBackend, MediaStreamTrack& track)
{
    if (m_endpoint->attachSenderSource(senderBackend, track))
        m_endpoint->linkSenderSourceIfNegotiated(senderBackend);
}
""",
    )


def peer_connection_header_patch(text):
    return replace_once(
        text,
        "    GStreamerRtpSenderBackend::Source createSourceForTrack(MediaStreamTrack&);\n",
        "    GStreamerRtpSenderBackend::Source createSourceForTrack(MediaStreamTrack&);\n"
        "    void attachSourcesToSendingSenders();\n"
        "    void attachSourceForReplacedTrack(GStreamerRtpSenderBackend&, MediaStreamTrack&);\n",
    )


def sender_patch(text):
    text = replace_once(
        text,
        "    peerConnectionBackend->trackWasReplaced(previousTrackId, newTrack ? newTrack->id() : emptyString());\n",
        "    if (!hasSource() && newTrack)\n"
        "        peerConnectionBackend->attachSourceForReplacedTrack(*this, *newTrack);\n",
    )
    return replace_once(
        text,
        "void GStreamerRtpSenderBackend::setMediaStreamIds(const FixedVector<String>&)\n{\n    notImplemented();\n}\n",
        "void GStreamerRtpSenderBackend::setMediaStreamIds(const FixedVector<String>& mediaStreamIds)\n{\n"
        "    m_mediaStreamId = mediaStreamIds.isEmpty() ? String() : mediaStreamIds[0];\n}\n",
    )


def sender_header_patch(text):
    text = replace_once(
        text,
        "    bool hasSource() const { return !std::holds_alternative<std::nullptr_t>(m_source); }\n",
        "    bool hasSource() const { return !std::holds_alternative<std::nullptr_t>(m_source); }\n"
        "    const String& mediaStreamId() const { return m_mediaStreamId; }\n",
    )
    return replace_once(
        text,
        "    mutable GUniquePtr<GstStructure> m_currentParameters;\n};\n",
        "    mutable GUniquePtr<GstStructure> m_currentParameters;\n    String m_mediaStreamId;\n};\n",
    )


REPLACED_TRACK_MSID = """    if (m_transceiver) {
        GRefPtr<GstCaps> caps;
        g_object_get(m_transceiver.get(), "codec-preferences", &caps.outPtr(), nullptr);

        m_pendingTrackId = trackPrivate ? trackPrivate->id() : emptyString();
"""


def outgoing_source_patch(text):
    return replace_between(
        text,
        REPLACED_TRACK_MSID,
        "        m_pendingTrackId = emptyString();\n        g_object_thaw_notify(G_OBJECT(m_transceiver.get()));\n    }\n",
        "",
    )


PATCHES = {
    ENDPOINT: endpoint_patch,
    ENDPOINT_HEADER: endpoint_header_patch,
    PEER_CONNECTION: peer_connection_patch,
    PEER_CONNECTION_HEADER: peer_connection_header_patch,
    SENDER: sender_patch,
    SENDER_HEADER: sender_header_patch,
    FIND_GSTREAMER: find_gstreamer_patch,
    GSTREAMER_CHECKS: gstreamer_checks_patch,
    WEBCORE_GSTREAMER: webcore_gstreamer_patch,
    TRANSCEIVER: transceiver_patch,
    OUTGOING_SOURCE: outgoing_source_patch,
}

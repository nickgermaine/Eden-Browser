from wpe.patch_support import replace_once


ENCODER = "Source/WebCore/platform/gstreamer/VideoEncoderPrivateGStreamer.cpp"


ACCELERATED_BITRATE = r"""static bool encoderUsesVideoAcceleration(EncoderId encoderId)
{
    switch (encoderId) {
    case VaapiH264:
    case VaapiH264LP:
    case VaapiH265:
    case VaapiAv1:
        return true;
    default:
        return false;
    }
}

static bool acceleratedEncoderShouldTakeBitrate(unsigned requested, unsigned applied, Seconds sinceRequest, Seconds sinceRestart)
{
    if (requested == applied)
        return false;
    if (!applied)
        return true;
    double ratio = static_cast<double>(requested) / applied;
    if (ratio <= 0.5)
        return true;
    if (ratio <= 0.85 || ratio >= 1.25)
        return sinceRestart >= 1_s;
    return sinceRequest >= 5_s && sinceRestart >= 5_s;
}

static Vector<std::pair<size_t, size_t>> acceleratedEncoderFillerRanges(EncoderId encoderId, std::span<const uint8_t> data)
{
    Vector<std::pair<size_t, size_t>> ranges;
    if (encoderId == VaapiAv1) {
        size_t index = 0;
        while (index < data.size()) {
            uint8_t header = data[index];
            size_t position = index + ((header & 0x04) ? 2 : 1);
            size_t end = data.size();
            if (header & 0x02) {
                uint64_t size = 0;
                for (unsigned shift = 0;; shift += 7) {
                    if (position >= data.size() || shift > 56)
                        return { };
                    uint8_t byte = data[position++];
                    size |= static_cast<uint64_t>(byte & 0x7f) << shift;
                    if (!(byte & 0x80))
                        break;
                }
                if (size > data.size() - position)
                    return { };
                end = position + size;
            }
            if (((header >> 3) & 0x0f) == 15)
                ranges.append({ index, end });
            index = end;
        }
        return ranges;
    }

    bool isH265 = encoderId == VaapiH265;
    Vector<std::pair<size_t, size_t>> units;
    for (size_t index = 0; index + 3 < data.size(); ++index) {
        if (data[index] || data[index + 1] || data[index + 2] != 1)
            continue;
        units.append({ index && !data[index - 1] ? index - 1 : index, index + 3 });
        index += 2;
    }
    for (size_t unit = 0; unit < units.size(); ++unit) {
        uint8_t header = data[units[unit].second];
        if ((isH265 ? (header >> 1) & 0x3f : header & 0x1f) == (isH265 ? 38 : 12))
            ranges.append({ units[unit].first, unit + 1 < units.size() ? units[unit + 1].first : data.size() });
    }
    return ranges;
}

static GstPadProbeReturn videoEncoderDropFiller(GstPad*, GstPadProbeInfo* info, gpointer encoderId)
{
    auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    Vector<std::pair<size_t, size_t>> ranges;
    size_t size = 0;
    {
        GstMappedBuffer mapped(buffer, GST_MAP_READ);
        if (!mapped)
            return GST_PAD_PROBE_OK;
        ranges = acceleratedEncoderFillerRanges(static_cast<EncoderId>(GPOINTER_TO_INT(encoderId)), mapped.span<uint8_t>());
        size = mapped.size();
    }
    if (ranges.isEmpty())
        return GST_PAD_PROBE_OK;

    size_t removed = 0;
    for (auto& [begin, end] : ranges)
        removed += end - begin;
    GRefPtr<GstBuffer> stripped = adoptGRef(gst_buffer_new_allocate(nullptr, size - removed, nullptr));
    {
        GstMappedBuffer source(buffer, GST_MAP_READ);
        GstMappedBuffer destination(stripped.get(), GST_MAP_WRITE);
        if (!source || !destination)
            return GST_PAD_PROBE_OK;
        auto input = source.span<uint8_t>();
        auto output = destination.mutableSpan<uint8_t>();
        size_t written = 0;
        size_t position = 0;
        for (auto& [begin, end] : ranges) {
            memcpySpan(output.subspan(written, begin - position), input.subspan(position, begin - position));
            written += begin - position;
            position = end;
        }
        memcpySpan(output.subspan(written), input.subspan(position));
    }
    gst_buffer_copy_into(stripped.get(), buffer, GST_BUFFER_COPY_METADATA, 0, -1);
    GST_LOG("Dropped %zu bytes of hardware encoder filler from a %zu byte frame", removed, size);
    GST_PAD_PROBE_INFO_DATA(info) = stripped.leakRef();
    gst_buffer_unref(buffer);
    return GST_PAD_PROBE_OK;
}

static void videoEncoderApplyBitrate(WebKitVideoEncoder* self, GstElement* encoder, bool isStreaming)
{
    auto* priv = self->priv;
    Locker locker { priv->bitrateLock };
    bool wasStreaming = std::exchange(priv->encoderIsStreaming, priv->encoderIsStreaming || isStreaming);
    if (isStreaming && !wasStreaming)
        priv->bitrateRestartTime = MonotonicTime::now();
    if (!encoder || priv->encoderId == None || priv->requestedBitrate == priv->appliedBitrate)
        return;
    if (encoderUsesVideoAcceleration(priv->encoderId) && wasStreaming) {
        auto now = MonotonicTime::now();
        if (!acceleratedEncoderShouldTakeBitrate(priv->requestedBitrate, priv->appliedBitrate, now - priv->bitrateRequestTime, now - priv->bitrateRestartTime))
            return;
        GST_INFO_OBJECT(self, "Moving the hardware encoder from %u to %u kbps, which restarts its GOP", priv->appliedBitrate, priv->requestedBitrate);
        priv->bitrateRestartTime = now;
    }
    priv->appliedBitrate = priv->requestedBitrate;
    auto definition = Encoders::definition(priv->encoderId);
    definition->setBitrate(G_OBJECT(encoder), definition->bitratePropertyName, priv->appliedBitrate);
}

static void videoEncoderSetBitrate(WebKitVideoEncoder* self, guint bitrate)
{
    auto* priv = self->priv;
    priv->bitrate = bitrate;
    {
        Locker locker { priv->bitrateLock };
        if (priv->requestedBitrate != bitrate) {
            priv->requestedBitrate = bitrate;
            priv->bitrateRequestTime = MonotonicTime::now();
        }
    }
    videoEncoderApplyBitrate(self, priv->encoder.get(), false);
}
"""


ENCODER_BITRATE_RESET = r"""    gst_bin_sync_children_states(bin);
    dumpBinToDotFile(bin, "configured-encoder"_s);
    {
        Locker locker { priv->bitrateLock };
        priv->appliedBitrate = 0;
        priv->encoderIsStreaming = false;
    }
    if (encoderUsesVideoAcceleration(encoderId)) {
        GRefPtr encoderSinkPad = adoptGRef(gst_element_get_static_pad(priv->encoder.get(), "sink"));
        gst_pad_add_probe(encoderSinkPad.get(), GST_PAD_PROBE_TYPE_BUFFER, reinterpret_cast<GstPadProbeCallback>(+[](GstPad* pad, GstPadProbeInfo*, WebKitVideoEncoder* self) -> GstPadProbeReturn {
            videoEncoderApplyBitrate(self, GST_ELEMENT_CAST(GST_PAD_PARENT(pad)), true);
            return GST_PAD_PROBE_OK;
        }), self, nullptr);
        GRefPtr encoderSrcPad = adoptGRef(gst_element_get_static_pad(priv->encoder.get(), "src"));
        gst_pad_add_probe(encoderSrcPad.get(), GST_PAD_PROBE_TYPE_BUFFER, videoEncoderDropFiller, GINT_TO_POINTER(encoderId), nullptr);
    }
    videoEncoderSetBitrate(self, priv->bitrate);
    return true;
}
"""


KEYFRAME_INTERVAL = r"""    case PROP_KEYFRAME_INTERVAL:
        if (priv->encoder) {
            auto encoder = Encoders::definition(priv->encoderId);
            auto interval = g_value_get_uint(value);
            if (auto* spec = g_object_class_find_property(G_OBJECT_GET_CLASS(priv->encoder.get()), encoder->keyframeIntervalPropertyName.characters())) {
                auto requested = interval;
                if (G_IS_PARAM_SPEC_UINT(spec))
                    interval = std::clamp(interval, G_PARAM_SPEC_UINT(spec)->minimum, G_PARAM_SPEC_UINT(spec)->maximum);
                else if (G_IS_PARAM_SPEC_INT(spec) && G_PARAM_SPEC_INT(spec)->maximum >= 0)
                    interval = std::min<guint>(interval, G_PARAM_SPEC_INT(spec)->maximum);
                if (interval != requested)
                    GST_DEBUG_OBJECT(self, "%s takes a keyframe interval of at most %u frames, not %u", encoder->name.characters(), interval, requested);
            }
            g_object_set(priv->encoder.get(), encoder->keyframeIntervalPropertyName.characters(), interval, nullptr);
        }
        break;
"""


def encoder_patch(text):
    text = replace_once(
        text,
        "#include <wtf/StdMap.h>\n",
        "#include <algorithm>\n#include <wtf/Lock.h>\n#include <wtf/MonotonicTime.h>\n#include <wtf/StdMap.h>\n",
    )
    text = replace_once(
        text,
        "    unsigned bitrate;\n    BitrateMode bitrateMode;\n",
        "    unsigned bitrate;\n"
        "    Lock bitrateLock;\n"
        "    unsigned requestedBitrate WTF_GUARDED_BY_LOCK(bitrateLock);\n"
        "    unsigned appliedBitrate WTF_GUARDED_BY_LOCK(bitrateLock);\n"
        "    MonotonicTime bitrateRequestTime WTF_GUARDED_BY_LOCK(bitrateLock);\n"
        "    MonotonicTime bitrateRestartTime WTF_GUARDED_BY_LOCK(bitrateLock);\n"
        "    bool encoderIsStreaming WTF_GUARDED_BY_LOCK(bitrateLock);\n"
        "    BitrateMode bitrateMode;\n",
    )
    text = replace_once(
        text,
        "static void videoEncoderSetBitrate(WebKitVideoEncoder* self, guint bitrate)\n"
        "{\n"
        "    auto* priv = self->priv;\n"
        "    priv->bitrate = bitrate;\n"
        "\n"
        "    if (priv->encoderId != None) {\n"
        "        auto encoder = Encoders::definition(priv->encoderId);\n"
        "        encoder->setBitrate(G_OBJECT(priv->encoder.get()), encoder->bitratePropertyName, priv->bitrate);\n"
        "    }\n"
        "}\n",
        ACCELERATED_BITRATE,
    )
    text = replace_once(
        text,
        "    gst_bin_sync_children_states(bin);\n"
        "    dumpBinToDotFile(bin, \"configured-encoder\"_s);\n"
        "    videoEncoderSetBitrate(self, priv->bitrate);\n"
        "    return true;\n"
        "}\n",
        ENCODER_BITRATE_RESET,
    )
    text = replace_once(
        text,
        "    case PROP_KEYFRAME_INTERVAL:\n"
        "        if (priv->encoder) {\n"
        "            auto encoder = Encoders::definition(priv->encoderId);\n"
        "            g_object_set(priv->encoder.get(), encoder->keyframeIntervalPropertyName.characters(), g_value_get_uint(value), nullptr);\n"
        "        }\n"
        "        break;\n",
        KEYFRAME_INTERVAL,
    )
    return replace_once(
        text,
        "    case REALTIME_LATENCY_MODE:\n"
        '        g_object_set(encoder, "target-usage", 1, nullptr);\n'
        "        break;\n"
        "    case QUALITY_LATENCY_MODE:\n"
        '        g_object_set(encoder, "target-usage", 7, nullptr);\n'
        '        gst_util_set_object_arg(G_OBJECT(encoder), "rate-control", "cqp");\n',
        "    case REALTIME_LATENCY_MODE:\n"
        '        g_object_set(encoder, "target-usage", 7, nullptr);\n'
        "        break;\n"
        "    case QUALITY_LATENCY_MODE:\n"
        '        g_object_set(encoder, "target-usage", 1, nullptr);\n',
    )


CODEC_UTILITIES = "Source/WebCore/platform/gstreamer/GStreamerCodecUtilities.cpp"


H264_LEVELS = r"""    if (!level.isEmpty()) {
        static constexpr std::array h264Levels { "1"_s, "1b"_s, "1.1"_s, "1.2"_s, "1.3"_s, "2"_s, "2.1"_s, "2.2"_s, "3"_s, "3.1"_s, "3.2"_s, "4"_s, "4.1"_s, "4.2"_s, "5"_s, "5.1"_s, "5.2"_s, "6"_s, "6.1"_s, "6.2"_s };
        auto highest = std::ranges::find_if(h264Levels, [&](auto candidate) { return level == candidate; });
        if (highest == h264Levels.end() || highest == h264Levels.begin())
            gst_caps_set_simple(outputCaps.get(), "level", G_TYPE_STRING, level.ascii().data(), nullptr);
        else {
            GValue levels = G_VALUE_INIT;
            g_value_init(&levels, GST_TYPE_LIST);
            for (auto candidate = highest;; --candidate) {
                GValue value = G_VALUE_INIT;
                g_value_init(&value, G_TYPE_STRING);
                g_value_set_string(&value, candidate->characters());
                gst_value_list_append_and_take_value(&levels, &value);
                if (candidate == h264Levels.begin())
                    break;
            }
            gst_caps_set_value(outputCaps.get(), "level", &levels);
            g_value_unset(&levels);
        }
    }
"""


def codec_utilities_patch(text):
    text = replace_once(text, "#include <gst/video/video.h>\n", "#include <algorithm>\n#include <array>\n#include <gst/video/video.h>\n")
    text = replace_once(
        text,
        "    if (!level.isEmpty())\n"
        "        gst_caps_set_simple(outputCaps.get(), \"level\", G_TYPE_STRING, level.ascii().data(), nullptr);\n",
        H264_LEVELS,
    )
    return replace_once(
        text,
        "    if (profile)\n"
        "        gst_caps_set_simple(outputCaps.get(), \"profile\", G_TYPE_STRING, profile.utf8(), nullptr);\n"
        "    if (!level.isEmpty()) {\n",
        "    if (profile == \"baseline\"_s) {\n"
        "        GValue profiles = G_VALUE_INIT;\n"
        "        g_value_init(&profiles, GST_TYPE_LIST);\n"
        "        for (const char* name : { \"constrained-baseline\", \"baseline\" }) {\n"
        "            GValue value = G_VALUE_INIT;\n"
        "            g_value_init(&value, G_TYPE_STRING);\n"
        "            g_value_set_string(&value, name);\n"
        "            gst_value_list_append_and_take_value(&profiles, &value);\n"
        "        }\n"
        "        gst_caps_set_value(outputCaps.get(), \"profile\", &profiles);\n"
        "        g_value_unset(&profiles);\n"
        "    } else if (profile)\n"
        "        gst_caps_set_simple(outputCaps.get(), \"profile\", G_TYPE_STRING, profile.utf8(), nullptr);\n"
        "    if (!level.isEmpty()) {\n",
    )


PATCHES = {
    ENCODER: encoder_patch,
    CODEC_UTILITIES: codec_utilities_patch,
}

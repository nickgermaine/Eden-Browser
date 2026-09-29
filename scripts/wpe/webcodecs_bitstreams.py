from wpe.patch_support import replace_once


VIDEO_DECODER = "Source/WebCore/platform/graphics/gstreamer/VideoDecoderGStreamer.cpp"


def video_decoder_patch(text):
    text = replace_once(
        text,
        "    if (codecName.startsWith(\"avc1\"_s)) {\n"
        "        m_inputCaps = adoptGRef(gst_caps_new_simple(\"video/x-h264\", \"stream-format\", G_TYPE_STRING, \"avc\", \"alignment\", G_TYPE_STRING, \"au\", nullptr));\n"
        "        if (auto codecData = wrapSpanData(config.description))\n"
        "            gst_caps_set_simple(m_inputCaps.get(), \"codec_data\", GST_TYPE_BUFFER, codecData.get(), nullptr);\n",
        "    if (codecName.startsWith(\"avc1\"_s)) {\n"
        "        auto codecData = wrapSpanData(config.description);\n"
        "        m_inputCaps = adoptGRef(gst_caps_new_simple(\"video/x-h264\", \"stream-format\", G_TYPE_STRING, codecData ? \"avc\" : \"byte-stream\", \"alignment\", G_TYPE_STRING, \"au\", nullptr));\n"
        "        if (codecData)\n"
        "            gst_caps_set_simple(m_inputCaps.get(), \"codec_data\", GST_TYPE_BUFFER, codecData.get(), nullptr);\n",
    )
    for tag in ("hvc1", "hev1"):
        text = replace_once(
            text,
            f"    }} else if (codecName.startsWith(\"{tag}\"_s)) {{\n"
            f"        m_inputCaps = adoptGRef(gst_caps_new_simple(\"video/x-h265\", \"stream-format\", G_TYPE_STRING, \"{tag}\", \"alignment\", G_TYPE_STRING, \"au\", nullptr));\n"
            "        if (auto codecData = wrapSpanData(config.description))\n"
            "            gst_caps_set_simple(m_inputCaps.get(), \"codec_data\", GST_TYPE_BUFFER, codecData.get(), nullptr);\n",
            f"    }} else if (codecName.startsWith(\"{tag}\"_s)) {{\n"
            "        auto codecData = wrapSpanData(config.description);\n"
            f"        m_inputCaps = adoptGRef(gst_caps_new_simple(\"video/x-h265\", \"stream-format\", G_TYPE_STRING, codecData ? \"{tag}\" : \"byte-stream\", \"alignment\", G_TYPE_STRING, \"au\", nullptr));\n"
            "        if (codecData)\n"
            "            gst_caps_set_simple(m_inputCaps.get(), \"codec_data\", GST_TYPE_BUFFER, codecData.get(), nullptr);\n",
        )
    return text


PATCHES = {
    VIDEO_DECODER: video_decoder_patch,
}

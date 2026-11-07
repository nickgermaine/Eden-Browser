from wpe.patch_support import replace_once


PLAYER = "Source/WebCore/platform/graphics/gstreamer/MediaPlayerPrivateGStreamer.cpp"
MEDIA_STREAM_SOURCE = "Source/WebCore/platform/mediastream/gstreamer/GStreamerMediaStreamSource.cpp"


def player_patch(text):
    text = replace_once(
        text,
        '            g_object_set(m_pipeline.get(), "audio-filter", scale, nullptr);\n'
        "    }\n"
        "\n"
        "    managePlayerSuspend();\n",
        '            g_object_set(m_pipeline.get(), "audio-filter", scale, nullptr);\n'
        "    }\n"
        "\n"
        "    if (!isMediaStream)\n"
        "        managePlayerSuspend();\n",
    )
    text = replace_once(
        text,
        "    if (isMediaStream)\n"
        '        flags = flags & ~getGstPlayFlag("buffering");\n',
        "    if (isMediaStream) {\n"
        '        flags = flags & ~getGstPlayFlag("buffering");\n'
        '        flags = flags & ~getGstPlayFlag("deinterlace");\n'
        "    }\n",
    )
    return replace_once(
        text,
        "    auto exit = makeScopeExit([this] {\n"
        "        if (!m_videoSink || isMediaStreamPlayer())\n"
        "            return;\n",
        "    auto exit = makeScopeExit([this] {\n"
        "        if (!m_videoSink || isMediaStreamPlayer())\n"
        "            return;\n"
        "#if ENABLE(MEDIA_STREAM)\n"
        "        if (m_streamPrivate)\n"
        "            return;\n"
        "#endif\n",
    )


def media_stream_source_patch(text):
    text = replace_once(
        text,
        "        {\n"
        "            Locker locker { m_eosLock };\n"
        "            m_eosPending = true;\n"
        "            m_eosCondition.waitFor(m_eosLock, 50_ms);\n"
        "        }\n",
        "        {\n"
        "            Locker locker { m_eosLock };\n"
        "            m_eosPending = true;\n"
        "        }\n",
    )
    text = replace_once(
        text,
        "        m_eosPending = false;\n"
        "        m_isEnded = true;\n"
        "        m_eosCondition.notifyAll();\n",
        "        m_eosPending = false;\n"
        "        m_isEnded = true;\n",
    )
    return replace_once(text, "    Condition m_eosCondition;\n", "")


PATCHES = {
    PLAYER: player_patch,
    MEDIA_STREAM_SOURCE: media_stream_source_patch,
}

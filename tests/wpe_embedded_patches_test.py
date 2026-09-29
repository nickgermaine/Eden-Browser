from pathlib import Path
import re
import sys
import tarfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import build_wpe
from build_wpe import EMBEDDED_CORRECTNESS_MODULES, EMBEDDED_PATHS, embedded_patches


def pinned_version(key="EDEN_WPE_PERFORMANCE_VERSION"):
    versions = (ROOT / "cmake" / "WpeVersion.cmake").read_text()
    return re.search(key + r' "([^"]+)"', versions)[1]


def originals(version):
    archive = ROOT / "third_party/wpe/packages" / f"wpewebkit-{version}.tar.xz"
    if not archive.is_file():
        raise unittest.SkipTest("The pinned WebKit archive is not present")
    with tarfile.open(archive) as package:
        return build_wpe.archive_originals(package, version)


def squeezed(text):
    return re.sub(r"\s+", "", text)


def section(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin + len(start))]


def module_paths(modules):
    return {path for module in modules for path in module.PATCHES}


def libwebrtc_only_paths():
    return module_paths(build_wpe.LIBWEBRTC_CORRECTNESS_MODULES) - module_paths(build_wpe.GSTREAMER_CORRECTNESS_MODULES)


class EmbeddedPatchTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.version = pinned_version()
        cls.originals = originals(cls.version)
        cls.patched = embedded_patches(cls.originals, cls.version)

    def test_every_pinned_file_is_patched_exactly_once(self):
        expected = [path for path in EMBEDDED_PATHS if path not in libwebrtc_only_paths()]
        self.assertEqual(sorted(self.patched), sorted(expected))
        for path in expected:
            self.assertNotEqual(self.patched[path], self.originals[path], path)
        self.assertEqual(self.patched, embedded_patches(self.originals, self.version))
        self.assertTrue(libwebrtc_only_paths() <= set(EMBEDDED_PATHS))

    def test_web_process_keeps_networking_for_gstreamer_webrtc(self):
        launcher = self.patched["Source/WebKit/UIProcess/Launcher/glib/BubblewrapLauncher.cpp"]
        start = launcher.index("static bool shouldUnshareNetwork")
        end = launcher.index("\n}", start)
        decision = launcher[start:end]
        self.assertIn("USE(GSTREAMER_WEBRTC)", decision)
        self.assertIn("ProcessLauncher::ProcessType::Web", decision)
        self.assertIn("EDEN_WEBKIT_ISOLATE_WEB_PROCESS_NETWORK", decision)
        self.assertEqual(decision.count("EDEN_WEBKIT_ISOLATE_WEB_PROCESS_NETWORK"), 1)
        self.assertLess(decision.index("USE(GSTREAMER_WEBRTC)"), decision.rindex("return true;"))
        self.assertEqual(launcher.count('sandboxArgs.append("--unshare-net");'), 1)
        self.assertEqual(
            self.originals["Source/WebKit/UIProcess/Launcher/glib/BubblewrapLauncher.cpp"].count("g_getenv"),
            launcher.count("g_getenv") - 1,
        )

    def test_each_sandbox_advertises_its_own_portal_instance(self):
        path = "Source/WebKit/UIProcess/Launcher/glib/BubblewrapLauncher.cpp"
        launcher = self.patched[path]
        info = launcher[launcher.index("static int createFlatpakInfo(const char* instanceID)"):]
        info = info[:info.index("\n}\n")]
        self.assertNotIn("static ", info.split("\n", 1)[1])
        self.assertIn('g_key_file_set_string(keyFile.get(), "Instance", "instance-id", instanceID);', info)
        self.assertIn('return createSealedMemFdWithData("flatpak-info", data.get(), size);', info)
        self.assertEqual(launcher.count("createFlatpakInfo(instanceID.get())"), 1)
        self.assertEqual(launcher.count("createBwrapInfo(launcher, sandboxArgs, instanceID.get())"), 1)

    def test_display_capture_keeps_the_requested_source_type(self):
        media = self.patched["Source/WebKit/UIProcess/API/glib/WebKitUserMediaPermissionRequest.cpp"]
        self.assertIn("eden-display-window-v1", media)
        self.assertIn("CaptureDevice::DeviceType::Window", media)
        self.assertIn("eligibleVideoDevices()", media)
        capture = self.patched[
            "Source/WebCore/platform/mediastream/gstreamer/GStreamerDisplayCaptureDeviceManager.cpp"
        ]
        self.assertIn("PipeWireOutputType::Window", capture)
        self.assertIn('CaptureDevice windowCaptureDevice(createVersion4UUIDString()', capture)
        request = self.patched["Source/WebKit/UIProcess/UserMediaPermissionRequestProxy.h"]
        self.assertIn("const Vector<WebCore::CaptureDevice>& eligibleVideoDevices() const", request)

    def test_media_stream_players_skip_suspension_while_their_pipeline_is_created(self):
        path = "Source/WebCore/platform/graphics/gstreamer/MediaPlayerPrivateGStreamer.cpp"
        player = self.patched[path]
        start = player.index("void MediaPlayerPrivateGStreamer::createGSTPlayBin(")
        creation = player[start:player.index("#if ENABLE(MEDIA_TELEMETRY)", start)]
        self.assertIn('bool isMediaStream = url.protocolIs("mediastream"_s);', creation)
        self.assertEqual(creation.count("managePlayerSuspend();"), 1)
        self.assertRegex(creation, r"if \(!isMediaStream\) \{\s*managePlayerSuspend\(\);\s*\}\s*$")
        self.assertEqual(player.count("managePlayerSuspend();"), self.originals[path].count("managePlayerSuspend();"))
        self.assertEqual(player.count("isMediaStreamPlayer() || isVisible == m_isVisibleInViewport"), 1)
        self.assertEqual(player.count("if (!m_pipeline || isMediaStreamPlayer())"), 1)

    def test_media_stream_players_render_every_live_frame(self):
        path = "Source/WebCore/platform/graphics/gstreamer/MediaPlayerPrivateGStreamer.cpp"
        player = self.patched[path]
        flags = section(player, "void MediaPlayerPrivateGStreamer::setPlaybackFlags(bool isMediaStream)", "\n    }\n\n")
        stream = section(flags, "if (isMediaStream) {", "}")
        self.assertIn('flags = flags & ~getGstPlayFlag("buffering");', stream)
        self.assertIn('flags = flags & ~getGstPlayFlag("deinterlace");', stream)
        self.assertEqual(player.count('getGstPlayFlag("deinterlace")'), 1)
        sink = section(player, "GstElement *MediaPlayerPrivateGStreamer::createVideoSink() {", '"qos", TRUE')
        guard = sink[sink.index("makeScopeExit"):]
        self.assertRegex(
            squeezed(guard),
            r"if\(!m_videoSink\|\|isMediaStreamPlayer\(\)\)\{?return;\}?#ifENABLE\(MEDIA_STREAM\)if\(m_streamPrivate\)\{?return;\}?#endif",
        )
        load = section(player, "void MediaPlayerPrivateGStreamer::load(MediaStreamPrivate &stream) {", "\n    }\n")
        self.assertLess(load.index("m_streamPrivate = &stream;"), load.index('load(makeString("mediastream://"_s'))
        creation = section(player, "void MediaPlayerPrivateGStreamer::load(const String &urlString) {", "\n    }\n")
        self.assertLess(creation.index("createGSTPlayBin(url);"), creation.index("setPlaybinURL(url);"))
        self.assertEqual(squeezed(player).count('g_object_set(sink,"qos",TRUE,nullptr);'), 1)

    def test_remote_ice_credentials_accept_the_libwebrtc_character_set(self):
        endpoint = self.patched["Source/WebCore/Modules/mediastream/gstreamer/GStreamerMediaEndpoint.cpp"]
        tolerated = endpoint[endpoint.index("static bool isTolerableIceCharacter"):]
        tolerated = tolerated[:tolerated.index("\n}")]
        self.assertEqual(sorted(re.findall(r"character == '(.)'", tolerated)), sorted("-=#_"))
        self.assertIn("minimumIceUsernameFragmentLength = 4;", endpoint)
        self.assertIn("minimumIcePasswordLength = 22;", endpoint)
        self.assertIn("maximumIceCredentialLength = 256;", endpoint)
        self.assertIn('extern "C" GType gst_webrtc_nice_get_type(void);', endpoint)
        self.assertIn("GST_WEBRTC_ICE_CLASS(klass)->set_remote_credentials = lenientNiceSetRemoteCredentials;", endpoint)
        creation = endpoint[endpoint.index("createLenientNiceAgent(makeString(binName"):]
        creation = creation[:creation.index("gst_element_factory_create(webrtcBinFactory.get(), binName")]
        self.assertIn("agent.leakRef()", creation)
        self.assertIn("gst_object_unref(transferredAgent)", creation)
        remote = endpoint[endpoint.index("if (descriptionType == DescriptionType::Remote) {"):]
        self.assertLess(
            remote.index("normalizeRemoteDescription(message.get(), iceCredentialSurrogates(m_webrtcBin.get()));"),
            remote.index("validateRTPHeaderExtensions"),
        )
        self.assertEqual(endpoint.count(', surrogates)) {'), 2)
        self.assertIn('fetchDescription(webrtcBin, "current-remote"_s, surrogates)', endpoint)
        self.assertIn('fetchDescription(webrtcBin, "pending-remote"_s, surrogates)', endpoint)
        self.assertNotIn('fetchDescription(webrtcBin, "current-local"_s, surrogates)', endpoint)
        self.assertIn('gstObjectHasProperty(GST_OBJECT_CAST(ice.get()), "ice-lite"_s)', endpoint)

    def test_legacy_data_channel_answers_are_normalized(self):
        endpoint = self.patched["Source/WebCore/Modules/mediastream/gstreamer/GStreamerMediaEndpoint.cpp"]
        normalization = endpoint[endpoint.index("static void normalizeLegacyDataChannelMedia"):]
        normalization = normalization[:normalization.index("\n}\n")]
        for expected in (
            'equal(unsafeSpan(protocol), "DTLS/SCTP"_s)',
            '"webrtc-datachannel"_s',
            'gst_sdp_media_set_proto(media, "UDP/DTLS/SCTP");',
            'gst_sdp_media_replace_format(media, 0, "webrtc-datachannel");',
            'gst_sdp_media_add_attribute(media, "sctp-port"',
            "gst_sdp_media_remove_attribute(media, *sctpMapIndex);",
        ):
            self.assertIn(expected, normalization)

    def test_answerer_senders_receive_outgoing_sources(self):
        media = "Source/WebCore/Modules/mediastream/gstreamer/"
        endpoint = self.patched[media + "GStreamerMediaEndpoint.cpp"]
        header = self.patched[media + "GStreamerMediaEndpoint.h"]
        connection = self.patched[media + "GStreamerPeerConnectionBackend.cpp"]
        sender = self.patched[media + "GStreamerRtpSenderBackend.cpp"]
        sender_header = self.patched[media + "GStreamerRtpSenderBackend.h"]
        for text in (endpoint, header, connection):
            self.assertNotIn("recycleTransceiverForSenderTrack", text)
        self.assertEqual(endpoint.count("attachPendingSenderSources();"), 2)
        initiate = endpoint[endpoint.index("void GStreamerMediaEndpoint::initiate("):]
        self.assertLess(initiate.index("attachPendingSenderSources();"), initiate.index("g_signal_emit_by_name"))
        self.assertIn('makeString("sink_"_s, *mLineIndex)', endpoint)
        self.assertIn("unlinkedSinkPadForTransceiver(m_webrtcBin.get(), transceiver.get())", endpoint)
        link = endpoint[endpoint.index("void GStreamerMediaEndpoint::linkOutgoingSources("):]
        link = link[:link.index("\n}\n")]
        self.assertIn("isSourceBoundToAnotherMLine(*source, i)", link)
        self.assertLess(link.index("if (!source->pad()) {"), link.index("source->configure("))
        self.assertIn("m_endpoint->attachSenderSource(senderBackend, *track);", connection)
        self.assertIn("m_endpoint->linkSenderSourceIfNegotiated(senderBackend);", connection)
        self.assertIn("peerConnectionBackend->attachSourceForReplacedTrack(*this, *newTrack);", sender)
        self.assertNotIn("notImplemented();\n}\n\nstd::unique_ptr<RTCDtlsTransportBackend>", sender)
        self.assertIn("m_mediaStreamId = mediaStreamIds.isEmpty() ? String() : mediaStreamIds[0];", sender)
        self.assertIn("String m_mediaStreamId;", sender_header)

    def test_meet_renegotiation_matches_libwebrtc(self):
        media = "Source/WebCore/Modules/mediastream/gstreamer/"
        endpoint = squeezed(self.patched[media + "GStreamerMediaEndpoint.cpp"])
        transport = section(endpoint, "staticboolisIceCandidateOnUnusableTransport(", "staticboolisSourceBoundToAnotherMLine(")
        for expected in ('equalLettersIgnoringASCIICase(tokens[6],"typ"_s)', 'equalLettersIgnoringASCIICase(tokens[2],"udp"_s)', 'equalLettersIgnoringASCIICase(tokens[2],"tcp"_s)'):
            self.assertIn(expected, transport)
        candidate = section(endpoint, "GStreamerMediaEndpoint::addIceCandidate(", "m_statsCollector->invalidateCache();")
        self.assertIn("if(isIceCandidateOnUnusableTransport(sdp)){", candidate)
        self.assertIn("task->run(descriptionsFromWebRTCBin(webrtcBin.get()));", candidate[candidate.index("isIceCandidateOnUnusableTransport(sdp)"):])
        self.assertLess(candidate.index("Expectline:candidate:<candidate-str>"), candidate.index("isIceCandidateOnUnusableTransport(sdp)"))
        self.assertIn(squeezed("if (bundlePolicy == GST_WEBRTC_BUNDLE_POLICY_BALANCED) { bundlePolicy = GST_WEBRTC_BUNDLE_POLICY_MAX_COMPAT; }"), endpoint)
        transceiver = squeezed(self.patched[media + "GStreamerRtpTransceiverBackend.cpp"])
        self.assertNotIn("toRtpCodecCapability", transceiver)
        self.assertNotIn("dynamicPayloadType", transceiver)
        preferences = section(transceiver, "ExceptionOr<void>GStreamerRtpTransceiverBackend::setCodecPreferences(", "applySourceSSRC(gstCodecs.get(),direction());")
        self.assertIn("gst_structure_copy(negotiatedCodecs[*match])", preferences)
        self.assertIn("if(isDerivedRedundancyCodec(codecName)){continue;}", preferences)
        self.assertNotIn("gst_structure_new(", preferences)
        self.assertIn("gst_codec_utils_h264_get_profile(bytes.data(),bytes.size())", transceiver)
        self.assertIn('equalLettersIgnoringASCIICase(codecName,"h264"_s)', transceiver)

    def test_replaced_tracks_keep_the_sender_msid(self):
        media = "Source/WebCore/Modules/mediastream/gstreamer/"
        sender = squeezed(self.patched[media + "GStreamerRtpSenderBackend.cpp"])
        self.assertNotIn("trackWasReplaced(", sender)
        self.assertIn("peerConnectionBackend->attachSourceForReplacedTrack(*this,*newTrack);", sender)
        source = squeezed(self.patched["Source/WebCore/platform/mediastream/gstreamer/RealtimeOutgoingMediaSourceGStreamer.cpp"])
        replace = section(source, "RealtimeOutgoingMediaSourceGStreamer::replaceTrack(", "if(!newTrack){")
        self.assertNotIn("codec-preferences", replace)
        self.assertNotIn("m_pendingTrackId", replace)

    def test_camera_nodes_carry_their_udev_entries_into_the_sandbox(self):
        path = "Source/WebKit/UIProcess/Launcher/glib/BubblewrapLauncher.cpp"
        for version, patched in ((self.version, self.patched), (pinned_version("EDEN_WPE_MINIMUM_VERSION"), None)):
            if patched is None:
                baseline = originals(version)
                patched = embedded_patches(baseline, version)
            launcher = squeezed(patched[path])
            v4l = section(launcher, "staticvoidbindV4l(", "staticboolenableDebugPermissions()")
            self.assertIn("if(!stat(path.data(),&deviceStatus)&&S_ISCHR(deviceStatus.st_mode)){", v4l)
            self.assertIn('makeString("/run/udev/data/c"_s,major(deviceStatus.st_rdev),\':\',minor(deviceStatus.st_rdev))', v4l)
            self.assertEqual(v4l.count('args.append(CString("--ro-bind-try"));'), 1)
            self.assertIn("#include<sys/sysmacros.h>", launcher)
            self.assertNotIn('"/run/udev","/run/udev"', launcher)

    def test_webrtc_nice_is_linked_without_its_include_paths(self):
        checks = self.patched["Source/cmake/GStreamerChecks.cmake"]
        self.assertIn("list(APPEND GSTREAMER_COMPONENTS webrtc webrtc_nice)", checks)
        finder = self.patched["Source/cmake/FindGStreamer.cmake"]
        self.assertIn("FIND_GSTREAMER_COMPONENT(GSTREAMER_WEBRTC_NICE gstreamer-webrtc-nice-1.0 gstwebrtcnice-1.0)", finder)
        webcore = self.patched["Source/WebCore/platform/GStreamer.cmake"]
        self.assertIn("${GSTREAMER_WEBRTC_NICE_LIBRARIES}", webcore)
        self.assertNotIn("${GSTREAMER_WEBRTC_NICE_INCLUDE_DIRS}", webcore)

    def test_display_devices_are_offered_before_the_portal_picks_a_source(self):
        mediastream = "Source/WebCore/platform/mediastream/"
        manager = squeezed(self.patched[mediastream + "DisplayCaptureManager.h"])
        self.assertIn(squeezed("virtual bool selectsSourceWhenCaptureStarts() const { return false; }"), manager)
        managers = squeezed(self.patched[mediastream + "gstreamer/GStreamerCaptureDeviceManager.h"])
        display = managers[managers.index("classGStreamerDisplayCaptureDeviceManagerfinal"):]
        self.assertIn(squeezed("bool selectsSourceWhenCaptureStarts() const final { return true; }"), display)
        self.assertEqual(managers.count("selectsSourceWhenCaptureStarts"), 1)
        center = squeezed(self.patched[mediastream + "RealtimeMediaSourceCenter.cpp"])
        listing = section(center, "voidRealtimeMediaSourceCenter::getDisplayMediaDevices(", "voidRealtimeMediaSourceCenter::")
        skip = squeezed(
            "if (displayCaptureFactory().displayCaptureDeviceManager().selectsSourceWhenCaptureStarts()) {"
            " displayDeviceInfo.append({ 1, device }); continue; }"
        )
        self.assertIn(skip, listing)
        self.assertLess(listing.index(skip), listing.index("createDisplayCaptureSource("))

    def test_capture_cancels_echo_from_the_communication_output(self):
        graphics = "Source/WebCore/platform/graphics/gstreamer/"
        mediastream = "Source/WebCore/platform/mediastream/gstreamer/"
        mixer = squeezed(self.patched[graphics + "GStreamerAudioMixer.cpp"])
        pipeline = section(mixer, "GStreamerAudioMixer::ensureMixerPipeline(", "voidGStreamerAudioMixer::teardownPipeline(")
        for expected in (
            'makeGStreamerElement("webrtcechoprobe"_s, makeString("webkitechoprobe-"_s, deviceId))',
            "gst_element_link_many(mixer, echoFormat, echoProbe, outputConvert, outputResample, audioSink, nullptr);",
            "gst_pipeline_use_clock(GST_PIPELINE(pipeline.get()), echoClock.get());",
            'gst_element_get_static_pad(echoProbe, "sink")',
            "shiftEchoReferenceTimestamp, GINT_TO_POINTER(static_cast<int>(EchoReferenceShift::ToClockTime))",
            'gst_element_get_static_pad(echoProbe, "src")',
            "shiftEchoReferenceTimestamp, GINT_TO_POINTER(static_cast<int>(EchoReferenceShift::ToRunningTime))",
        ):
            self.assertIn(squeezed(expected), pipeline)
        self.assertNotIn("gst_element_set_base_time", pipeline)
        shift = section(mixer, "staticGstPadProbeReturnshiftEchoReferenceTimestamp(", "GStreamerAudioMixer::MixerPipeline&")
        self.assertIn("gst_element_get_base_time(element.get())", shift)
        self.assertIn(squeezed("toClockTime ? GST_BUFFER_PTS(buffer) + baseTime : GST_BUFFER_PTS(buffer) - baseTime"), shift)
        teardown = section(mixer, "voidGStreamerAudioMixer::teardownPipeline(", "voidGStreamerAudioMixer::ensureState(")
        self.assertLess(teardown.index(squeezed('deviceId == "default"_s && locker->echoReferences')), teardown.index("unregisterPipeline"))
        self.assertLess(teardown.index("if(mp.mixer->numsinkpads){return;}"), teardown.index("unregisterPipeline"))
        self.assertNotIn("ASSERT(!mp.mixer->numsinkpads)", teardown)
        self.assertIn("#include<wtf/MainThread.h>", mixer)
        self.assertIn("if(isMainThread()){result->value->teardownTimer.stop();}", pipeline)
        acquire = section(mixer, "GStreamerAudioMixer::acquireEchoReference()", "voidGStreamerAudioMixer::releaseEchoReference()")
        self.assertIn("if(isMainThread()){mp.teardownTimer.stop();}", acquire)
        self.assertEqual(mixer.count("teardownTimer.stop();"), 2)
        self.assertEqual(mixer.count("if(isMainThread()){"), 2)
        self.assertIn("locker->echoReferences++;", acquire)
        sink = squeezed(self.patched[graphics + "WebKitAudioSinkGStreamer.cpp"])
        self.assertIn(squeezed("sink->priv->usage != WebKitAudioSinkUsage::Communication && (!enableAudioMixer"), sink)
        self.assertEqual(sink.count("mixer.registerProducer("), 2)
        self.assertEqual(sink.count("mixer.configureCommunicationSource("), 2)
        self.assertIn(squeezed("return createPlatformAudioSink(role, deviceId, device);"), sink)
        player = squeezed(self.patched[graphics + "MediaPlayerPrivateGStreamer.cpp"])
        creation = section(player, "voidMediaPlayerPrivateGStreamer::createGSTPlayBin(", 'g_object_set(m_pipeline.get(),"audio-sink"')
        self.assertIn(squeezed("hasAudioSourceClient = m_audioSourceProvider && m_audioSourceProvider->client();"), creation)
        self.assertLess(creation.index("createAudioSink(WebKitAudioSinkUsage::Communication)"), creation.rindex("createAudioSink();"))
        capturer = squeezed(self.patched[mediastream + "GStreamerAudioCapturer.cpp"])
        processor = section(capturer, "voidGStreamerAudioCapturer::addVoiceProcessor(", "GStreamerAudioCapturer::createConverter()")
        for expected in ('"probe", m_echoProbeName->data()', '"echo-cancel", TRUE', '"noise-suppression", !hasNoiseSuppressor', '"voice-detection", FALSE'):
            self.assertIn(squeezed(expected), processor)
        self.assertLess(processor.index("acquireEchoReference()"), processor.index('makeGStreamerElement("webrtcdsp"_s)'))
        converter = section(capturer, "GStreamerAudioCapturer::createConverter()", "boolGStreamerAudioCapturer::setSampleRate(")
        voice = "addVoiceProcessor(GST_BIN_CAST(bin),hasNoiseSuppressor);"
        self.assertLess(converter.index("hasNoiseSuppressor=true;"), converter.index(voice))
        self.assertLess(converter.index(voice), converter.index("gst_bin_find_unlinked_pad(GST_BIN_CAST(bin),GST_PAD_SRC)"))
        destructor = section(capturer, "GStreamerAudioCapturer::~GStreamerAudioCapturer()", "voidGStreamerAudioCapturer::setVoiceProcessing(")
        self.assertIn("releaseEchoReference();", destructor)
        source = squeezed(self.patched[mediastream + "GStreamerAudioCaptureSource.cpp"])
        start = section(source, "voidGStreamerAudioCaptureSource::startProducingData()", "voidGStreamerAudioCaptureSource::")
        self.assertLess(start.index("m_capturer->setVoiceProcessing(echoCancellation());"), start.index("m_capturer->setupPipeline();"))
        self.assertIn("settings.contains(RealtimeMediaSourceSettings::Flag::EchoCancellation)", source)

    def test_capture_gain_follows_chrome_agc2_on_libwebrtc(self):
        for patched in (self.patched, self.libwebrtc_patched()):
            capturer = squeezed(patched["Source/WebCore/platform/mediastream/gstreamer/GStreamerAudioCapturer.cpp"])
            includes = section(capturer, "#include<wtf/MediaTime.h>", "namespaceWebCore{")
            self.assertIn("#ifUSE(LIBWEBRTC)WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN#include<webrtc/api/audio/audio_processing.h>#include<webrtc/api/audio/builtin_audio_processing_builder.h>#include<webrtc/api/environment/environment_factory.h>WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END#endif", includes)
            controller = section(capturer, "#ifUSE(LIBWEBRTC)classCaptureGainController{", "GStreamerAudioCapturer::~GStreamerAudioCapturer()")
            order = [
                "config.gain_controller1.enabled=false;",
                "config.gain_controller2.enabled=true;",
                "config.gain_controller2.adaptive_digital.enabled=true;",
                "config.gain_controller2.input_volume_controller.enabled=false;",
                "config.gain_controller2.fixed_digital.gain_db=0;",
                "m_processor=webrtc::BuiltinAudioProcessingBuilder(config).Build(webrtc::CreateEnvironment());",
            ]
            positions = [controller.index(item) for item in order]
            self.assertEqual(positions, sorted(positions))
            self.assertNotIn("headroom_db", controller)
            self.assertNotIn("max_gain_db", controller)
            self.assertIn("GST_AUDIO_INFO_FORMAT(&info)!=GST_AUDIO_FORMAT_S16||GST_AUDIO_INFO_LAYOUT(&info)!=GST_AUDIO_LAYOUT_INTERLEAVED||GST_AUDIO_INFO_RATE(&info)%100", controller)
            self.assertIn("autosamples=unsafeMakeSpan(reinterpret_cast<int16_t*>(map.data),map.size/sizeof(int16_t));", controller)
            self.assertIn("for(;offset+frameSamples<=samples.size();offset+=frameSamples){autoframe=samples.subspan(offset,frameSamples);m_processor->ProcessStream(frame.data(),streamConfig,streamConfig,frame.data());}", controller)
            self.assertIn("buffer=gst_buffer_make_writable(buffer);GST_PAD_PROBE_INFO_DATA(info)=buffer;controller.process(buffer);", controller)
            processor = section(capturer, "voidGStreamerAudioCapturer::addVoiceProcessor(", "GStreamerAudioCapturer::createConverter()")
            self.assertIn("#ifUSE(LIBWEBRTC)boolusesDigitalGainControl=false;#elseboolusesDigitalGainControl=true;#endif", processor)
            self.assertIn('"gain-control",usesDigitalGainControl', processor)
            self.assertIn('gst_util_set_object_arg(G_OBJECT(processor),"noise-suppression-level","high");', processor)
            probe = "gst_pad_add_probe(processorOutput.get(),static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER|GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),applyCaptureGain,newCaptureGainController,"
            self.assertLess(processor.index(probe), processor.index("gst_pad_link(processorOutput.get(),chainInput.get());"))
            self.assertIn("deletestatic_cast<CaptureGainController*>(data);", processor)

    def test_baseline_release_receives_the_embedding_patches(self):
        version = pinned_version("EDEN_WPE_MINIMUM_VERSION")
        baseline = originals(version)
        patched = embedded_patches(baseline, version)
        active = module_paths(EMBEDDED_CORRECTNESS_MODULES)
        missing = [path for path in EMBEDDED_PATHS if path not in baseline]
        self.assertTrue(set(missing) <= module_paths(build_wpe.GSTREAMER_CORRECTNESS_MODULES + build_wpe.LIBWEBRTC_CORRECTNESS_MODULES), missing)
        for path in EMBEDDED_PATHS:
            if path in missing:
                self.assertNotIn(path, patched)
            elif path in libwebrtc_only_paths():
                self.assertNotIn(path, patched)
            elif path in active:
                self.assertEqual(patched[path], baseline[path], path)
            else:
                self.assertNotEqual(patched[path], baseline[path], path)
        settings = patched["Source/WebKit/UIProcess/glib/SystemSettingsManagerProxy.cpp"]
        self.assertEqual(settings.count("#include <glib.h>"), 1)
        self.assertIn("#include <WebCore/SystemSettings.h>\n#include <glib.h>", settings)
        self.assertIn("changedState.fontFamily = family;", settings)
        self.assertIn("changedState.fontSize = static_cast<float>(points * (xftDPI() / 1024.0) / 72.0);", settings)
        launcher = patched["Source/WebKit/UIProcess/Launcher/glib/BubblewrapLauncher.cpp"]
        self.assertEqual(launcher.count("createFlatpakInfo(instanceID.get())"), 1)
        self.assertEqual(patched, embedded_patches(baseline, version))

    def test_libwebrtc_backend_leaves_webrtcbin_files_pristine(self):
        webrtcbin_only = {
            path
            for module in build_wpe.GSTREAMER_CORRECTNESS_MODULES
            for path in module.PATCHES
        } - {path for module in build_wpe.LIBWEBRTC_CORRECTNESS_MODULES for path in module.PATCHES}
        self.assertIn("Source/WebCore/Modules/mediastream/gstreamer/GStreamerMediaEndpoint.cpp", webrtcbin_only)
        self.assertNotIn("Source/WebCore/platform/mediastream/gstreamer/GStreamerAudioCapturer.cpp", webrtcbin_only)
        with mock.patch.object(build_wpe, "EMBEDDED_CORRECTNESS_MODULES", build_wpe.LIBWEBRTC_CORRECTNESS_MODULES):
            patched = embedded_patches(self.originals, self.version)
        for path in webrtcbin_only:
            self.assertEqual(patched.get(path, self.originals[path]), self.originals[path], path)
        for module in build_wpe.LIBWEBRTC_CORRECTNESS_MODULES:
            for path in module.PATCHES:
                self.assertNotEqual(patched[path], self.originals[path], path)
                gstreamer_modules = [item for item in build_wpe.GSTREAMER_CORRECTNESS_MODULES if path in item.PATCHES]
                libwebrtc_modules = [item for item in build_wpe.LIBWEBRTC_CORRECTNESS_MODULES if path in item.PATCHES]
                if gstreamer_modules == libwebrtc_modules:
                    self.assertEqual(patched[path], self.patched[path], path)
        launcher = "Source/WebKit/UIProcess/Launcher/glib/BubblewrapLauncher.cpp"
        self.assertEqual(patched[launcher], self.patched[launcher])

    def libwebrtc_patched(self):
        with mock.patch.object(build_wpe, "EMBEDDED_CORRECTNESS_MODULES", build_wpe.LIBWEBRTC_CORRECTNESS_MODULES):
            return embedded_patches(self.originals, self.version)

    def test_libwebrtc_debug_logging_is_opt_in_per_process(self):
        path = "Source/WebCore/platform/mediastream/libwebrtc/LibWebRTCProvider.cpp"
        provider = squeezed(self.libwebrtc_patched()[path])
        self.assertIn("#include<webrtc/rtc_base/logging.h>", provider)
        severity = section(provider, "staticstd::optional<webrtc::LoggingSeverity>debugLoggingSeverity()", "staticvoidinitializePeerConnectionFactoryAndThreads(")
        self.assertIn('getenv("EDEN_WEBRTC_LOG")', severity)
        for name, level in (("verbose", "LS_VERBOSE"), ("info", "LS_INFO"), ("warning", "LS_WARNING"), ("error", "LS_ERROR")):
            self.assertIn(f'if(equalLettersIgnoringASCIICase(value,"{name}"_s)){{returnwebrtc::{level};}}', severity)
        self.assertTrue(severity.endswith("returnstd::nullopt;}"))
        startup = section(provider, "staticvoidinitializePeerConnectionFactoryAndThreads(", "factoryAndThreads.networkThread=")
        self.assertIn("if(autoseverity=debugLoggingSeverity()){webrtc::LoggingConfigconfig;config.set_min_severity(*severity);config.set_debug_severity(*severity);", startup)
        self.assertIn("config.set_log_to_stderr(true);if(!webrtc::InitializeLogging(WTF::move(config))){WTFLogAlways(", startup)
        self.assertEqual(provider.count("InitializeLogging("), 1)
        self.assertNotIn("LogToDebug(", provider)
        self.assertNotIn(path, self.patched)

    def test_libwebrtc_video_encoders_match_the_chrome_codec_stack(self):
        glue = "Source/WebCore/platform/mediastream/libwebrtc/gstreamer/"
        patched = self.libwebrtc_patched()
        source = squeezed(patched[glue + "GStreamerVideoEncoderFactory.cpp"])
        factory = section(source, "std::unique_ptr<webrtc::VideoEncoder>GStreamerVideoEncoderFactory::Create(", "GStreamerVideoEncoderFactory::GStreamerVideoEncoderFactory(")
        self.assertIn("returnstd::make_unique<webrtc::SimulcastEncoderAdapter>(environment,m_internalFactory.get(),nullptr,format);", factory)
        self.assertLess(factory.index("IsCodecInList(m_internalFactory->GetSupportedFormats())"), factory.index("SimulcastEncoderAdapter"))
        internal = section(source, "classLibWebRTCInternalVideoEncoderFactoryfinal", "std::unique_ptr<webrtc::VideoEncoder>GStreamerVideoEncoderFactory::Create(")
        formats = section(internal, "GetSupportedFormats()constfinal", "std::unique_ptr<webrtc::VideoEncoder>Create(")
        order = ["LibvpxVp8EncoderTemplateAdapter::SupportedFormats()", "m_h264Factory.GetSupportedFormats()", "LibaomAv1EncoderTemplateAdapter::SupportedFormats()", "webrtc::SupportedVP9Codecs(true)"]
        self.assertEqual([formats.index(item) for item in order], sorted(formats.index(item) for item in order))
        create = section(internal, "std::unique_ptr<webrtc::VideoEncoder>Create(", "private:")
        for expected in (
            "returnwebrtc::CreateVp8Encoder(environment);",
            "returnwebrtc::CreateVp9Encoder(environment,{webrtc::VP9Profile::kProfile0});",
            "returnwebrtc::CreateVp9Encoder(environment,{webrtc::VP9Profile::kProfile2});",
            "returnwebrtc::CreateLibaomAv1Encoder(environment);",
            "returnm_h264Factory.Create(environment,format);",
        ):
            self.assertIn(expected, create)
        self.assertNotIn("LibWebRTCGStreamerVideoEncoder>(formatName", source)
        h264 = section(source, "staticstd::vector<webrtc::SdpVideoFormat>h264EncoderFormats()", "classLibWebRTCGStreamerVideoEncoderfinal")
        self.assertEqual(re.findall(r'"([0-9a-f]{6})"', h264), ["42001f", "42e01f", "4d001f"])
        self.assertIn('{"packetization-mode","1"}', h264)
        self.assertNotIn('"packetization-mode","0"', h264)
        encoder = section(source, "classLibWebRTCGStreamerVideoEncoderfinal", "classGStreamerH264EncoderFactoryfinal")
        init = section(encoder, "int32_tInitEncode(", "int32_tRegisterEncodeCompleteCallback(")
        self.assertIn("if(codecSettings->numberOfSimulcastStreams>1){returnWEBRTC_VIDEO_CODEC_ERR_SIMULCAST_PARAMETERS_NOT_SUPPORTED;}", init)
        self.assertIn("m_bitRate=static_cast<uint64_t>(codecSettings->startBitrate)*1000;", init)
        self.assertIn("m_frameRate=std::clamp<double>(codecSettings->maxFramerate?codecSettings->maxFramerate:maximumDeclaredFrameRate,1,maximumDeclaredFrameRate);", init)
        self.assertNotIn("m_frameTimes.clear();", init)
        self.assertNotIn("m_rateFactor=", init)
        self.assertIn("staticconstexprdoublemaximumDeclaredFrameRate=30;", source)
        rates = section(encoder, "voidSetRates(", "VideoEncoder::EncoderInfoGetEncoderInfo()")
        self.assertIn("m_bitRate=parameters.bitrate.get_sum_bps();", rates)
        self.assertIn("m_inputFrameRate=m_framesSeen>=2?parameters.framerate_fps:0;", rates)
        self.assertNotIn("m_frameRate=", rates)
        self.assertLess(rates.index("updateRateFactor();"), rates.index("applyRates();"))
        measured = section(encoder, "doublemeasuredFrameRate()const", "boolupdateRateFactor()")
        self.assertIn("if(available<recentFrameIntervals){return0;}", measured)
        self.assertIn("autointerval=medianInterval(recentFrameIntervals);", measured)
        self.assertIn("if(available>=measuredFrameIntervals){interval=std::min(interval,medianInterval(measuredFrameIntervals));}", measured)
        self.assertIn("staticconstexprsize_trecentFrameIntervals=3;", source)
        factor = section(encoder, "boolupdateRateFactor()", "uint64_tencoderBitRate()const")
        self.assertIn("doubleframeRate=std::max(m_inputFrameRate,measuredFrameRate());", factor)
        note = section(encoder, "voidnoteFrameTime(", "doublemeasuredFrameRate()const")
        self.assertTrue(note.startswith("voidnoteFrameTime(int64_ttimestampUs){if(m_framesSeen<2){++m_framesSeen;}"))
        self.assertIn("doublefactor=std::clamp(m_frameRate/frameRate,minimumRateFactor,maximumRateFactor);", factor)
        self.assertIn("if(std::abs(factor/m_rateFactor-1)<rateFactorStep){returnfalse;}", factor)
        apply = section(encoder, "voidapplyRates()", "webrtc::SdpVideoFormatm_sdpVideoFormat;")
        self.assertIn("if(bitRate==m_encoderBitRate){return;}", apply)
        self.assertIn("m_internalEncoder->setRatesSync(bitRate,m_frameRate);", apply)
        self.assertNotIn("setRatesSync(m_bitRate", encoder)
        info = section(encoder, "VideoEncoder::EncoderInfoGetEncoderInfo()", "int32_tEncode(")
        self.assertIn("info.is_hardware_accelerated=m_isHardwareAccelerated;", info)
        self.assertIn("info.has_trusted_rate_controller=false;", info)
        self.assertIn("info.supports_native_handle=m_acceptsDMABuf;", info)
        encode = section(encoder, "int32_tEncode(", "private:")
        self.assertIn("boolencoderScalesFrame=m_acceptsDMABuf&&frame.video_frame_buffer()&&frame.video_frame_buffer()->type()==webrtc::VideoFrameBuffer::Type::kNative;", encode)
        self.assertLess(encode.index("if(!encoderScalesFrame&&frameSize!=m_size){"), encode.index("createInternalEncoder()"))
        self.assertIn("autosample=convertLibWebRTCVideoFrameToGStreamerSample(frame,m_acceptsDMABuf);", encode)
        self.assertNotIn("options.rotation", encode)
        self.assertLess(encode.index("m_pendingFrames.append("), encode.index("encodeSync("))
        self.assertIn("noteFrameTime(frame.timestamp_us());if(updateRateFactor()){applyRates();}", encode)
        self.assertLess(encode.index("noteFrameTime("), encode.index("encodeSync("))
        create_encoder = section(encoder, "int32_tcreateInternalEncoder()", "voidnoteFrameTime(")
        self.assertIn("m_internalEncoder->setKeyFrameIntervalSync(h264KeyFrameInterval);", create_encoder)
        self.assertIn("config.bitRate=encoderBitRate();", create_encoder)
        self.assertIn("m_encoderBitRate=0;applyRates();", create_encoder)
        self.assertLess(create_encoder.index("m_isHardwareAccelerated=m_internalEncoder->isHardwareAccelerated();"), create_encoder.index("m_acceptsDMABuf=m_internalEncoder->acceptsDMABuf();"))
        self.assertIn("staticconstexprunsignedh264KeyFrameInterval=3000;", source)
        notify = section(encoder, "voidnotifyEncodedFrame(", "webrtc::SdpVideoFormatm_sdpVideoFormat;")
        self.assertIn("GStreamerEncodedImageBuffer::create(WTF::move(frame.data))", notify)
        self.assertIn("if(!rtpTimestamp){", notify)
        self.assertIn("m_codecInfo.codecSpecific.H264.idr_frame=frame.isKeyFrame;", notify)
        buffer = section(source, "classGStreamerEncodedImageBuffer:publicwebrtc::EncodedImageBufferInterface", "staticconstexprunsignedh264KeyFrameInterval")
        self.assertIn("Vector<uint8_t>m_data;", buffer)
        self.assertNotIn("std::span", buffer)
        header = squeezed(patched[glue + "GStreamerVideoEncoderFactory.h"])
        self.assertIn("GStreamerVideoEncoderFactory(boolisSupportingVP9Profile0,boolisSupportingVP9Profile2,boolisSupportingAV1);", header)
        self.assertIn("std::unique_ptr<webrtc::VideoEncoderFactory>m_internalFactory;", header)
        provider = squeezed(patched[glue + "LibWebRTCProviderGStreamer.cpp"])
        self.assertEqual(provider.count("isSupportingAV1()"), 2)

    def test_libwebrtc_video_decoders_use_libvpx_dav1d_and_fail_safely(self):
        glue = "Source/WebCore/platform/mediastream/libwebrtc/gstreamer/"
        patched = self.libwebrtc_patched()
        source = squeezed(patched[glue + "GStreamerVideoDecoderFactory.cpp"])
        create = section(source, "std::unique_ptr<webrtc::VideoDecoder>GStreamerVideoDecoderFactory::Create(", "GStreamerVideoDecoderFactory::GStreamerVideoDecoderFactory(")
        self.assertIn("returnstd::make_unique<webrtc::LibvpxVp8Decoder>(environment);", create)
        self.assertIn("returnwebrtc::VP9Decoder::Create();", create)
        self.assertIn("returnwebrtc::CreateDav1dDecoder(environment);", create)
        formats = section(source, "std::vector<webrtc::SdpVideoFormat>GStreamerVideoDecoderFactory::GetSupportedFormats()", "#undefGST_CAT_DEFAULT")
        self.assertIn("formats{webrtc::SdpVideoFormat::VP8()};", formats)
        self.assertIn("Dav1dDecoderTemplateAdapter::SupportedFormats()", formats)
        self.assertNotIn("VP8Decoder().addDecoderIfSupported", formats)
        configure = section(source, "boolConfigure(constwebrtc::VideoDecoder::Settings&codecSettings)override", "int32_tRegisterDecodeCompleteCallback(")
        self.assertNotIn("WEBRTC_VIDEO_CODEC_ERROR", configure)
        self.assertEqual(configure.count("returnfalse;"), 2)
        decode = section(source, "int32_tDecode(constwebrtc::EncodedImage&inputImage,int64_t)override", "virtualvoidupdateCapsFromImageSize(")
        self.assertIn("200*GST_MSECOND", decode)
        self.assertLess(decode.index("if(!frame){"), decode.index("m_imageReadyCb->Decoded(*frame);"))
        self.assertIn("std::atomic<bool>m_needsKeyframe{true};", source)
        self.assertIn("webrtc::DecodedImageCallback*m_imageReadyCb{nullptr};", source)

    def test_libwebrtc_frames_wrap_exact_plane_memory(self):
        glue = "Source/WebCore/platform/mediastream/libwebrtc/gstreamer/"
        patched = self.libwebrtc_patched()
        source = squeezed(patched[glue + "GStreamerVideoFrameLibWebRTC.cpp"])
        wrap = section(source, "GRefPtr<GstSample>convertLibWebRTCVideoFrameToGStreamerSample(", "std::optional<webrtc::VideoFrame>convertGStreamerSampleToLibWebRTCVideoFrame(")
        self.assertIn("if(!isTightlyPacked(*i420Buffer)){i420Buffer=webrtc::I420Buffer::Copy(*i420Buffer);}", wrap)
        self.assertIn("gsizesize=offsets[2]+static_cast<gsize>(strides[2])*chromaHeight;", wrap)
        self.assertIn("data,size,0,size,holder", wrap)
        self.assertNotIn("info.size", wrap)
        self.assertIn("static_cast<webrtc::I420BufferInterface*>(holder)->Release();", wrap)
        self.assertNotIn("reinterpret_cast<webrtc::I420Buffer*>", source)
        convert = section(source, "std::optional<webrtc::VideoFrame>convertGStreamerSampleToLibWebRTCVideoFrame(", "webrtc::scoped_refptr<webrtc::VideoFrameBuffer>GStreamerVideoFrameLibWebRTC::create(")
        self.assertIn("GST_TIME_AS_USECONDS(pts)", convert)
        self.assertNotIn("GST_USECOND", convert.replace("GST_TIME_AS_USECONDS", ""))
        self.assertNotIn("ASSERT_NOT_REACHED", source)
        to_i420 = section(source, "webrtc::scoped_refptr<webrtc::I420BufferInterface>GStreamerVideoFrameLibWebRTC::ToI420()", "#undefGST_CAT_DEFAULT")
        self.assertIn("webrtc::I420Buffer::SetBlack(blackBuffer.get());", to_i420)
        black = squeezed(patched[glue + "RealtimeOutgoingVideoSourceLibWebRTC.cpp"])
        self.assertIn("webrtc::I420Buffer::SetBlack(buffer.get());", black)
        self.assertNotIn("GST_VIDEO_FORMAT_RGB", black)
        incoming = squeezed(patched[glue + "RealtimeIncomingVideoSourceLibWebRTC.cpp"])
        self.assertLess(incoming.index("if(!sample)[[unlikely]]{"), incoming.index("videoFrameAvailable("))

    def test_libwebrtc_audio_module_waits_for_in_flight_playout(self):
        base = "Source/WebCore/platform/mediastream/libwebrtc/"
        patched = self.libwebrtc_patched()
        source = squeezed(patched[base + "LibWebRTCAudioModule.cpp"])
        register = section(source, "int32_tLibWebRTCAudioModule::RegisterAudioCallback(", "int32_tLibWebRTCAudioModule::StartPlayout()")
        self.assertLess(register.index("Lockerlocker{m_audioTransportLock};"), register.index("m_audioTransport=audioTransport;"))
        start = section(source, "int32_tLibWebRTCAudioModule::StartPlayout()", "int32_tLibWebRTCAudioModule::StopPlayout()")
        self.assertIn("if(m_isPlaying.exchange(true)){return0;}", start)
        stop = section(source, "int32_tLibWebRTCAudioModule::StopPlayout()", "voidLibWebRTCAudioModule::logTimerFired()")
        self.assertLess(stop.index("m_isPlaying=false;"), stop.index("m_queue->dispatchSync([]{});"))
        poll = section(source, "voidLibWebRTCAudioModule::pollAudioData()", "voidLibWebRTCAudioModule::pollFromSource()")
        self.assertLess(poll.index("Lockerlocker{m_audioTransportLock};"), poll.index("ForbidMallocUseForCurrentThreadScopeforbidMallocUse;"))
        header = squeezed(patched[base + "LibWebRTCAudioModule.h"])
        self.assertIn("std::atomic<bool>m_isPlaying{false};", header)
        self.assertIn("webrtc::AudioTransport*m_audioTransportWTF_GUARDED_BY_LOCK(m_audioTransportLock){nullptr};", header)
        self.assertIn("voidpollFromSource()WTF_REQUIRES_LOCK(m_audioTransportLock);", header)

    def test_libwebrtc_backend_requires_and_builds_system_av1(self):
        patched = self.libwebrtc_patched()
        checks = patched["Source/cmake/GStreamerChecks.cmake"]
        branch = checks[checks.index("SET_AND_EXPOSE_TO_BUILD(USE_LIBWEBRTC TRUE)"):]
        for expected in ("pkg_check_modules(LIBWEBRTC_AOM REQUIRED IMPORTED_TARGET GLOBAL aom>=3.8.0)", "pkg_check_modules(LIBWEBRTC_DAV1D REQUIRED IMPORTED_TARGET GLOBAL dav1d>=1.2.0)", "SET_AND_EXPOSE_TO_BUILD(USE_LIBWEBRTC_AV1 TRUE)"):
            self.assertIn(expected, branch)
        self.assertNotIn("USE_LIBWEBRTC_AV1", self.patched.get("Source/cmake/GStreamerChecks.cmake", self.originals["Source/cmake/GStreamerChecks.cmake"]))
        encoder = squeezed(patched["Source/WebCore/platform/graphics/gstreamer/VideoEncoderGStreamer.cpp"])
        self.assertIn('g_object_set(m_harness->element(),"keyframe-interval",interval,nullptr);', encoder)
        self.assertIn('g_strstr_len(klass,-1,"Hardware")', encoder)
        self.assertIn("voidGStreamerVideoEncoder::setRatesSync(uint64_tbitRate,doubleframeRate){m_internalEncoder->setRates(bitRate,frameRate);}", encoder)

    def test_libwebrtc_encodings_carry_the_scalability_mode(self):
        patched = self.libwebrtc_patched()
        idl = squeezed(patched["Source/WebCore/Modules/mediastream/RTCRtpEncodingParameters.idl"])
        self.assertIn("doublescaleResolutionDownBy;DOMStringscalabilityMode;", idl)
        header = squeezed(patched["Source/WebCore/Modules/mediastream/RTCRtpEncodingParameters.h"])
        self.assertIn("StringscalabilityMode;", header)
        utilities = squeezed(patched["Source/WebCore/Modules/mediastream/libwebrtc/LibWebRTCUtils.cpp"])
        self.assertIn("if(rtcParameters.scalability_mode&&!isAudio){parameters.scalabilityMode=fromStdString(*rtcParameters.scalability_mode);}", utilities)
        self.assertIn("if(!parameters.scalabilityMode.isNull()){rtcParameters.scalability_mode=parameters.scalabilityMode.utf8().toStdString();}", utilities)
        for path in ("Source/WebCore/Modules/mediastream/RTCRtpEncodingParameters.idl", "Source/WebCore/Modules/mediastream/RTCRtpEncodingParameters.h"):
            self.assertNotIn("scalabilityMode", self.patched.get(path, self.originals[path]))

    def test_captured_frames_always_carry_a_video_meta(self):
        for patched in (self.patched, self.libwebrtc_patched()):
            capturer = squeezed(patched["Source/WebCore/platform/mediastream/gstreamer/GStreamerVideoCapturer.cpp"])
            helper = section(capturer, "staticGRefPtr<GstSample>sampleWithVideoMeta(", "voidGStreamerVideoCapturer::handleSample(")
            self.assertIn("if(!buffer||!caps||gst_buffer_get_video_meta(buffer)){returnWTF::move(sample);}", helper)
            self.assertIn("gst_video_info_dma_drm_from_caps(&drmInfo,caps)", helper)
            self.assertIn("gst_video_info_dma_drm_to_video_info(&drmInfo,&info)", helper)
            self.assertIn("gst_buffer_make_writable(gst_buffer_ref(buffer))", helper)
            self.assertIn("GST_VIDEO_INFO_N_PLANES(&info),info.offset,info.stride)", helper)
            reconfigure = section(capturer, "voidGStreamerVideoCapturer::reconfigure()", "#undefGST_CAT_DEFAULT")
            self.assertNotIn("MimeTypeSelector", reconfigure)
            self.assertIn("automode=bestCaptureDeviceMode(deviceCaps.get(),requestedWidth,requestedHeight,requestedFrameRate);", reconfigure)
            converter = section(capturer, "GStreamerVideoCapturer::createConverter()", "gst_bin_add_many(GST_BIN_CAST(bin),videoConvert.get(),videorate,nullptr);")
            self.assertIn('g_object_set(videorate,"drop-only",TRUE,"average-period",UINT64_C(1),nullptr);', converter)
            self.assertNotIn("gst_check_version(1,28,0)", converter)
            handle = section(capturer, "voidGStreamerVideoCapturer::handleSample(", "voidGStreamerVideoCapturer::setSinkVideoFrameCallback(")
            self.assertLess(handle.index("sampleWithVideoMeta(WTF::move(capturedSample))"), handle.index("VideoFrameGStreamer::create("))
            setup = section(capturer, "voidGStreamerVideoCapturer::setupPipeline()", "GstElement*GStreamerVideoCapturer::createConverter()")
            self.assertLess(setup.index("GStreamerCapturer::setupPipeline();"), setup.index('g_object_set(m_sink.get(),"sync",FALSE,nullptr);'))
            self.assertEqual(capturer.count('"sync",FALSE'), 1)

    def test_camera_modes_follow_the_request_and_frames_use_webcodecs_formats(self):
        for patched in (self.patched, self.libwebrtc_patched()):
            capturer = squeezed(patched["Source/WebCore/platform/mediastream/gstreamer/GStreamerVideoCapturer.cpp"])
            self.assertIn('GStreamerCapturer(WTF::move(device),adoptGRef(gst_caps_from_string("video/x-raw,format=(string)I420,pixel-aspect-ratio=(fraction)1/1")))', capturer)
            select = section(capturer, "staticstd::optional<CaptureDeviceMode>bestCaptureDeviceMode(", "voidGStreamerVideoCapturer::reconfigure()")
            self.assertIn("boolprefersCompressed=requestedWidth>maximumPreferredUncompressedWidth||requestedHeight>maximumPreferredUncompressedHeight;", select)
            self.assertIn("returnstd::tuple{!covers,!reachesFrameRate,!matchesAspectRatio,covers?area:-area,isCompressed!=prefersCompressed,-mode.frameRate};", select)
            self.assertIn("!gst_caps_features_is_equal(features,GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY)", select)
            self.assertIn("if(!best||rank(mode)<rank(*best)){best=WTF::move(mode);}", select)
            self.assertNotIn("returnFALSE", select)
            reconfigure = section(capturer, "voidGStreamerVideoCapturer::reconfigure()", "#undefGST_CAT_DEFAULT")
            self.assertLess(reconfigure.index('g_object_set(m_aspectRatioCrop.get(),"aspect-ratio",requestedWidth,requestedHeight,nullptr);'), reconfigure.index("automode=bestCaptureDeviceMode("))
            self.assertIn('gst_caps_set_simple(caps.get(),"format",G_TYPE_STRING,mode->format.ascii().data(),nullptr);', reconfigure)
            converter = section(capturer, "GStreamerVideoCapturer::createConverter()", "boolGStreamerVideoCapturer::setSize(")
            order = [
                'm_aspectRatioCrop=gst_element_factory_make("aspectratiocrop",nullptr);',
                "gst_element_link(m_aspectRatioCrop.get(),videoConvert.get());",
                "decodedVideoSink=m_aspectRatioCrop;",
                'GRefPtrsinkPad=adoptGRef(gst_element_get_static_pad(decodedVideoSink.get(),"sink"));',
                'g_signal_connect_data(decodebin,"pad-added"',
            ]
            positions = [converter.index(item) for item in order]
            self.assertEqual(positions, sorted(positions))
            teardown = section(capturer, "voidGStreamerVideoCapturer::tearDown(booldisconnectSignals)", "voidGStreamerVideoCapturer::setupPipeline()")
            self.assertIn("if(disconnectSignals){m_videoSrcMIMETypeFilter=nullptr;m_aspectRatioCrop=nullptr;}", teardown)
            header = squeezed(patched["Source/WebCore/platform/mediastream/gstreamer/GStreamerVideoCapturer.h"])
            self.assertIn("GRefPtr<GstElement>m_aspectRatioCrop;};", header)

    def test_camera_resizes_report_the_delivered_size(self):
        for patched in (self.patched, self.libwebrtc_patched()):
            base = squeezed(patched["Source/WebCore/platform/mediastream/RealtimeVideoCaptureSource.cpp"])
            resize = section(base, "voidRealtimeVideoCaptureSource::setSizeFrameRateAndZoom(", "voidRealtimeVideoCaptureSource::setSizeFrameRateAndZoomForPhoto(")
            self.assertIn("autonewSize=match->requestedSize.isEmpty()?match->encodingPreset->size():match->requestedSize;", resize)
            self.assertLess(resize.index("autonewSize="), resize.index("setSize(newSize);"))
            camera = squeezed(patched["Source/WebCore/platform/mediastream/gstreamer/GStreamerVideoCaptureSource.cpp"])
            override = section(camera, "voidGStreamerVideoCaptureSource::setSizeFrameRateAndZoom(", "voidGStreamerVideoCaptureSource::applyFrameRateAndZoomWithPreset(")
            self.assertEqual(override, "voidGStreamerVideoCaptureSource::setSizeFrameRateAndZoom(constVideoPresetConstraints&constraints){RealtimeVideoCaptureSource::setSizeFrameRateAndZoom(constraints);}")
            mock = squeezed(patched["Source/WebCore/platform/mediastream/gstreamer/MockRealtimeVideoSourceGStreamer.cpp"])
            self.assertIn("MockRealtimeVideoSource::setSizeFrameRateAndZoom(constraints);if(!size().isEmpty()){m_capturer->setSize(size());}", mock)
            self.assertNotIn("*constraints.width", mock)

    def test_display_frames_are_cropped_to_the_shared_surface(self):
        for patched in (self.patched, self.libwebrtc_patched()):
            capturer = squeezed(patched["Source/WebCore/platform/mediastream/gstreamer/GStreamerVideoCapturer.cpp"])
            self.assertIn('#include"GStreamerVideoFrameConverter.h"#include"VideoFrameGStreamer.h"', capturer)
            apply = section(capturer, "staticGRefPtr<GstSample>sampleWithCropApplied(GRefPtr<GstSample>&&sample)", "voidGStreamerVideoCapturer::updateDisplayFrameSize(")
            order = [
                "auto*crop=buffer&&caps?gst_buffer_get_video_crop_meta(buffer):nullptr;if(!crop){returnWTF::move(sample);}",
                "isLinear=!drmInfo.drm_modifier;",
                "if(!crop->width||!crop->height||crop->x+crop->width>frameWidth||crop->y+crop->height>frameHeight){",
                "alignmentX=std::max(alignmentX,1u<<GST_VIDEO_FORMAT_INFO_W_SUB(info.finfo,component));",
                "unsignedx=crop->x-crop->x%alignmentX;",
                "if(isLinear&&!GST_VIDEO_FORMAT_INFO_IS_TILED(info.finfo)){returnsampleWithCropRectangle(sample,info,buffer,x,y,width,height);}",
                '"format",G_TYPE_STRING,"BGRA"',
                "GStreamerVideoFrameConverter::singleton().convert(sample,systemMemoryCaps);",
                "returnsampleWithCropRectangle(converted,convertedInfo,buffer,crop->x,crop->y,crop->width,crop->height);",
            ]
            self.assertEqual([apply.index(item) for item in order], sorted(apply.index(item) for item in order))
            rectangle = section(capturer, "staticGRefPtr<GstSample>sampleWithCropRectangle(", "staticGRefPtr<GstSample>sampleWithCropApplied(")
            self.assertLess(rectangle.index("gst_buffer_make_writable(gst_buffer_ref(sourceBuffer))"), rectangle.index("gst_buffer_add_parent_buffer_meta(writableBuffer.get(),sourceBuffer);"))
            meta = section(capturer, "staticGRefPtr<GstSample>sampleWithVideoMeta(", "staticIntSizesampleResolution(")
            self.assertLess(meta.index("gst_buffer_make_writable(gst_buffer_ref(buffer))"), meta.index("gst_buffer_add_parent_buffer_meta(writableBuffer.get(),buffer);"))
            for expected in (
                "GST_BUFFER_PTS(writableBuffer.get())=GST_BUFFER_PTS(timingSource);",
                "gst_video_format_info_component(info.finfo,plane,components);",
                "offsets[plane]+=static_cast<gsize>(componentY)*strides[plane]+static_cast<gsize>(componentX)*GST_VIDEO_FORMAT_INFO_PSTRIDE(info.finfo,components[0]);}WTF_ALLOW_UNSAFE_BUFFER_USAGE_END;",
                "WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN;for(unsignedplane=0;plane<planes;++plane){",
                "gst_buffer_remove_meta(writableBuffer.get(),GST_META_CAST(videoMeta));",
                "gst_buffer_remove_meta(writableBuffer.get(),GST_META_CAST(cropMeta));",
                "gst_buffer_add_video_meta_full(writableBuffer.get(),flags,GST_VIDEO_INFO_FORMAT(&info),width,height,planes,offsets,strides);",
                '"width",G_TYPE_INT,static_cast<int>(width),"height",G_TYPE_INT,static_cast<int>(height)',
            ):
                self.assertIn(expected, rectangle)
            notify = section(capturer, "voidGStreamerVideoCapturer::updateDisplayFrameSize(", "voidGStreamerVideoCapturer::handleSample(")
            self.assertIn("if(frameSize.isEmpty()||(frameSize==m_displayFrameSize&&sourceSize==m_displaySourceSize)){return;}", notify)
            self.assertLess(notify.index("callOnMainThread("), notify.index("observer.sourceCapsChanged(caps.get());"))
            handle = section(capturer, "voidGStreamerVideoCapturer::handleSample(GRefPtr<GstSample>&&capturedSample)", "voidGStreamerVideoCapturer::setSinkVideoFrameCallback(")
            order = [
                "autosourceSize=isCapturingDisplay()?sampleResolution(capturedSample):IntSize{};",
                "autosample=sampleWithCropApplied(sampleWithVideoMeta(WTF::move(capturedSample)));",
                "if(isCapturingDisplay()){updateDisplayFrameSize(sourceSize,sample);}",
                "autosize=isCapturingDisplay()?sampleResolution(sample):this->size();",
                "VideoFrameGStreamer::create(",
            ]
            self.assertEqual([handle.index(item) for item in order], sorted(handle.index(item) for item in order))
            header = squeezed(patched["Source/WebCore/platform/mediastream/gstreamer/GStreamerVideoCapturer.h"])
            self.assertIn("voidupdateDisplayFrameSize(constIntSize&,constGRefPtr<GstSample>&);", header)
            self.assertIn("IntSizem_size;IntSizem_displaySourceSize;IntSizem_displayFrameSize;RefPtr<PipeWireFrameGate>m_frameGate;GRefPtr<GstElement>m_aspectRatioCrop;};", header)
            gate = section(header, "classPipeWireFrameGatefinal:publicThreadSafeRefCounted<PipeWireFrameGate>{", "classGStreamerVideoCapturerfinal")
            self.assertIn("if(m_closeLock.tryLock()){Lockerlocker{AdoptLock,m_closeLock};buffer=nullptr;return;}Lockerlocker{m_pendingLock};m_pending.append(WTF::move(buffer));", gate)
            self.assertIn("Lockerlocker{m_closeLock};closeSource();", gate)
            self.assertLess(gate.index("closeSource();"), gate.index("pending=std::exchange(m_pending,{});"))
            wrap = section(capturer, "staticGRefPtr<GstSample>sampleReleasedThroughGate(", "staticGRefPtr<GstSample>sampleWithVideoMeta(")
            self.assertIn("gst_buffer_iterate_meta_filtered(gated.get(),&state,GST_PARENT_BUFFER_META_API_TYPE)", wrap)
            self.assertIn("gst_buffer_remove_meta(gated.get(),parent);", wrap)
            self.assertIn("newGatedPipeWireBuffer{gate,GRefPtr<GstBuffer>(buffer)}", wrap)
            self.assertIn("gatedBuffer->gate->release(WTF::move(gatedBuffer->buffer));", wrap)
            handle = section(capturer, "voidGStreamerVideoCapturer::handleSample(GRefPtr<GstSample>&&capturedSample)", "voidGStreamerVideoCapturer::setSinkVideoFrameCallback(")
            self.assertLess(handle.index("if(m_frameGate){capturedSample=sampleReleasedThroughGate(WTF::move(capturedSample),*m_frameGate);}"), handle.index("sampleWithCropApplied(sampleWithVideoMeta("))
            self.assertIn("GStreamerVideoCapturer::GStreamerVideoCapturer(constPipeWireCaptureDevice&device):GStreamerCapturer(device),m_frameGate(PipeWireFrameGate::create())", capturer)
            teardown = section(capturer, "voidGStreamerVideoCapturer::tearDown(booldisconnectSignals)", "voidGStreamerVideoCapturer::setupPipeline()")
            self.assertIn("if(RefPtrframeGate=m_frameGate){frameGate->close([&]{GStreamerCapturer::tearDown(disconnectSignals);});}else{GStreamerCapturer::tearDown(disconnectSignals);}", teardown)

    def test_display_capture_reports_its_surface(self):
        for patched in (self.patched, self.libwebrtc_patched()):
            source = squeezed(patched["Source/WebCore/platform/mediastream/gstreamer/GStreamerVideoCaptureSource.cpp"])
            settings = section(source, "constRealtimeMediaSourceSettings&GStreamerVideoCaptureSource::settings()", "voidGStreamerVideoCaptureSource::configurationChanged()")
            self.assertIn("boolisDisplay=m_deviceType==CaptureDevice::DeviceType::Screen||m_deviceType==CaptureDevice::DeviceType::Window;", settings)
            self.assertIn("supportedConstraints.setSupportsFacingMode(!isDisplay);", settings)
            self.assertIn("settings.setDisplaySurface(m_deviceType==CaptureDevice::DeviceType::Screen?DisplaySurfaceType::Monitor:DisplaySurfaceType::Window);settings.setLogicalSurface(false);", settings)
            self.assertIn("supportedConstraints.setSupportsDisplaySurface(true);supportedConstraints.setSupportsLogicalSurface(true);", settings)
            self.assertIn("if(m_deviceType==CaptureDevice::DeviceType::Camera){m_currentSettings->setFacingMode(facingMode());}", settings)
            caps = section(source, "voidGStreamerVideoCaptureSource::sourceCapsChanged(constGstCaps*caps)", "voidGStreamerVideoCaptureSource::captureEnded()")
            order = [
                "boolfollowsIntrinsicSize=size().isEmpty()||size()==intrinsicSize();",
                "setIntrinsicSize(IntSize(*videoResolution),false);",
                "if(m_deviceType!=CaptureDevice::DeviceType::Screen&&m_deviceType!=CaptureDevice::DeviceType::Window){return;}",
                "if(followsIntrinsicSize){setSize(intrinsicSize());}else{ensureIntrinsicSizeMaintainsAspectRatio();}",
            ]
            self.assertEqual([caps.index(item) for item in order], sorted(caps.index(item) for item in order))

    def test_libwebrtc_h264_decoder_formats_are_unique(self):
        path = "Source/WebCore/platform/mediastream/libwebrtc/gstreamer/GStreamerVideoCommon.cpp"
        original = self.originals[path]
        patched = self.libwebrtc_patched()[path]
        self.assertEqual(original.count("webrtc::H264Level::kLevel3_1"), 12)
        self.assertNotIn("webrtc::H264Level::kLevel3_1", patched)
        formats = re.findall(r"createH264Format\(webrtc::H264Profile::kProfile(\w+), webrtc::H264Level::kLevel4, \"([01])\"\)", patched)
        self.assertEqual(len(formats), 12)
        self.assertEqual(len(set(formats)), 12)
        self.assertNotIn(path, self.patched)

    def test_video_frame_converter_is_thread_safe_on_both_backends(self):
        for patched in (self.patched, self.libwebrtc_patched()):
            converter = squeezed(patched["Source/WebCore/platform/graphics/gstreamer/GStreamerVideoFrameConverter.cpp"])
            convert = section(converter, "GRefPtr<GstSample>GStreamerVideoFrameConverter::convert(", "voidGStreamerVideoFrameConverter::releaseUnusedPipelinesTimerFired()")
            self.assertLess(convert.index("convertInSystemMemory(sample,destinationCaps.get());"), convert.index("Lockerlocker{m_lock};"))
            self.assertLess(convert.index("Lockerlocker{m_lock};"), convert.index("convertOnGPU(sample,destinationCaps.get());"))
            self.assertLess(convert.index("convertOnGPU(sample,destinationCaps.get());"), convert.index("ensurePipeline().run(sample,destinationCaps.get());"))
            self.assertLess(convert.index("ensurePipeline().run(sample,destinationCaps.get());"), convert.index("convertMappedInSystemMemory(sample,destinationCaps.get());"))
            self.assertIn("staticthread_localSystemMemoryVideoConvertercache;", converter)
            guard = section(converter, "staticboolisSystemMemoryRawVideo(", "structSystemMemoryVideoConverter")
            self.assertIn('gst_structure_has_name(structure,"video/x-raw")&&format&&g_strcmp0(format,"DMA_DRM")', guard)
            self.assertIn("RunLoop::mainSingleton()", converter)
            self.assertNotIn("RunLoop::currentSingleton()", converter)
            release = section(converter, "voidGStreamerVideoFrameConverter::releaseUnusedPipelinesTimerFired()", "#undefGST_CAT_DEFAULT")
            self.assertIn("if(m_systemMemoryPipeline&&m_systemMemoryPipelineLastUse<=idleSince){m_systemMemoryPipeline=nullptr;}", release)
            self.assertIn("m_gpuPipelines.removeAllMatching([&](auto&pipeline){returnpipeline->lastUse<=idleSince;});", release)
            run = section(converter, "GStreamerVideoFrameConverter::Pipeline::run(", "GStreamerVideoFrameConverter&GStreamerVideoFrameConverter::singleton()")
            self.assertNotIn("GLMemory", run)
            self.assertNotIn("m_capsfilter", converter)
            header = squeezed(patched["Source/WebCore/platform/graphics/gstreamer/GStreamerVideoFrameConverter.h"])
            self.assertIn("Pipeline&ensurePipeline()WTF_REQUIRES_LOCK(m_lock);", header)
            self.assertIn("GRefPtr<GstSample>convertOnGPU(constGRefPtr<GstSample>&,GstCaps*)WTF_REQUIRES_LOCK(m_lock);", header)
            self.assertIn("std::unique_ptr<Pipeline>m_systemMemoryPipelineWTF_GUARDED_BY_LOCK(m_lock);", header)
            self.assertIn("Vector<std::unique_ptr<GPUPipeline>>m_gpuPipelinesWTF_GUARDED_BY_LOCK(m_lock);", header)
            self.assertNotIn("DMABufMemory", header)
            tuning = squeezed(patched["Source/WebCore/platform/gstreamer/VideoEncoderPrivateGStreamer.cpp"])
            latency = section(tuning, "staticvoidsetVaLatencyMode(", "staticvoidwebkit_video_encoder_class_init(")
            self.assertLess(latency.index('"target-usage",7'), latency.index("QUALITY_LATENCY_MODE"))
            self.assertLess(latency.index("QUALITY_LATENCY_MODE"), latency.index('"target-usage",1'))
            self.assertNotIn("cqp", latency)

    def test_h264_codec_strings_accept_every_level_up_to_the_requested_one(self):
        for patched in (self.patched, self.libwebrtc_patched()):
            utilities = squeezed(patched["Source/WebCore/platform/gstreamer/GStreamerCodecUtilities.cpp"])
            h264 = section(utilities, "staticstd::pair<GRefPtr<GstCaps>,GRefPtr<GstCaps>>h264CapsFromCodecString(", "StringBuilderformatBuilder;")
            self.assertIn('if(profile=="baseline"_s){', h264)
            self.assertIn('for(constchar*name:{"constrained-baseline","baseline"})', h264)
            self.assertIn('staticconstexprstd::arrayh264Levels{"1"_s,"1b"_s,"1.1"_s,"1.2"_s,"1.3"_s,"2"_s,"2.1"_s,"2.2"_s,"3"_s,"3.1"_s,"3.2"_s,"4"_s,"4.1"_s,"4.2"_s,"5"_s,"5.1"_s,"5.2"_s,"6"_s,"6.1"_s,"6.2"_s};', h264)
            loop = section(h264, "for(autocandidate=highest;;--candidate){", "gst_caps_set_value(outputCaps.get(),\"level\",&levels);")
            self.assertLess(loop.index("gst_value_list_append_and_take_value(&levels,&value);"), loop.index("if(candidate==h264Levels.begin()){break;}"))
            self.assertIn('gst_caps_set_value(outputCaps.get(),"level",&levels);', h264)
            self.assertIn('if(highest==h264Levels.end()||highest==h264Levels.begin()){gst_caps_set_simple(outputCaps.get(),"level",G_TYPE_STRING,level.ascii().data(),nullptr);}', h264)
            self.assertEqual(h264.count('"level"'), 2)

    def test_video_post_processor_pool_follows_caps_changes(self):
        for patched in (self.patched, self.libwebrtc_patched()):
            encoder = squeezed(patched["Source/WebCore/platform/gstreamer/VideoEncoderPrivateGStreamer.cpp"])
            refresh = section(encoder, "staticvoidvideoEncoderRefreshPostProcessorPool(", "staticboolencoderAcceptsFrameSize(")
            self.assertIn('GRefPtrquery=adoptGRef(gst_query_new_allocation(caps,TRUE));', refresh)
            self.assertIn("gst_pad_query(pad.get(),query.get())", refresh)
            handler = section(encoder, "if(self->priv->acceptsDMABuf){videoEncoderScaleInputWithVideoAcceleration(self,event);", "autoscaleResolutionDownBy=self->priv->scaleResolutionDownBy;")
            self.assertLess(handler.index("boolhandled=gst_pad_event_default(pad,parent,event);"), handler.index("videoEncoderRefreshPostProcessorPool(self,inputCaps.get());"))

    def test_ended_tracks_do_not_block_the_main_thread(self):
        for patched in (self.patched, self.libwebrtc_patched()):
            source = squeezed(patched["Source/WebCore/platform/mediastream/gstreamer/GStreamerMediaStreamSource.cpp"])
            stopped = section(source, "voidsourceStopped()final{", "voidtrackEnabledChanged(")
            self.assertIn("stopObserving();", stopped)
            self.assertIn("{Lockerlocker{m_eosLock};m_eosPending=true;}", stopped)
            self.assertNotIn("waitFor", stopped)
            self.assertNotIn("m_eosCondition", source)
            notify = section(source, "voidnotifyEOS(){", "booleosPending()const{")
            self.assertEqual(notify, "voidnotifyEOS(){assertIsHeld(m_eosLock);m_eosPending=false;m_isEnded=true;}")
            chain = section(source, "staticGstFlowReturnwebkitMediaStreamSrcChain(", "GstFlowReturnchainResult=")
            self.assertIn("source->notifyEOS();}returnGST_FLOW_EOS;", chain)

    def test_hardware_encoder_filler_is_dropped(self):
        for patched in (self.patched, self.libwebrtc_patched()):
            encoder = squeezed(patched["Source/WebCore/platform/gstreamer/VideoEncoderPrivateGStreamer.cpp"])
            ranges = section(encoder, "staticVector<std::pair<size_t,size_t>>acceleratedEncoderFillerRanges(", "staticGstPadProbeReturnvideoEncoderDropFiller(")
            self.assertIn("if(((header>>3)&0x0f)==15){ranges.append({index,end});}", ranges)
            self.assertIn("if((isH265?(header>>1)&0x3f:header&0x1f)==(isH265?38:12)){", ranges)
            self.assertIn("if(size>data.size()-position){return{};}", ranges)
            drop = section(encoder, "staticGstPadProbeReturnvideoEncoderDropFiller(", "staticvoidvideoEncoderApplyBitrate(")
            self.assertIn("if(ranges.isEmpty()){returnGST_PAD_PROBE_OK;}", drop)
            self.assertIn("gst_buffer_copy_into(stripped.get(),buffer,GST_BUFFER_COPY_METADATA,0,-1);", drop)
            self.assertIn("GST_PAD_PROBE_INFO_DATA(info)=stripped.leakRef();gst_buffer_unref(buffer);", drop)
            setup = section(encoder, "staticboolvideoEncoderSetEncoder(", "EncoderIdvideoEncoderFindForFormat(")
            self.assertIn('GRefPtrencoderSrcPad=adoptGRef(gst_element_get_static_pad(priv->encoder.get(),"src"));gst_pad_add_probe(encoderSrcPad.get(),GST_PAD_PROBE_TYPE_BUFFER,videoEncoderDropFiller,GINT_TO_POINTER(encoderId),nullptr);', setup)

    def test_hardware_encoders_do_not_restart_on_every_rate_update(self):
        for patched in (self.patched, self.libwebrtc_patched()):
            encoder = squeezed(patched["Source/WebCore/platform/gstreamer/VideoEncoderPrivateGStreamer.cpp"])
            self.assertEqual(encoder.count("staticboolencoderUsesVideoAcceleration(EncoderIdencoderId)"), 1)
            self.assertLess(encoder.index("staticboolencoderUsesVideoAcceleration(EncoderIdencoderId)"), encoder.index("staticvoidvideoEncoderSetBitrate("))
            state = section(encoder, "struct_WebKitVideoEncoderPrivate{", "};")
            for field in ("unsignedrequestedBitrateWTF_GUARDED_BY_LOCK(bitrateLock);", "unsignedappliedBitrateWTF_GUARDED_BY_LOCK(bitrateLock);", "MonotonicTimebitrateRequestTimeWTF_GUARDED_BY_LOCK(bitrateLock);", "MonotonicTimebitrateRestartTimeWTF_GUARDED_BY_LOCK(bitrateLock);", "boolencoderIsStreamingWTF_GUARDED_BY_LOCK(bitrateLock);"):
                self.assertIn(field, state)
            policy = section(encoder, "staticboolacceleratedEncoderShouldTakeBitrate(", "staticvoidvideoEncoderApplyBitrate(")
            self.assertIn("if(requested==applied){returnfalse;}", policy)
            self.assertIn("if(ratio<=0.5){returntrue;}", policy)
            self.assertLess(policy.index("if(ratio<=0.5){returntrue;}"), policy.index("if(ratio<=0.85||ratio>=1.25){returnsinceRestart>=1_s;}"))
            self.assertIn("returnsinceRequest>=5_s&&sinceRestart>=5_s;", policy)
            apply = section(encoder, "staticvoidvideoEncoderApplyBitrate(", "staticvoidvideoEncoderSetBitrate(")
            self.assertIn("Lockerlocker{priv->bitrateLock};", apply)
            self.assertIn("if(!encoder||priv->encoderId==None||priv->requestedBitrate==priv->appliedBitrate){return;}", apply)
            self.assertIn("boolwasStreaming=std::exchange(priv->encoderIsStreaming,priv->encoderIsStreaming||isStreaming);", apply)
            self.assertIn("if(encoderUsesVideoAcceleration(priv->encoderId)&&wasStreaming){", apply)
            self.assertIn("if(isStreaming&&!wasStreaming){priv->bitrateRestartTime=MonotonicTime::now();}", apply)
            self.assertLess(apply.index("priv->bitrateRestartTime=now;"), apply.index("priv->appliedBitrate=priv->requestedBitrate;"))
            self.assertLess(apply.index("acceleratedEncoderShouldTakeBitrate("), apply.index("definition->setBitrate("))
            set_bitrate = section(encoder, "staticvoidvideoEncoderSetBitrate(", "staticboolvideoEncoderSetEncoder(")
            self.assertIn("if(priv->requestedBitrate!=bitrate){priv->requestedBitrate=bitrate;priv->bitrateRequestTime=MonotonicTime::now();}", set_bitrate)
            self.assertIn("videoEncoderApplyBitrate(self,priv->encoder.get(),false);", set_bitrate)
            self.assertNotIn("encoder->setBitrate(", set_bitrate)
            setup = section(encoder, "staticboolvideoEncoderSetEncoder(", "EncoderIdvideoEncoderFindForFormat(")
            reset = setup.index("priv->appliedBitrate=0;priv->encoderIsStreaming=false;")
            probe = setup.index("gst_pad_add_probe(encoderSinkPad.get(),GST_PAD_PROBE_TYPE_BUFFER,")
            self.assertLess(reset, probe)
            self.assertLess(probe, setup.index("videoEncoderSetBitrate(self,priv->bitrate);returntrue;"))
            self.assertIn("if(encoderUsesVideoAcceleration(encoderId)){GRefPtrencoderSinkPad", setup)
            self.assertIn("videoEncoderApplyBitrate(self,GST_ELEMENT_CAST(GST_PAD_PARENT(pad)),true);", setup)
            keyframes = section(encoder, "casePROP_KEYFRAME_INTERVAL:if(priv->encoder){autoencoder=Encoders::definition(priv->encoderId);autointerval", "break;")
            self.assertIn("interval=std::clamp(interval,G_PARAM_SPEC_UINT(spec)->minimum,G_PARAM_SPEC_UINT(spec)->maximum);", keyframes)
            self.assertIn("interval=std::min<guint>(interval,G_PARAM_SPEC_INT(spec)->maximum);", keyframes)
            self.assertIn("g_object_set(priv->encoder.get(),encoder->keyframeIntervalPropertyName.characters(),interval,nullptr);", keyframes)

    def test_gpu_frames_are_scaled_and_converted_on_the_gpu_before_one_readback(self):
        for patched in (self.patched, self.libwebrtc_patched()):
            converter = squeezed(patched["Source/WebCore/platform/graphics/gstreamer/GStreamerVideoFrameConverter.cpp"])
            initialize = section(converter, "boolGStreamerVideoFrameConverter::GPUPipeline::initialize(){", "GRefPtr<GstSample>GStreamerVideoFrameConverter::GPUPipeline::run(")
            chain = "gst_element_link_many(upload.get(),inputConvert.get(),scale.get(),outputConvert.get(),glCapsFilter.get(),download.get(),videoConvert.get(),m_sink.get(),nullptr)"
            self.assertIn(chain, initialize)
            for element in ('"glupload"_s', '"glcolorconvert"_s', '"glcolorscale"_s', '"gldownload"_s'):
                self.assertIn(element, initialize)
            self.assertIn('isGLColorConvertOutputFormat(format)?format:"RGBA"', initialize)
            self.assertIn('"texture-target",G_TYPE_STRING,"2D"', initialize)
            self.assertIn("gst_caps_set_features(glCaps.get(),0,gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_GL_MEMORY,nullptr));", initialize)
            self.assertIn('"sync",FALSE,"async",FALSE', initialize)
            self.assertIn("gst_bus_set_flushing(bus.get(),TRUE);", initialize)
            self.assertLess(initialize.index('setGstElementGLContext(m_pipeline.get(),"gst.gl.app_context"_s)'), initialize.index("gst_element_set_state(m_pipeline.get(),GST_STATE_PLAYING)"))
            self.assertLess(initialize.index("gst_event_new_stream_start("), initialize.index("gst_event_new_caps(m_inputCaps.get())"))
            self.assertLess(initialize.index("gst_event_new_caps(m_inputCaps.get())"), initialize.index("gst_event_new_segment(&segment)"))
            run = section(converter, "GRefPtr<GstSample>GStreamerVideoFrameConverter::GPUPipeline::run(GstBuffer*buffer){", "GRefPtr<GstSample>GStreamerVideoFrameConverter::convertOnGPU(")
            self.assertLess(run.index("gst_pad_push(m_sourcePad.get(),gst_buffer_ref(buffer))"), run.index("gst_app_sink_try_pull_sample(GST_APP_SINK_CAST(m_sink.get()),0)"))
            self.assertIn("downloaded=gst_memory_map(memory,&mapInfo,GST_MAP_READ);", run)
            self.assertLess(run.index("m_failureTime=MonotonicTime::now();"), run.index("tearDown();"))
            gpu = section(converter, "GRefPtr<GstSample>GStreamerVideoFrameConverter::convertOnGPU(", "GRefPtr<GstSample>GStreamerVideoFrameConverter::convert(")
            self.assertIn("!gst_caps_features_is_equal(destinationFeatures,GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY)", gpu)
            self.assertIn("now-m_gpuPipelines[index]->failureTime()>=gpuPipelineRetryDelay", gpu)
            self.assertIn("if(m_gpuPipelines.size()>=maximumGPUPipelines){", gpu)
            self.assertTrue(gpu.endswith("returnconvertMappedInSystemMemory(sample,destinationCaps);}#endif"))
            self.assertIn("staticconstexprsize_tmaximumGPUPipelines=4;", converter)
            mapped = section(converter, "staticGRefPtr<GstSample>convertMappedInSystemMemory(", "#ifUSE(GSTREAMER_GL)")
            self.assertIn("drmInfo.drm_modifier", mapped)
            self.assertIn("returnconvertInSystemMemory(systemMemorySample,destinationCaps);", mapped)
            convert = section(converter, "GRefPtr<GstSample>GStreamerVideoFrameConverter::convert(", "voidGStreamerVideoFrameConverter::releaseUnusedPipelinesTimerFired()")
            self.assertIn("if(!videoMeta||videoMeta->format!=format||videoMeta->width!=static_cast<guint>(*width)||videoMeta->height!=static_cast<guint>(*height)){", convert)
            self.assertIn("gst_buffer_get_parent_buffer_meta(writableBuffer.get())", convert)

    def test_video_frame_copy_uses_real_plane_strides_for_every_format(self):
        for patched in (self.patched, self.libwebrtc_patched()):
            frame = squeezed(patched["Source/WebCore/platform/graphics/gstreamer/VideoFrameGStreamer.cpp"])
            copy = section(frame, "voidVideoFrame::copyTo(", "RefPtr<NativeImage>VideoFrame::copyNativeImage()")
            self.assertNotIn("FIXME", copy)
            self.assertNotIn("inputFrame.width()%2", copy)
            self.assertNotIn("VideoPixelFormat::NV12", copy)
            self.assertIn("unsignedplaneCount=GST_VIDEO_INFO_N_PLANES(inputFrame.info());if(computedPlaneLayout.size()!=planeCount){", copy)
            self.assertIn("if(!copyPlane(destination,inputFrame.planeData(plane),inputFrame.planeStride(plane),layout)){callback({});return;}", copy)
            self.assertIn("planeLayouts.append({layout.destinationOffset,layout.destinationStride});", copy)
            helper = section(frame, "staticboolcopyPlane(", "voidVideoFrame::copyTo(")
            self.assertEqual(helper.count("returnfalse;"), 2)
            self.assertIn("returntrue;}", helper)

    def test_video_frame_copy_converts_to_requested_rgb_formats(self):
        for patched in (self.patched, self.libwebrtc_patched()):
            idl = squeezed(patched["Source/WebCore/Modules/webcodecs/WebCodecsVideoFrame.idl"])
            self.assertIn("dictionaryVideoFrameCopyToOptions{DOMRectInitrect;sequence<PlaneLayout>layout;VideoPixelFormatformat;PredefinedColorSpacecolorSpace;};", idl)
            header = squeezed(patched["Source/WebCore/Modules/webcodecs/WebCodecsVideoFrame.h"])
            self.assertIn("structCopyToOptions{std::optional<DOMRectInit>rect;std::optional<Vector<PlaneLayout>>layout;std::optional<VideoPixelFormat>format;std::optional<PredefinedColorSpace>colorSpace;};", header)
            algorithms = squeezed(patched["Source/WebCore/Modules/webcodecs/WebCodecsVideoFrameAlgorithms.cpp"])
            parse = section(algorithms, "ExceptionOr<CombinedPlaneLayout>parseVideoFrameCopyToOptions(", "voidinitializeVisibleRectAndDisplaySize(")
            self.assertIn("if(options.format&&!isRGBVideoPixelFormat(*options.format)){returnException{ExceptionCode::NotSupportedError,", parse)
            self.assertIn("if(options.colorSpace&&*options.colorSpace!=PredefinedColorSpace::SRGB){returnException{ExceptionCode::NotSupportedError,", parse)
            self.assertIn("returncomputeLayoutAndAllocationSize(parsedRect.returnValue(),options.layout,options.format?*options.format:sourceFormat);", parse)
            implementation = squeezed(patched["Source/WebCore/Modules/webcodecs/WebCodecsVideoFrame.cpp"])
            self.assertEqual(implementation.count("if(!m_data.format&&!options.format)"), 2)
            self.assertNotIn("if(!m_data.format){", implementation)
            self.assertIn("autocopyFormat=options.format?*options.format:*m_data.format;protect(m_data.internalFrame)->copyTo(buffer,copyFormat,", implementation)
            frame = squeezed(patched["Source/WebCore/platform/graphics/gstreamer/VideoFrameGStreamer.cpp"])
            copy = section(frame, "voidVideoFrame::copyTo(", "RefPtr<NativeImage>VideoFrame::copyNativeImage()")
            self.assertLess(copy.index("if(!copiesWithoutConversion(static_cast<GstVideoFormat>(self.pixelFormat()),targetFormat)){"), copy.index("inputSample=self.convert(targetFormat,"))
            self.assertLess(copy.index("inputSample=self.convert(targetFormat,"), copy.index("switch(self.memoryType()){"))
            helper = section(frame, "staticboolcopiesWithoutConversion(", "voidVideoFrame::copyTo(")
            self.assertIn("(source==GST_VIDEO_FORMAT_BGRA&&target==GST_VIDEO_FORMAT_BGRx)||(source==GST_VIDEO_FORMAT_RGBA&&target==GST_VIDEO_FORMAT_RGBx)", helper)
            self.assertNotIn("GST_VIDEO_FORMAT_BGRx&&target==GST_VIDEO_FORMAT_BGRA", helper)

    def test_audio_starts_with_sticky_activation_on_both_backends(self):
        for patched in (self.patched, self.libwebrtc_patched()):
            policy = squeezed(patched["Source/WebCore/html/PermissionsPolicy.cpp"])
            original = squeezed(self.originals["Source/WebCore/html/PermissionsPolicy.cpp"])
            for include in ('#include"FrameDestructionObserverInlines.h"', '#include"FrameTree.h"', '#include"LocalFrame.h"', '#include"Page.h"'):
                self.assertEqual(policy.count(include), 1 if include not in original else original.count(include))
            allowed = section(policy, "boolPermissionsPolicy::isDocumentAllowedToAutoplay(constDocument&document){", "boolPermissionsPolicy::isFeatureEnabled(Featurefeature,constDocument&document,ShouldReportViolationshouldReportViolation)")
            order = [
                "if(document.requiresUserGestureForVideoPlayback()){returnfalse;}",
                "RefPtrpage=document.page();if(page&&page->requiresUserGestureForVideoPlayback()){returnfalse;}",
                "if(document.isCapturing()){returntrue;}",
                "boolautoplayIsEnabled=isFeatureEnabled(Feature::Autoplay,document,ShouldReportViolation::No);",
                "for(RefPtrframe=document.frame();frame;frame=dynamicDowncast<LocalFrame>(frame->tree().parent())){",
                "if(RefPtrwindow=frame->window();window&&window->hasStickyActivation()){returntrue;}",
            ]
            positions = [allowed.index(item) for item in order]
            self.assertEqual(positions, sorted(positions))
            self.assertTrue(allowed.endswith("if(!autoplayIsEnabled){returnfalse;}}returnfalse;}"))
            self.assertNotIn("hasTransientActivation", allowed)
            self.assertNotIn("isSameOriginAs", allowed)
            header = squeezed(patched["Source/WebCore/html/PermissionsPolicy.h"])
            self.assertIn("PrivateToken,StorageAccess,Autoplay,Invalid};", header)
            self.assertIn("staticboolisFeatureEnabled(Feature,constDocument&,ShouldReportViolation=ShouldReportViolation::Yes);staticboolisDocumentAllowedToAutoplay(constDocument&);", header)
            self.assertIn('casePermissionsPolicy::Feature::Autoplay:return"Autoplay"_s;', policy)
            self.assertIn('constexprautoautoplayToken{"autoplay"_s};', policy)
            self.assertIn("}elseif(value.startsWith(autoplayToken)){feature=PermissionsPolicy::Feature::Autoplay;remainingValue=value.substring(autoplayToken.length());}", policy)
            self.assertIn("casePermissionsPolicy::Feature::PrivateToken:casePermissionsPolicy::Feature::Autoplay:return\"'self'\"_s;", policy)
            coders = squeezed(patched["Source/WebKit/Shared/WebCoreArgumentCoders.serialization.in"])
            self.assertIn("PrivateToken,StorageAccess,Autoplay,Invalid};classWebCore::PermissionsPolicy{", coders)
            context = squeezed(patched["Source/WebCore/Modules/webaudio/AudioContext.cpp"])
            self.assertEqual(context.count('#include"PermissionsPolicy.h"'), 1)
            self.assertNotIn("documentHasStickyActivationForWebAudio", context)
            allow = section(context, "staticboolshouldDocumentAllowWebAudioToAutoPlay(", "voidAudioContext::setDefaultSampleRateForTesting(")
            order = [
                "if(document.isCapturing()){returntrue;}",
                "shouldAutoplayWebAudioForArbitraryUserGesture()",
                "RefPtrwindow=document.window();if(window&&window->hasTransientActivation()){returntrue;}",
                "returnPermissionsPolicy::isDocumentAllowedToAutoplay(document);}",
            ]
            positions = [allow.index(item) for item in order]
            self.assertEqual(positions, sorted(positions))
            self.assertNotIn("returnwindow&&window->hasTransientActivation();", allow)
            suspend = section(context, "voidAudioContext::suspendRendering(", "voidAudioContext::resumeRendering(")
            self.assertIn("if(!willPausePlayback()){if(state()==State::Suspended){promise.resolve();return;}addReaction(State::Suspended,WTF::move(promise));return;}", suspend)
            session = squeezed(patched["Source/WebCore/html/MediaElementSession.cpp"])
            self.assertEqual(session.count('#include"PermissionsPolicy.h"'), 1)
            permitted = section(session, "Expected<void,MediaPlaybackDenialExplanation>MediaElementSession::playbackStateChangePermitted(MediaPlaybackStatestate)const{", "staticboolautoplayPermittedWhileInvisible(")
            self.assertEqual(permitted.count("isDocumentAllowedToAutoplay"), 1)
            self.assertIn(
                "if(m_restrictions&RequireUserGestureForAudioRateChange&&(!element->isVideo()||element->hasAudio())&&!element->muted()&&element->volume()&&!document->processingUserGestureForMedia()&&!PermissionsPolicy::isDocumentAllowedToAutoplay(document.get())){returnmakeUnexpectedDenial(MediaPlaybackDenialReason::UserGestureRequired,\"Usergesturerequiredforaudioratechange\"_s);}",
                permitted,
            )
            self.assertIn("if(m_restrictions&RequireUserGestureForVideoRateChange&&element->isVideo()&&!document->processingUserGestureForMedia()){returnmakeUnexpectedDenial(", permitted)
            for kept in ("requiresFullscreenForVideoPlayback()&&!fullscreenPermitted()", "requiresUserGestureToPauseInPictureInPicture()", "requiresUserGestureToPlayInFullscreen()", "RequirePageVisibilityToPlayAudio&&", "RequireUserGestureForVideoDueToLowPowerMode&&", "RequireUserGestureForVideoDueToAggressiveThermalMitigation&&"):
                self.assertEqual(permitted.count(kept), 1, kept)
            invisible = section(session, "staticboolautoplayPermittedWhileInvisible(", "boolMediaElementSession::autoplayPermitted()const{")
            self.assertTrue(invisible.endswith("if(element.paused()&&!element.wasInterruptedForInvisibleAutoplay()){returnfalse;}if(element.isVideo()&&!element.hasAudio()){returnfalse;}if(element.muted()){returnfalse;}if(!element.volume()){returnfalse;}returntrue;}"))
            self.assertIn("if(!hasBehaviorRestriction(MediaElementSession::InvisibleAutoplayNotPermitted)){returntrue;}if(autoplayPermittedWhileInvisible(*element)){returntrue;}", session)

    def test_hang_recorder_is_opt_in_and_reports_where_time_goes(self):
        for modules in (build_wpe.GSTREAMER_CORRECTNESS_MODULES, build_wpe.LIBWEBRTC_CORRECTNESS_MODULES):
            self.assertIn(build_wpe.hang_recorder, modules)
        ipc = "Source/WebKit/Platform/IPC/"
        for patched in (self.patched, self.libwebrtc_patched()):
            header = squeezed(patched[ipc + "MessageLog.h"])
            self.assertIn("std::optional<Seconds>hangLogThreshold();voidreportSlowMessage(ASCIILiteralactivity,MessageName,Secondsduration);", header)
            log = squeezed(patched[ipc + "MessageLog.cpp"])
            self.assertIn('parseInteger<unsigned>(StringView::fromLatin1(getenv("EDEN_WPE_HANG_LOG")))', log)
            self.assertIn("if(!milliseconds||!*milliseconds){returnstd::nullopt;}", log)
            self.assertIn("if(!threshold||duration<*threshold){return;}", log)
            connection = squeezed(patched[ipc + "Connection.cpp"])
            send = section(connection, "autoConnection::sendSyncMessage(", "autoConnection::waitForSyncReply(")
            self.assertLess(send.index("automessageName=encoder->messageName();autosendStarted=MonotonicTime::now();"), send.index("waitForSyncReply(syncRequestID,messageName,timeout,sendSyncOptions)"))
            self.assertIn('popPendingSyncRequestID(syncRequestID);reportSlowMessage("waitingforthereplyto"_s,messageName,MonotonicTime::now()-sendStarted);});', send)
            self.assertIn('autohandlingStarted=MonotonicTime::now();protect(client())->didReceiveSyncMessage(*this,decoder,replyEncoder);reportSlowMessage("handling"_s,decoder.messageName(),MonotonicTime::now()-handlingStarted);', connection)
            self.assertIn('autohandlingStarted=MonotonicTime::now();client->didReceiveMessage(*this,decoder);reportSlowMessage("handling"_s,decoder.messageName(),MonotonicTime::now()-handlingStarted);', connection)
            stream = squeezed(patched[ipc + "StreamServerConnection.cpp"])
            self.assertIn('autohandlingStarted=MonotonicTime::now();receiver.didReceiveStreamMessage(*this,message);reportSlowMessage("handling"_s,message.messageName(),MonotonicTime::now()-handlingStarted);', stream)
            process = squeezed(patched["Source/WebKit/Shared/AuxiliaryProcess.cpp"])
            self.assertIn("initializeProcessName(parameters);#ifOS(LINUX)startEdenHangRecorderIfRequested();#endif", process)
            self.assertIn("voidstartEdenHangRecorderIfRequested();voidstartEdenHangRecorderIfRequested(){", process)
            recorder = section(process, "voidstartEdenHangRecorderIfRequested(){", "voidAuxiliaryProcess::initialize(")
            order = [
                "staticstd::atomic<bool>started{false};",
                'automs=parseInteger<unsigned>(StringView::fromLatin1(getenv("EDEN_WPE_HANG_LOG")));'.replace("automs", "automilliseconds"),
                "if(!milliseconds||!*milliseconds){return;}",
                "if(started.exchange(true)){return;}",
                "intsignalNumber=SIGRTMIN+5;",
                "action.sa_flags=SA_RESTART;",
                "backtrace(warmUp.data(),",
                "heartbeat.get()->startRepeating(interval);",
                'Thread::create("EdenHangRecorder"_s,',
                "printHangStacks(mainThread,signalNumber);",
                "})->detach();",
            ]
            positions = [recorder.index(item) for item in order]
            self.assertEqual(positions, sorted(positions))
            handler = section(process, "staticvoidcaptureHangStack(int){", "staticCStringhangThreadName(")
            self.assertEqual(handler, "staticvoidcaptureHangStack(int){if(hangCaptureTarget.load()!=currentThreadIdentifier()){return;}hangCaptureCount.store(backtrace(hangCaptureFrames.data(),static_cast<int>(hangCaptureFrames.size())));sem_post(&hangCaptureDone);}")
            self.assertNotIn("SIGUSR1", recorder)
            self.assertNotIn("SIGUSR2", recorder)
            context = squeezed(patched["Source/WebKit/UIProcess/API/glib/WebKitWebContext.cpp"])
            self.assertIn("#ifOS(LINUX)namespaceWebKit{voidstartEdenHangRecorderIfRequested();}#endifusingnamespaceWebKit;", context)
            constructed = section(context, "staticvoidwebkitWebContextConstructed(GObject*object){", "GUniquePtr<char>bundleFilename(")
            self.assertEqual(constructed, "staticvoidwebkitWebContextConstructed(GObject*object){G_OBJECT_CLASS(webkit_web_context_parent_class)->constructed(object);#ifOS(LINUX)startEdenHangRecorderIfRequested();#endif")

    def test_webgl_teardown_makes_no_round_trip_to_the_gpu_process(self):
        for patched in (self.patched, self.libwebrtc_patched()):
            webgl = squeezed(patched["Source/WebCore/html/canvas/WebGLRenderingContextBase.cpp"])
            stop = section(webgl, "voidWebGLRenderingContextBase::stop(){", "voidWebGLRenderingContextBase::suspend(")
            self.assertEqual(stop, "voidWebGLRenderingContextBase::stop(){if(isContextLost()){return;}m_contextLostState=ContextLostState{SyntheticLostContext};m_contextLostState->errors.add(GCGLErrorCode::ContextLost);{Lockerlocker{objectGraphLock()};detachAndRemoveAllObjects();}loseExtensions(SyntheticLostContext);destroyGraphicsContextGL();}")
            self.assertNotIn("getErrors", stop)
            lost = section(webgl, "voidWebGLRenderingContextBase::forceLostContext(WebGLRenderingContextBase::LostContextModemode){", "voidWebGLRenderingContextBase::forceRestoreContext(")
            self.assertIn("protect(graphicsContextGL())->getErrors();", lost)

    def test_webgl_video_frames_stay_in_the_web_process_on_gstreamer(self):
        for modules in (build_wpe.GSTREAMER_CORRECTNESS_MODULES, build_wpe.LIBWEBRTC_CORRECTNESS_MODULES):
            self.assertIn("Source/WebCore/platform/graphics/texmap/GraphicsContextGLTextureMapperANGLE.cpp", module_paths(modules))
        for patched in (self.patched, self.libwebrtc_patched()):
            webgl = squeezed(patched["Source/WebCore/html/canvas/WebGLRenderingContextBase.cpp"])
            self.assertIn('#ifENABLE(VIDEO)#include"VideoFrame.h"#ifUSE(GSTREAMER)#include"VideoFrameGStreamer.h"#endif#endif', webgl)
            capture = section(webgl, "RefPtr<VideoFrame>WebGLRenderingContextBase::surfaceBufferToVideoFrame(SurfaceBufferbuffer){", "RefPtr<ImageBuffer>WebGLRenderingContextBase::transferToImageBuffer()")
            self.assertIn(
                "if(buffer==SurfaceBuffer::DrawingBuffer){clearIfComposited(CallerTypeOther);}"
                "#ifUSE(GSTREAMER)RefPtrcontext=graphicsContextGL();"
                "if(RefPtrframe=context->surfaceBufferToVideoFrame(toGCGLSurfaceBuffer(buffer))){returnframe;}"
                "RefPtrimage=context->copyNativeImageYFlipped(toGCGLSurfaceBuffer(buffer));"
                "if(!image){returnnullptr;}returnVideoFrameGStreamer::createFromBottomUpNativeImage(*image,30);"
                "#elsereturnprotect(graphicsContextGL())->surfaceBufferToVideoFrame(toGCGLSurfaceBuffer(buffer));#endif}",
                capture,
            )
            header = squeezed(patched["Source/WebCore/platform/graphics/gstreamer/VideoFrameGStreamer.h"])
            self.assertEqual(header.count("staticRefPtr<VideoFrameGStreamer>createFromBottomUpNativeImage(NativeImage&,doubleframeRate);"), 1)
            videoFrame = squeezed(patched["Source/WebCore/platform/graphics/gstreamer/VideoFrameGStreamer.cpp"])
            self.assertEqual(videoFrame.count('#include"TypedArrayPixelBuffer.h"'), 1)
            bottomUp = section(videoFrame, "RefPtr<VideoFrameGStreamer>VideoFrameGStreamer::createFromBottomUpNativeImage(NativeImage&image,doubleframeRate){", "RefPtr<VideoFrame>VideoFrame::fromNativeImage(NativeImage&image)")
            order = [
                "ensureVideoFrameDebugCategoryInitialized();",
                "autosize=image.size();",
                "if(!platformImage||size.isEmpty()||platformImage->width()!=size.width()||platformImage->height()!=size.height()){returnnullptr;}",
                "boolcanPeek=!platformImage->isTextureBacked()&&platformImage->peekPixels(&pixmap)&&(pixmap.colorType()==kRGBA_8888_SkColorType||pixmap.colorType()==kBGRA_8888_SkColorType)&&pixmap.alphaType()!=kPremul_SkAlphaType;",
                "RefPtrpixelBuffer=ByteArrayPixelBuffer::tryCreate({AlphaPremultiplication::Unpremultiplied,pixelFormat,DestinationColorSpace::SRGB()},size);",
                "if(destination.size()<rowBytes*height){returnnullptr;}",
                "if(canPeek){",
                "for(size_trow=0;row<height;++row){memcpySpan(destination.subspan(row*rowBytes,rowBytes),source.subspan((height-1-row)*pixmap.rowBytes(),rowBytes));}",
                "returncreateFromPixelBuffer(pixelBuffer.releaseNonNull(),{},frameRate,{});}",
                "if(platformImage->isTextureBacked()&&!PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent()){returnnullptr;}",
                "if(!platformImage->readPixels(image.grContext(),info,destination.data(),rowBytes,0,0)){returnnullptr;}",
                "for(size_ttop=0,bottom=height-1;top<bottom;++top,--bottom){",
                "memcpySpan(scratch.mutableSpan(),bottomRow);memcpySpan(bottomRow,topRow);memcpySpan(topRow,scratch.span());}",
                "returncreateFromPixelBuffer(pixelBuffer.releaseNonNull(),{},frameRate,{});}",
            ]
            positions = [bottomUp.index(item) for item in order[:8]] + [bottomUp.rindex(item) if item.startswith("returncreateFromPixelBuffer") else bottomUp.index(item) for item in order[8:]]
            self.assertEqual(positions[:8], sorted(positions[:8]))
            self.assertLess(positions[7], bottomUp.index(order[8]))
            self.assertEqual(positions[9:], sorted(positions[9:]))
            self.assertEqual(bottomUp.count("returncreateFromPixelBuffer(pixelBuffer.releaseNonNull(),{},frameRate,{});"), 2)
            for absent in ("ImageBuffer::create(", "paintToCanvas", "getPixelBuffer(", "Rotation::UpsideDown", "isMirrored", "ensureGStreamerInitialized()"):
                self.assertNotIn(absent, bottomUp)
            setup = section(videoFrame, "staticvoidensureVideoFrameDebugCategoryInitialized(){", "#ifUSE(GBM)")
            self.assertLess(setup.index("if(isInWebProcess()){ensureGStreamerInitialized();}else{ensureGStreamerInitializedNonWebProcess();}"), setup.index("GST_DEBUG_CATEGORY_INIT(webkit_video_frame_debug"))
            proxy = squeezed(patched["Source/WebKit/WebProcess/GPU/graphics/RemoteGraphicsContextGLProxy.cpp"])
            frame = section(proxy, "RefPtr<WebCore::VideoFrame>RemoteGraphicsContextGLProxy::surfaceBufferToVideoFrame(SurfaceBufferbuffer){", "#ifENABLE(VIDEO)")
            self.assertIn("if(isContextLost()){returnnullptr;}#ifUSE(GSTREAMER)UNUSED_PARAM(buffer);returnnullptr;#elseautosendResult=sendSync(Messages::RemoteGraphicsContextGL::SurfaceBufferToVideoFrame(buffer));", frame)
            self.assertLess(frame.index("RemoteVideoFrameProxy::create("), frame.index("#endif}"))
            for absent in ("copyNativeImageYFlipped", "ImageBuffer::create(", "paintToCanvas", "fromNativeImage"):
                self.assertNotIn(absent, frame)
            server = squeezed(patched["Source/WebKit/GPUProcess/graphics/RemoteGraphicsContextGL.cpp"])
            handler = section(server, "voidRemoteGraphicsContextGL::surfaceBufferToVideoFrame(", "boolRemoteGraphicsContextGL::webXREnabled()")
            self.assertIn("#ifUSE(GSTREAMER)UNUSED_PARAM(buffer);#elseif(autovideoFrame=protect(m_context)->surfaceBufferToVideoFrame(buffer)){result=m_videoFrameObjectHeap->add(videoFrame.releaseNonNull());}#endifcompletionHandler(WTF::move(result));", handler)
            gbm = squeezed(patched["Source/WebKit/WebProcess/GPU/graphics/gbm/RemoteGraphicsContextGLProxyGBM.cpp"])
            prepare = section(gbm, "voidRemoteGraphicsContextGLProxyGBM::prepareForDisplay()", "Ref<RemoteGraphicsContextGLProxy>RemoteGraphicsContextGLProxy::platformCreate(")
            self.assertIn("if(!m_displayBuffer){return;}m_hasPreparedForDisplay=true;OptionSet<TextureMapperFlags>flags", prepare)

    def test_webgl_frames_travel_as_pooled_dmabufs_on_both_backends(self):
        for patched in (self.patched, self.libwebrtc_patched()):
            queue = squeezed(patched["Source/WebCore/platform/graphics/DMABufBuffer.h"])
            release = section(queue, "classDMABufBufferReleaseQueuefinal:publicThreadSafeRefCounted<DMABufBufferReleaseQueue>{", "Vector<uint64_t>m_releasedBufferIDsWTF_GUARDED_BY_LOCK(m_lock);")
            self.assertIn("voidadd(uint64_tbufferID){Lockerlocker{m_lock};m_releasedBufferIDs.append(bufferID);}", release)
            self.assertIn("Vector<uint64_t>take(){Lockerlocker{m_lock};returnstd::exchange(m_releasedBufferIDs,{});}", release)
            api = squeezed(patched["Source/WebCore/platform/VideoFrame.h"])
            self.assertIn("#ifUSE(GSTREAMER)&&USE(GBM)WEBCORE_EXPORTstaticRefPtr<VideoFrame>createFromDMABufBuffer(DMABufBuffer&,DMABufBufferReleaseQueue&);WEBCORE_EXPORTstaticVector<uint64_t>preferredDMABufModifiers(boolhasAlpha);#endif", api)

            frame = squeezed(patched["Source/WebCore/platform/graphics/gstreamer/VideoFrameGStreamer.cpp"])
            wrap = section(frame, "RefPtr<VideoFrame>VideoFrame::createFromDMABufBuffer(DMABufBuffer&dmabuf,DMABufBufferReleaseQueue&releaseQueue){", "VideoFrameGStreamer::VideoFrameGStreamer(")
            order = [
                "if(!gst_video_info_dma_drm_from_video_info(&drmInfo,&info,attributes.modifier)){returnnullptr;}",
                "GRefPtrcaps=adoptGRef(gst_video_info_dma_drm_to_caps(&drmInfo));",
                "autofd=attributes.fds[0].duplicate();",
                "if(end>0&&static_cast<gsize>(end)<requiredSize){",
                "auto*memory=gst_dmabuf_allocator_alloc(dmabufVideoFrameAllocator(),fd.value(),end>0?static_cast<gsize>(end):requiredSize);",
                "[[maybe_unused]]intmemoryFD=fd.release();",
                "GST_MINI_OBJECT_FLAG_SET(memory,GST_MEMORY_FLAG_READONLY);",
                "release->queue->add(release->bufferID);",
                'staticGQuarkdmabufQuark=g_quark_from_static_string("wk-dmabuf-buffer");dmabuf.ref();',
                "gst_buffer_add_video_meta_full(buffer.get(),GST_VIDEO_FRAME_FLAG_NONE,format,width,height,1,offsets,strides);",
                "returnVideoFrameGStreamer::create(WTF::move(sample),options);",
            ]
            self.assertEqual([wrap.index(item) for item in order], sorted(wrap.index(item) for item in order))
            self.assertIn("caseDRM_FORMAT_ARGB8888:format=GST_VIDEO_FORMAT_BGRA;break;caseDRM_FORMAT_XRGB8888:format=GST_VIDEO_FORMAT_BGRx;break;", wrap)
            modifiers = section(frame, "Vector<uint64_t>VideoFrame::preferredDMABufModifiers(boolhasAlpha){", "RefPtr<VideoFrame>VideoFrame::createFromDMABufBuffer(")
            self.assertIn('gst_element_factory_find("vapostproc")', modifiers)
            self.assertIn("if(padTemplate->direction!=GST_PAD_SINK){continue;}", modifiers)
            self.assertIn("if(modifier==DRM_FORMAT_MOD_LINEAR||modifier==DRM_FORMAT_MOD_INVALID){return;}", modifiers)
            self.assertIn("returnmodifiers.get()[hasAlpha?1:0];", modifiers)

            header = squeezed(patched["Source/WebCore/platform/graphics/gbm/GraphicsContextGLTextureMapperGBM.h"])
            self.assertIn("VideoFrameBufferCopycopySurfaceBufferToVideoFrameBuffer(SurfaceBuffer,std::span<constuint64_t>releasedBufferIDs,std::span<constuint64_t>preferredModifiers);", header)
            self.assertIn("RefPtr<VideoFrame>surfaceBufferToVideoFrame(SurfaceBuffer)final;", header)
            self.assertIn("structDrawingBuffer{RefPtr<DMABufBuffer>dmabuf;EGLImageKHRimage{nullptr};GCGLuintreadTexture{0};};", header)
            context = squeezed(patched["Source/WebCore/platform/graphics/gbm/GraphicsContextGLTextureMapperGBM.cpp"])
            self.assertIn("staticconstexprsize_tmaximumVideoFrameBuffers=6;", context)
            self.assertIn('m_canCopyVideoFrames=supportsExtensionImpl("GL_CHROMIUM_copy_texture"_s);', context)
            copy = section(context, "autoGraphicsContextGLTextureMapperGBM::copySurfaceBufferToVideoFrameBuffer(", "#ifUSE(GSTREAMER)RefPtr<VideoFrame>GraphicsContextGLTextureMapperGBM::surfaceBufferToVideoFrame(")
            order = [
                "buffer.isInUse=false;",
                "if(!m_canCopyVideoFrames||!makeContextCurrent()){returncopy;}",
                "updateErrors();",
                "autosourceTexture=videoFrameSourceTexture(source);",
                "autoindex=acquireVideoFrameBuffer(size,attributes.alpha?DRM_FORMAT_ARGB8888:DRM_FORMAT_XRGB8888,preferredModifiers,copy.retiredBufferIDs);",
                "boolshouldUnmultiply=attributes.alpha&&attributes.premultipliedAlpha;",
                "GL_CopySubTextureCHROMIUM(sourceTexture,0,GL_TEXTURE_2D,buffer.texture,0,0,0,0,0,size.width(),size.height(),GL_TRUE,GL_FALSE,shouldUnmultiply?GL_TRUE:GL_FALSE);",
                "if(autoerror=GL_GetError();error!=GL_NO_ERROR){",
                "publishVideoFrameBufferContents(buffer);",
                "buffer.isInUse=true;",
                "copy.isNewBuffer=!std::exchange(buffer.wasDelivered,true);",
            ]
            self.assertEqual([copy.index(item) for item in order], sorted(copy.index(item) for item in order))
            publish = section(context, "voidGraphicsContextGLTextureMapperGBM::publishVideoFrameBufferContents(", "autoGraphicsContextGLTextureMapperGBM::copySurfaceBufferToVideoFrameBuffer(")
            self.assertLess(publish.index("autofence=createExportedFence();"), publish.index("DMA_BUF_IOCTL_IMPORT_SYNC_FILE"))
            self.assertIn("structdma_buf_import_sync_filerequest{DMA_BUF_SYNC_WRITE,fence.value()};", publish)
            self.assertIn("while(result==-1&&(errno==EINTR||errno==EAGAIN));", publish)
            self.assertTrue(publish.rstrip("}").endswith("GL_Finish();"))
            source = section(context, "GCGLuintGraphicsContextGLTextureMapperGBM::videoFrameSourceTexture(SurfaceBuffersource){", "voidGraphicsContextGLTextureMapperGBM::publishVideoFrameBufferContents(")
            self.assertIn("GL_EGLImageTargetTexture2DOES(GL_TEXTURE_2D,m_displayBuffer.image);", source)
            self.assertIn("if(contextAttributes().antialias){resolveMultisamplingIfNecessary();}", source)
            self.assertIn("returnm_preserveDrawingBufferTexture?m_preserveDrawingBufferTexture:m_texture;", source)
            acquire = section(context, "std::optional<size_t>GraphicsContextGLTextureMapperGBM::acquireVideoFrameBuffer(", "GCGLuintGraphicsContextGLTextureMapperGBM::videoFrameSourceTexture(")
            self.assertIn("retiredBufferIDs.append(buffer.dmabuf->id());destroyVideoFrameBuffer(buffer);m_videoFrameBuffers.removeAt(index);", acquire)
            self.assertLess(acquire.index("createVideoFrameBuffer(size,fourcc,preferredModifiers);"), acquire.index("createVideoFrameBuffer(size,fourcc,linearModifier);"))
            create = section(context, "GraphicsContextGLTextureMapperGBM::VideoFrameBufferGraphicsContextGLTextureMapperGBM::createVideoFrameBuffer(", "voidGraphicsContextGLTextureMapperGBM::destroyVideoFrameBuffer(")
            self.assertIn("gbm_bo_create_with_modifiers2(gbmDevice->device(),size.width(),size.height(),fourcc,modifiers.data(),modifiers.size(),GBM_BO_USE_RENDERING);", create)
            self.assertIn("elseif(std::ranges::find(modifiers,dmaBufAttributes->modifier)==modifiers.end()){return{};}", create)
            destructor = section(context, "GraphicsContextGLTextureMapperGBM::~GraphicsContextGLTextureMapperGBM(){", "boolGraphicsContextGLTextureMapperGBM::platformInitialize()")
            self.assertLess(destructor.index("if(!makeContextCurrent()){"), destructor.index("freeVideoFrameBuffers();"))
            self.assertLess(destructor.index("freeVideoFrameBuffers();"), destructor.index("freeDrawingBuffers();"))
            self.assertIn("if(buffer.readTexture){GL_DeleteTextures(1,&buffer.readTexture);buffer.readTexture=0;}", context)

            texmap = squeezed(patched["Source/WebCore/platform/graphics/texmap/GraphicsContextGLTextureMapperANGLE.cpp"])
            self.assertIn("RefPtr<VideoFrame>GraphicsContextGLTextureMapperANGLE::surfaceBufferToVideoFrame(SurfaceBuffer){returnnullptr;}", texmap)
            self.assertNotIn("Rotation::UpsideDown", texmap)
            self.assertIn("RefPtr<VideoFrame>surfaceBufferToVideoFrame(SurfaceBuffer)override;", squeezed(patched["Source/WebCore/platform/graphics/texmap/GraphicsContextGLTextureMapperANGLE.h"]))

            messages = patched["Source/WebKit/GPUProcess/graphics/RemoteGraphicsContextGL.messages.in"]
            self.assertIn("#if USE(GBM) && USE(GSTREAMER)\n    void CopySurfaceBufferToVideoFrameBuffer(enum:bool WebCore::GraphicsContextGLSurfaceBuffer buffer, Vector<uint64_t> releasedBufferIDs, Vector<uint64_t> preferredModifiers) -> (uint64_t bufferID, std::optional<WebCore::DMABufBufferAttributes> dmabufAttributes, Vector<uint64_t> retiredBufferIDs) Synchronous NotStreamEncodableReply\n#endif\n", messages)
            server = squeezed(patched["Source/WebKit/GPUProcess/graphics/RemoteGraphicsContextGLGBM.cpp"])
            handler = section(server, "voidRemoteGraphicsContextGLGBM::copySurfaceBufferToVideoFrameBuffer(", "Ref<RemoteGraphicsContextGL>RemoteGraphicsContextGL::create(")
            self.assertIn("assertIsCurrent(workQueue());", handler)
            self.assertIn("completionHandler(0,std::nullopt,WTF::move(copy.retiredBufferIDs));", handler)
            self.assertIn("source.fds.map([](constauto&fd){returnfd.duplicate();})", handler)
            proxy = squeezed(patched["Source/WebKit/WebProcess/GPU/graphics/gbm/RemoteGraphicsContextGLProxyGBM.cpp"])
            request = section(proxy, "RefPtr<VideoFrame>RemoteGraphicsContextGLProxyGBM::surfaceBufferToVideoFrame(SurfaceBufferbuffer){", "Ref<RemoteGraphicsContextGLProxy>RemoteGraphicsContextGLProxy::platformCreate(")
            order = [
                "autoreleasedBufferIDs=m_videoFrameReleaseQueue->take();",
                "sendSync(Messages::RemoteGraphicsContextGL::CopySurfaceBufferToVideoFrameBuffer(buffer,releasedBufferIDs,preferredModifiers));",
                "markContextLost();",
                "m_videoFrameBuffers.remove(retiredBufferID);",
                "m_videoFrameBuffers.set(bufferID,DMABufBuffer::create(bufferID,WTF::move(*bufferAttributes)));",
                "frame=VideoFrame::createFromDMABufBuffer(*dmabuf,m_videoFrameReleaseQueue);",
                "if(!frame){m_videoFrameReleaseQueue->add(bufferID);}",
            ]
            self.assertEqual([request.index(item) for item in order], sorted(request.index(item) for item in order))
            self.assertIn("RefPtr<WebCore::VideoFrame>surfaceBufferToVideoFrame(SurfaceBuffer)override;", squeezed(patched["Source/WebKit/WebProcess/GPU/graphics/RemoteGraphicsContextGLProxy.h"]))

            encoderBin = squeezed(patched["Source/WebCore/platform/gstreamer/VideoEncoderPrivateGStreamer.cpp"])
            chain = section(encoderBin, "staticboolvideoEncoderSetEncoder(", "EncoderIdvideoEncoderFindForFormat(")
            self.assertIn('if(encoderUsesVideoAcceleration(encoderId)){videoConvert=makeGStreamerElement("vapostproc"_s);', chain)
            self.assertIn('gst_caps_set_features(videoAccelerationCaps.get(),0,gst_caps_features_new("memory:VAMemory",nullptr));', chain)
            self.assertIn("if(priv->enableVideoFlip&&!priv->acceptsDMABuf){", chain)
            self.assertIn("encoderAcceptsFrameSize(encoder.factory.get(),caps.get())", encoderBin)
            self.assertIn("if(self->priv->acceptsDMABuf){videoEncoderScaleInputWithVideoAcceleration(self,event);GstCaps*caps=nullptr;gst_event_parse_caps(event,&caps);GRefPtr<GstCaps>inputCaps=caps;boolhandled=gst_pad_event_default(pad,parent,event);if(handled&&inputCaps){videoEncoderRefreshPostProcessorPool(self,inputCaps.get());}returnhandled;}", encoderBin)
            self.assertNotIn('"video-direction","auto");}}if(!videoConvert)', encoderBin)
            self.assertIn("priv->postProcessor=videoConvert;", chain)
            tag = section(encoderBin, "if(GST_EVENT_TYPE(event)==GST_EVENT_TAG){", "if(GST_EVENT_TYPE(event)==GST_EVENT_CAPS){")
            self.assertIn('gst_tag_list_get_string(tags,GST_TAG_IMAGE_ORIENTATION,&orientation.outPtr())&&g_strcmp0(orientation.get(),"rotate-0")', tag)
            self.assertIn('gst_util_set_object_arg(G_OBJECT(self->priv->postProcessor.get()),"video-direction","auto");', tag)
            wrapper = squeezed(patched["Source/WebCore/platform/graphics/gstreamer/VideoEncoderGStreamer.cpp"])
            encode = section(wrapper, "boolGStreamerInternalVideoEncoder::encode(", "voidGStreamerInternalVideoEncoder::setRates(")
            self.assertLess(encode.index("if(!encoderTakesFrameMemory(gstVideoFrame.memoryType(),acceptsDMABuf())){"), encode.index("sample=gstVideoFrame.convert(GST_VIDEO_FORMAT_I420,size);"))
            source = squeezed(patched["Source/WebCore/platform/mediastream/gstreamer/GStreamerMediaStreamSource.cpp"])
            self.assertIn("if(autoformat=systemMemoryFormatForDownstream(*gstVideoFrame,needsEvenSize)){sample=gstVideoFrame->convert(*format,captureSize);}", source)
            downstream = section(source, "std::optional<GstVideoFormat>systemMemoryFormatForDownstream(", "voidaudioSamplesAvailable(")
            self.assertIn("m_needsDownstreamMemoryCheck.exchange(false)", downstream)
            self.assertIn("if(frame.memoryType()==VideoFrameGStreamer::MemoryType::System||m_consumerIsVideoPlayer){returnstd::nullopt;}", downstream)
            self.assertIn("boolisExplicit=allowed&&!gst_caps_is_any(allowed.get())&&!gst_caps_is_empty(allowed.get());", downstream)
            self.assertIn("if(isExplicit&&gst_caps_can_intersect(allowed.get(),caps)){m_downstreamSystemMemoryFormat=std::nullopt;}", downstream)
            self.assertIn("if(GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info))==GST_EVENT_RECONFIGURE){self->m_needsDownstreamMemoryCheck=true;}", source)
            packetizer = squeezed(patched["Source/WebCore/platform/mediastream/gstreamer/GStreamerVideoRTPPacketizer.cpp"])
            self.assertIn("gst_caps_set_features(caps.get(),0,gst_caps_features_new_any());", packetizer)

        glue = "Source/WebCore/platform/mediastream/libwebrtc/gstreamer/"
        patched = self.libwebrtc_patched()
        outgoing = squeezed(patched[glue + "RealtimeOutgoingVideoSourceLibWebRTC.cpp"])
        self.assertLess(outgoing.index("frameSize.scale(this->videoFrameScaling());"), outgoing.index("GStreamerVideoFrameLibWebRTC::createScaled(WTF::move(nativeSample),frameSize)"))
        self.assertLess(outgoing.index("GStreamerVideoFrameLibWebRTC::createScaled(WTF::move(nativeSample),frameSize)"), outgoing.index("autosample=gstFrame.convert(GST_VIDEO_FORMAT_I420,frameSize,colorSpace);"))
        frames = squeezed(patched[glue + "GStreamerVideoFrameLibWebRTC.cpp"])
        crop = section(frames, "webrtc::scoped_refptr<webrtc::VideoFrameBuffer>GStreamerVideoFrameLibWebRTC::CropAndScale(", "webrtc::scoped_refptr<webrtc::I420BufferInterface>GStreamerVideoFrameLibWebRTC::copyToI420()")
        self.assertIn("returnwebrtc::VideoFrameBuffer::CropAndScale(offsetX,offsetY,cropWidth,cropHeight,scaledWidth,scaledHeight);", crop)
        self.assertIn("FloatRectvisibleRect{m_visibleRect.x()+offsetX*scaleX,m_visibleRect.y()+offsetY*scaleY,cropWidth*scaleX,cropHeight*scaleY};", crop)
        toI420 = section(frames, "webrtc::scoped_refptr<webrtc::I420BufferInterface>GStreamerVideoFrameLibWebRTC::ToI420()", "#undefGST_CAT_DEFAULT")
        self.assertLess(toI420.index("Lockerlocker{m_i420Lock};"), toI420.index("m_i420Buffer=copyToI420();"))
        wrap = section(frames, "staticwebrtc::scoped_refptr<webrtc::I420BufferInterface>wrapI420Sample(", "GRefPtr<GstSample>convertLibWebRTCVideoFrameToGStreamerSample(")
        self.assertIn("returnwebrtc::WrapI420Buffer(size.width(),size.height(),planeY.data(),strideY,planeU.data(),strideU,planeV.data(),strideV,[frame=frame.release()]{deleteframe;});", wrap)
        self.assertIn("offset.x()%2||offset.y()%2", wrap)
        convert = section(frames, "GRefPtr<GstSample>convertLibWebRTCVideoFrameToGStreamerSample(constwebrtc::VideoFrame&frame,boolacceptsDMABuf){", "std::optional<webrtc::VideoFrame>convertGStreamerSampleToLibWebRTCVideoFrame(")
        self.assertIn("caseGStreamerVideoFrameLibWebRTC::MemoryType::DMABuf:if(acceptsDMABuf&&!nativeBuffer.isCropped()){returnnativeBuffer.sample();}break;", convert)
        self.assertIn("convertLibWebRTCVideoFrameToGStreamerSample(frame,true);", squeezed(patched[glue + "RealtimeIncomingVideoSourceLibWebRTC.cpp"]))
        decoder = squeezed(patched[glue + "GStreamerVideoDecoderFactory.cpp"])
        self.assertNotIn("gst_app_sink_set_caps", decoder)

    def test_webcodecs_decoders_label_annex_b_input_as_a_byte_stream(self):
        for patched in (self.patched, self.libwebrtc_patched()):
            decoder = squeezed(patched["Source/WebCore/platform/graphics/gstreamer/VideoDecoderGStreamer.cpp"])
            self.assertIn('autocodecData=wrapSpanData(config.description);m_inputCaps=adoptGRef(gst_caps_new_simple("video/x-h264","stream-format",G_TYPE_STRING,codecData?"avc":"byte-stream","alignment",G_TYPE_STRING,"au",nullptr));if(codecData){gst_caps_set_simple(m_inputCaps.get(),"codec_data",GST_TYPE_BUFFER,codecData.get(),nullptr);}', decoder)
            for tag in ("hvc1", "hev1"):
                self.assertIn(f'm_inputCaps=adoptGRef(gst_caps_new_simple("video/x-h265","stream-format",G_TYPE_STRING,codecData?"{tag}":"byte-stream","alignment",G_TYPE_STRING,"au",nullptr));', decoder)
            self.assertNotIn('"stream-format",G_TYPE_STRING,"avc","alignment"', decoder)

    def test_capture_devices_are_enumerated_off_the_main_thread(self):
        gstreamer = "Source/WebCore/platform/mediastream/gstreamer/"
        for patched in (self.patched, self.libwebrtc_patched()):
            manager = squeezed(patched[gstreamer + "GStreamerCaptureDeviceManager.cpp"])
            original = squeezed(self.originals[gstreamer + "GStreamerCaptureDeviceManager.cpp"])
            self.assertIn("GST_CLOCK_TIME_NONE,GST_MESSAGE_DEVICE_MONITOR_STARTED", original)
            self.assertNotIn("GST_CLOCK_TIME_NONE", manager)
            for include in ("Condition.h", "Lock.h", "MainThread.h", "NeverDestroyed.h", "ThreadSafeRefCounted.h", "WorkQueue.h"):
                self.assertIn(f"#include<wtf/{include}>", manager)
            self.assertLess(manager.index("classGStreamerDeviceMonitorStartupfinal:publicThreadSafeRefCounted<GStreamerDeviceMonitorStartup>{"), manager.index("GStreamerCaptureDeviceManager::GStreamerCaptureDeviceManager()"))
            self.assertLess(manager.index("staticgintsortDevices("), manager.index("staticStartedGStreamerDeviceMonitorstartDeviceMonitorAndCollectDevices("))
            startup = section(manager, "classGStreamerDeviceMonitorStartupfinal", "staticWorkQueue&deviceMonitorQueue()")
            self.assertIn("voidfinish(StartedGStreamerDeviceMonitor&&started){Lockerlocker{m_lock};m_started=WTF::move(started);m_finished=true;m_condition.notifyAll();}", startup)
            self.assertIn("Lockerlocker{m_lock};while(waitForStartup&&!m_finished){m_condition.wait(m_lock);}returnstd::exchange(m_started,std::nullopt);", startup)
            self.assertIn("boolm_finishedWTF_GUARDED_BY_LOCK(m_lock){false};", startup)
            self.assertIn('staticNeverDestroyed<Ref<WorkQueue>>queue(WorkQueue::create("GStreamerdevicemonitor"_s));', manager)
            start = section(manager, "staticStartedGStreamerDeviceMonitorstartDeviceMonitorAndCollectDevices(OptionSet<CaptureDevice::DeviceType>types){", "GStreamerAudioCaptureDeviceManager&GStreamerAudioCaptureDeviceManager::singleton()")
            order = [
                "GRefPtrmonitor=adoptGRef(gst_device_monitor_new());",
                'gst_device_monitor_add_filter(monitor.get(),"Video/Source",nullptr);',
                'gst_device_monitor_add_filter(monitor.get(),"Audio/Source",caps.get());',
                'gst_device_monitor_add_filter(monitor.get(),"Audio/Sink",caps.get());',
                "if(!gst_device_monitor_start(monitor.get())){",
                "#ifGST_CHECK_VERSION(1,28,0)staticconstexprGstClockTimemaximumProviderStartup=5*GST_SECOND;",
                "gst_bus_timed_pop_filtered(bus.get(),maximumProviderStartup,GST_MESSAGE_DEVICE_MONITOR_STARTED)",
                "g_list_sort(gst_device_monitor_get_devices(monitor.get()),sortDevices);",
                "started.devices.append(adoptGRef(GST_DEVICE_CAST(devices->data)));",
                "gst_bus_set_flushing(bus.get(),TRUE);gst_bus_set_flushing(bus.get(),FALSE);",
                "started.monitor=WTF::move(monitor);returnstarted;}",
            ]
            positions = [start.index(item) for item in order]
            self.assertEqual(positions, sorted(positions))
            self.assertEqual(manager.count("gst_bus_set_flushing(bus.get(),TRUE);"), 1)
            compute = section(manager, "voidGStreamerCaptureDeviceManager::computeCaptureDevices(CompletionHandler<void()>&&callback){", "voidGStreamerCaptureDeviceManager::startMonitorAsynchronously()")
            self.assertEqual(compute, "voidGStreamerCaptureDeviceManager::computeCaptureDevices(CompletionHandler<void()>&&callback){if(m_deviceMonitor||m_isTearingDown||MockRealtimeMediaSourceCenter::mockRealtimeMediaSourceCenterEnabled()){callback();return;}m_pendingComputeCallbacks.append(WTF::move(callback));if(!m_monitorStartup){startMonitorAsynchronously();}}")
            self.assertEqual(manager.count('#include"MockRealtimeMediaSourceCenter.h"'), 1)
            self.assertNotIn(gstreamer + "GStreamerMockDeviceProvider.cpp", build_wpe.capture_device_enumeration.PATCHES)
            launch = section(manager, "voidGStreamerCaptureDeviceManager::startMonitorAsynchronously(){", "voidGStreamerCaptureDeviceManager::finishMonitorStartup(")
            self.assertIn("m_monitorStartup=startup.copyRef();deviceMonitorQueue().dispatch([this,startup=WTF::move(startup),types=deviceTypes()]()mutable{startup->finish(startDeviceMonitorAndCollectDevices(types));callOnMainThread([this,startup=WTF::move(startup)]()mutable{finishMonitorStartup(startup.get());});});", launch)
            finish = section(manager, "voidGStreamerCaptureDeviceManager::finishMonitorStartup(GStreamerDeviceMonitorStartup&startup){", "voidGStreamerCaptureDeviceManager::refreshCaptureDevices()")
            self.assertIn("autostarted=startup.take(false);if(&startup==m_monitorStartup.get()){m_monitorStartup=nullptr;if(started){adoptDeviceMonitor(WTF::move(*started));}}", finish)
            self.assertIn("else{if(started&&started->monitor){gst_device_monitor_stop(started->monitor.get());}if(m_monitorStartup){return;}if(!m_deviceMonitor&&!m_isTearingDown&&!m_pendingComputeCallbacks.isEmpty()){startMonitorAsynchronously();return;}}", finish)
            self.assertTrue(finish.endswith("for(auto&callback:std::exchange(m_pendingComputeCallbacks,{})){callback();}}"))
            refresh = section(manager, "voidGStreamerCaptureDeviceManager::refreshCaptureDevices(){", "voidGStreamerCaptureDeviceManager::adoptDeviceMonitor(")
            self.assertIn("m_devices.clear();m_gstreamerDevices.clear();m_speakerDevices.clear();if(m_isTearingDown){return;}", refresh)
            self.assertIn("if(!m_deviceMonitor){std::optional<StartedGStreamerDeviceMonitor>started;if(RefPtrstartup=std::exchange(m_monitorStartup,nullptr)){started=startup->take(true);}else{started=startDeviceMonitorAndCollectDevices(deviceTypes());}if(started){adoptDeviceMonitor(WTF::move(*started));}return;}", refresh)
            adopt = section(manager, "voidGStreamerCaptureDeviceManager::adoptDeviceMonitor(StartedGStreamerDeviceMonitor&&started){", "#undefGST_CAT_DEFAULT")
            order = [
                "if(!started.monitor){return;}",
                "if(m_isTearingDown){gst_device_monitor_stop(started.monitor.get());return;}",
                "m_devices.clear();m_gstreamerDevices.clear();m_speakerDevices.clear();m_deviceMonitor=WTF::move(started.monitor);",
                "for(auto&device:started.devices){addDevice(WTF::move(device));}",
                "GRefPtrbus=adoptGRef(gst_device_monitor_get_bus(m_deviceMonitor.get()));gst_bus_add_watch(bus.get(),",
                "manager->addDevice(WTF::move(device));manager->deviceChanged();",
            ]
            positions = [adopt.index(item) for item in order]
            self.assertEqual(positions, sorted(positions))
            self.assertIn("voidGStreamerCaptureDeviceManager::stopMonitor(){m_monitorStartup=nullptr;if(!m_deviceMonitor){return;}", manager)
            changed = section(manager, "voidGStreamerCaptureDeviceManager::devicesChanged(){", "voidGStreamerCaptureDeviceManager::deviceWillBeRemoved(")
            self.assertTrue(changed.endswith("stopMonitor();m_gstreamerDevices.clear();m_devices.clear();m_speakerDevices.clear();}"))
            header = squeezed(patched[gstreamer + "GStreamerCaptureDeviceManager.h"])
            self.assertIn("voidteardownGStreamerCaptureDeviceManagers();classGStreamerDeviceMonitorStartup;structStartedGStreamerDeviceMonitor;", header)
            self.assertIn("constVector<CaptureDevice>&captureDevices()final;voidcomputeCaptureDevices(CompletionHandler<void()>&&)override;", header)
            self.assertIn("voidrefreshCaptureDevices();voidstartMonitorAsynchronously();voidfinishMonitorStartup(GStreamerDeviceMonitorStartup&);voidadoptDeviceMonitor(StartedGStreamerDeviceMonitor&&);", header)
            self.assertIn("RefPtr<GStreamerCapturer>m_defaultCapturer;RefPtr<GStreamerDeviceMonitorStartup>m_monitorStartup;Vector<CompletionHandler<void()>>m_pendingComputeCallbacks;", header)
            video = squeezed(patched[gstreamer + "GStreamerVideoCaptureSource.cpp"])
            chained = section(video, "voidGStreamerVideoCaptureDeviceManager::computeCaptureDevices(CompletionHandler<void()>&&callback){", "CaptureSourceOrErrorGStreamerVideoCaptureDeviceManager::createVideoCaptureSource(")
            self.assertEqual(chained, "voidGStreamerVideoCaptureDeviceManager::computeCaptureDevices(CompletionHandler<void()>&&callback){m_devices.clear();m_pipewireCaptureDeviceManager->computeCaptureDevices([this,callback=WTF::move(callback)]()mutable{if(!m_devices.isEmpty()){callback();return;}GStreamerCaptureDeviceManager::computeCaptureDevices(WTF::move(callback));});}")

    def test_libwebrtc_outgoing_audio_converts_whole_planar_buffers_without_writing(self):
        glue = "Source/WebCore/platform/mediastream/libwebrtc/gstreamer/"
        self.assertEqual(libwebrtc_only_paths(), set(build_wpe.libwebrtc_glue.PATCHES) - module_paths(build_wpe.GSTREAMER_CORRECTNESS_MODULES))
        self.assertIn("Source/cmake/GStreamerChecks.cmake", module_paths(build_wpe.GSTREAMER_CORRECTNESS_MODULES))
        patched = self.libwebrtc_patched()
        source = squeezed(patched[glue + "RealtimeOutgoingAudioSourceLibWebRTC.cpp"])
        original = squeezed(self.originals[glue + "RealtimeOutgoingAudioSourceLibWebRTC.cpp"])
        self.assertIn("GST_MAP_READWRITE", original)
        self.assertIn("gst_audio_converter_get_in_frames(", original)
        for absent in ("GST_MAP_READWRITE", "GST_AUDIO_CONVERTER_FLAG_IN_WRITABLE", "gst_adapter", "m_adapter", "gst_audio_converter_get_in_frames(", "m_audioBuffer.grow("):
            self.assertNotIn(absent, source)
        self.assertIn("namespaceWebCore{staticconstexprsize_tmaximumPendingChunks=50;", source)
        receive = section(source, "voidRealtimeOutgoingAudioSourceLibWebRTC::audioSamplesAvailable(", "voidRealtimeOutgoingAudioSourceLibWebRTC::pullAudioData()")
        order = [
            "Lockerlocker{m_conversionLock};",
            "gst_audio_info_is_equal(&m_inputStreamDescription",
            "m_convertedAudio.clear();",
            "gst_audio_converter_new(GST_AUDIO_CONVERTER_FLAG_NONE",
            "gst_audio_buffer_map(&captured,&m_inputStreamDescription,buffer,GST_MAP_READ)",
            "size_tinFrames=GST_AUDIO_BUFFER_N_SAMPLES(&captured);",
            "size_toutFrames=gst_audio_converter_get_out_frames(m_sampleConverter.get(),inFrames);",
            "m_convertedAudio.grow(offset+outFrames*outFrameSize);",
            "gst_audio_converter_samples(m_sampleConverter.get(),GST_AUDIO_CONVERTER_FLAG_NONE,captured.planes,inFrames,out,outFrames);",
            "gst_audio_buffer_unmap(&captured);",
            "if((!converted||isSilenced())&&!output.empty()){webkitGstAudioFormatFillSilence(",
            "if(m_convertedAudio.size()>maximumPendingSize){",
            "m_convertedAudio.removeAt(0,excess);",
            "callOnWebRTCSignalingThread",
        ]
        self.assertEqual([receive.index(item) for item in order], sorted(receive.index(item) for item in order))
        pull = section(source, "voidRealtimeOutgoingAudioSourceLibWebRTC::pullAudioData()", "boolRealtimeOutgoingAudioSourceLibWebRTC::isReachingBufferedAudioDataHighLimit()")
        order = [
            "while(true)",
            "Lockerlocker{m_conversionLock};",
            "if(!m_sampleConverter){return;}",
            "if(!outChunkSize||m_convertedAudio.size()<outChunkSize){return;}",
            "memcpySpan(m_audioBuffer.mutableSpan(),m_convertedAudio.span().first(outChunkSize));",
            "m_convertedAudio.removeAt(0,outChunkSize);",
            "channelCount=GST_AUDIO_INFO_CHANNELS(",
            "sendAudioFrames(",
        ]
        self.assertEqual([pull.index(item) for item in order], sorted(pull.index(item) for item in order))
        self.assertNotIn("gst_audio_converter_samples(", pull)
        header = squeezed(patched[glue + "RealtimeOutgoingAudioSourceLibWebRTC.h"])
        for member in ("m_sampleConverter", "m_inputStreamDescription", "m_outputStreamDescription", "m_convertedAudio"):
            self.assertIn(member + "WTF_GUARDED_BY_LOCK(m_conversionLock);", header)
        self.assertNotIn("m_adapter", header)
        self.assertLess(header.index("Lockm_conversionLock;"), header.index("m_sampleConverterWTF_GUARDED_BY_LOCK"))

    def test_each_backend_has_its_own_generator_identity(self):
        gstreamer = build_wpe.embedded_generator_sha256(build_wpe.correctness_modules("gstreamer"))
        libwebrtc = build_wpe.embedded_generator_sha256(build_wpe.correctness_modules("libwebrtc"))
        self.assertNotEqual(gstreamer, libwebrtc)
        self.assertEqual(gstreamer, build_wpe.embedded_generator_sha256(build_wpe.GSTREAMER_CORRECTNESS_MODULES))
        self.assertIs(build_wpe.EMBEDDED_CORRECTNESS_MODULES, build_wpe.GSTREAMER_CORRECTNESS_MODULES)

    def test_a_changed_pinned_source_is_rejected(self):
        drifted = dict(self.originals)
        path = "Source/WebKit/UIProcess/Launcher/glib/BubblewrapLauncher.cpp"
        drifted[path] = drifted[path].replace("ProcessLauncher::ProcessType::DBusProxy", "ProcessType::Renamed")
        with self.assertRaises(RuntimeError):
            embedded_patches(drifted, self.version)


if __name__ == "__main__":
    unittest.main()

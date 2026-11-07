async function edenDOMProbe() {
    const signaling = globalThis.edenInteropSignaling;
    const measurements = {sfuLoopback: null, answererSending: null, toleratedIceCharacters: null, liteOffer: null, rejections: null, meetRenegotiation: null, failures: []};
    const cleanup = [];
    const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));

    const requireCondition = (condition, name) => {
        if (!condition) {
            const error = new Error(name);
            error.probeCheck = name;
            throw error;
        }
    };

    const waitUntil = async (condition, name, timeout = 20000) => {
        const deadline = performance.now() + timeout;
        while (!condition()) {
            requireCondition(performance.now() < deadline, name);
            await delay(50);
        }
    };

    const recordFailure = (section, error) => {
        measurements.failures.push({section, error: error && error.name ? error.name : "Error", check: error && error.probeCheck ? error.probeCheck : String(error && error.message || error).slice(0, 160)});
    };

    const lines = sdp => sdp.split(/\r?\n/).filter(Boolean);

    const sections = sdp => {
        const session = [];
        const media = [];
        for (const line of lines(sdp)) {
            if (line.startsWith("m=")) {
                media.push([line]);
            } else if (media.length) {
                media[media.length - 1].push(line);
            } else {
                session.push(line);
            }
        }
        return {session, media};
    };

    const join = parsed => [...parsed.session, ...parsed.media.flat()].join("\r\n") + "\r\n";

    const waitForGathering = peer => peer.iceGatheringState === "complete" ? Promise.resolve() : new Promise(resolve => {
        const listener = () => {
            if (peer.iceGatheringState === "complete") {
                peer.removeEventListener("icegatheringstatechange", listener);
                resolve();
            }
        };
        peer.addEventListener("icegatheringstatechange", listener);
    });

    const createCanvasTrack = seed => {
        const canvas = document.createElement("canvas");
        canvas.width = 320;
        canvas.height = 180;
        document.body.appendChild(canvas);
        const context = canvas.getContext("2d");
        let frame = 0;
        const timer = setInterval(() => {
            frame += 1;
            context.fillStyle = `hsl(${(seed * 97 + frame * 7) % 360} 80% 50%)`;
            context.fillRect(0, 0, 320, 180);
            context.fillStyle = "white";
            context.fillRect((frame * 5) % 300, 40, 20, 100);
        }, 33);
        cleanup.push(() => clearInterval(timer));
        const stream = canvas.captureStream(30);
        return {track: stream.getVideoTracks()[0], stream};
    };

    const createToneTrack = frequency => {
        const context = new AudioContext();
        const oscillator = context.createOscillator();
        oscillator.frequency.value = frequency;
        const destination = context.createMediaStreamDestination();
        oscillator.connect(destination);
        oscillator.start();
        cleanup.push(() => context.close());
        return {track: destination.stream.getAudioTracks()[0], stream: destination.stream};
    };

    const inboundStats = async (peer, kind) => {
        const report = await peer.getStats();
        let packets = 0;
        let frames = 0;
        let width = 0;
        report.forEach(stat => {
            if (stat.type === "inbound-rtp" && stat.kind === kind) {
                packets += stat.packetsReceived || 0;
                frames += stat.framesDecoded || 0;
                width = Math.max(width, stat.frameWidth || 0);
            }
        });
        return {packets, frames, width};
    };

    const toMeetShapedAnswer = sdp => {
        const parsed = sections(sdp);
        parsed.session = parsed.session.filter(line => !line.startsWith("a=msid-semantic") && line !== "a=ice-lite");
        parsed.session.push("a=msid-semantic: WMS virtual-6666", "a=ice-lite");
        let candidateRewrites = 0;
        let legacyDataChannel = false;
        parsed.media = parsed.media.map((section, index) => {
            let result = section.map(line => {
                const candidate = line.match(/^a=candidate:(\S+) (\d+) (udp|tcp) (.*)$/i);
                if (candidate) {
                    candidateRewrites += 1;
                    return `a=candidate: ${candidate[2]} ${candidate[3]} ${candidate[4]}`;
                }
                return line;
            });
            if (result[0].startsWith("m=application")) {
                const port = result[0].split(" ")[1];
                result[0] = `m=application ${port} DTLS/SCTP 5000`;
                result = result.filter(line => !line.startsWith("a=sctp-port") && !line.startsWith("a=max-message-size"));
                result.push("a=sctpmap:5000 webrtc-datachannel 1024");
                legacyDataChannel = true;
            }
            if (index === 0) {
                result.splice(1, 0, "a=candidate: 1 ssltcp 2113932029 127.0.0.1 19313 typ host generation 0");
            }
            return result;
        });
        return {sdp: join(parsed), candidateRewrites, legacyDataChannel};
    };

    const runSfuLoopback = async () => {
        const client = new RTCPeerConnection({bundlePolicy: "max-bundle", rtcpMuxPolicy: "require"});
        const sfu = new RTCPeerConnection({bundlePolicy: "max-bundle", rtcpMuxPolicy: "require"});
        cleanup.push(() => client.close(), () => sfu.close());
        const result = {};
        const clientVideo = createCanvasTrack(1);
        const clientAudio = createToneTrack(440);
        client.addTransceiver(clientAudio.track, {direction: "sendonly", streams: [clientAudio.stream]});
        client.addTransceiver(clientVideo.track, {direction: "sendonly", streams: [clientVideo.stream]});
        client.addTransceiver("audio", {direction: "recvonly"});
        client.addTransceiver("audio", {direction: "recvonly"});
        client.addTransceiver("video", {direction: "recvonly"});
        const clientChannel = client.createDataChannel("session-control", {ordered: true});
        const remoteVideo = document.createElement("video");
        remoteVideo.muted = true;
        remoteVideo.autoplay = true;
        document.body.appendChild(remoteVideo);
        const diagnostics = {videoTracks: 0};
        result.diagnostics = diagnostics;
        client.addEventListener("track", event => {
            if (event.track.kind === "video") {
                diagnostics.videoTracks += 1;
                remoteVideo.srcObject = event.streams[0] || new MediaStream([event.track]);
                remoteVideo.play().catch(() => {});
            }
        });
        let sfuChannel = null;
        sfu.addEventListener("datachannel", event => {
            sfuChannel = event.channel;
            sfuChannel.addEventListener("message", message => sfuChannel.send("echo:" + message.data));
        });
        client.addEventListener("icecandidate", event => {
            if (event.candidate) {
                sfu.addIceCandidate(event.candidate).catch(error => recordFailure("sfuLoopback-trickle", error));
            }
        });
        await client.setLocalDescription(await client.createOffer());
        await sfu.setRemoteDescription(client.localDescription);
        const sfuVideo = createCanvasTrack(2);
        const sfuAudio = createToneTrack(660);
        const sfuTransceivers = sfu.getTransceivers();
        requireCondition(sfuTransceivers.length === 5, "SFU transceiver count changed");
        for (const transceiver of sfuTransceivers) {
            const kind = transceiver.receiver.track.kind;
            const offered = client.getTransceivers().find(item => item.mid === transceiver.mid);
            if (offered.direction === "recvonly") {
                transceiver.direction = "sendonly";
                await transceiver.sender.replaceTrack(kind === "video" ? sfuVideo.track : sfuAudio.track);
            } else {
                transceiver.direction = "recvonly";
            }
        }
        await sfu.setLocalDescription(await sfu.createAnswer());
        await waitForGathering(sfu);
        const shaped = toMeetShapedAnswer(sfu.localDescription.sdp);
        result.answerRewrites = {rewrittenAddresses: shaped.candidateRewrites, legacyDataChannel: shaped.legacyDataChannel};
        requireCondition(shaped.candidateRewrites > 0, "The SFU answer carried no candidates to rewrite");
        await client.setRemoteDescription({type: "answer", sdp: shaped.sdp});
        result.clientTransceivers = client.getTransceivers().map(transceiver => ({mid: transceiver.mid, direction: transceiver.currentDirection}));
        requireCondition(result.clientTransceivers.length === 5, "The legacy data channel section created an RTP transceiver");
        result.remoteDataSection = client.remoteDescription.sdp.includes("UDP/DTLS/SCTP webrtc-datachannel") ? "normalized" : "legacy";
        await waitUntil(() => client.connectionState === "connected", "The client did not connect to the SFU-shaped answer");
        await waitUntil(() => clientChannel.readyState === "open" && sfuChannel && sfuChannel.readyState === "open", "The data channel did not open");
        const echoed = new Promise(resolve => clientChannel.addEventListener("message", event => resolve(event.data), {once: true}));
        clientChannel.send("hello");
        result.echo = await Promise.race([echoed, delay(10000).then(() => null)]);
        requireCondition(result.echo === "echo:hello", "The data channel did not echo");
        const snapshot = async () => {
            const [clientVideoIn, sfuReport] = await Promise.all([inboundStats(client, "video"), sfu.getStats()]);
            let sent = 0;
            let encoded = 0;
            sfuReport.forEach(stat => {
                if (stat.type === "outbound-rtp" && stat.kind === "video") {
                    sent += stat.packetsSent || 0;
                    encoded += stat.framesEncoded || 0;
                }
            });
            Object.assign(diagnostics, {clientVideoIn, sfuVideoSent: sent, sfuVideoEncoded: encoded, readyState: remoteVideo.readyState, width: remoteVideo.videoWidth, paused: remoteVideo.paused, receivers: client.getReceivers().filter(receiver => receiver.track.kind === "video").map(receiver => ({state: receiver.track.readyState, muted: receiver.track.muted}))});
        };
        try {
            await waitUntil(() => remoteVideo.videoWidth === 320 && remoteVideo.readyState >= 2, "The client did not render the SFU video", 25000);
        } catch (error) {
            await snapshot();
            measurements.sfuDiagnostics = diagnostics;
            throw error;
        }
        let clientAudioIn = null;
        let sfuVideoIn = null;
        let sfuAudioIn = null;
        await waitUntil(() => {
            Promise.all([inboundStats(client, "audio"), inboundStats(sfu, "video"), inboundStats(sfu, "audio")]).then(([a, b, c]) => {
                clientAudioIn = a;
                sfuVideoIn = b;
                sfuAudioIn = c;
            });
            return clientAudioIn && sfuVideoIn && sfuAudioIn && clientAudioIn.packets > 20 && sfuVideoIn.frames > 5 && sfuAudioIn.packets > 20;
        }, "Media did not flow in both directions", 25000);
        result.media = {
            clientVideo: {width: remoteVideo.videoWidth, height: remoteVideo.videoHeight},
            clientAudioPackets: clientAudioIn.packets > 20,
            sfuVideoFrames: sfuVideoIn.frames > 5,
            sfuVideoWidth: sfuVideoIn.width,
            sfuAudioPackets: sfuAudioIn.packets > 20
        };
        result.connection = client.connectionState;
        result.passed = true;
        return result;
    };

    const connectPeers = (first, second) => {
        first.addEventListener("icecandidate", event => event.candidate && second.addIceCandidate(event.candidate).catch(error => recordFailure("answererSending-trickle", error)));
        second.addEventListener("icecandidate", event => event.candidate && first.addIceCandidate(event.candidate).catch(error => recordFailure("answererSending-trickle", error)));
    };

    const watchRemoteVideo = peer => {
        const video = document.createElement("video");
        video.muted = true;
        video.autoplay = true;
        document.body.appendChild(video);
        const info = {video, tracks: 0, streams: 0};
        peer.addEventListener("track", event => {
            if (event.track.kind !== "video") {
                return;
            }
            info.tracks += 1;
            info.streams = event.streams.length;
            video.srcObject = event.streams[0] || new MediaStream([event.track]);
            video.play().catch(() => {});
        });
        return info;
    };

    const negotiate = async (offerer, answerer, beforeAnswer) => {
        await offerer.setLocalDescription(await offerer.createOffer());
        await answerer.setRemoteDescription(offerer.localDescription);
        if (beforeAnswer) {
            await beforeAnswer();
        }
        await answerer.setLocalDescription(await answerer.createAnswer());
        await offerer.setRemoteDescription(answerer.localDescription);
    };

    const runAnswererSending = async () => {
        const result = {};
        {
            const offerer = new RTCPeerConnection();
            const answerer = new RTCPeerConnection();
            cleanup.push(() => offerer.close(), () => answerer.close());
            connectPeers(offerer, answerer);
            offerer.addTransceiver("video", {direction: "recvonly"});
            const remote = watchRemoteVideo(offerer);
            const source = createCanvasTrack(5);
            await negotiate(offerer, answerer, async () => answerer.addTrack(source.track, source.stream));
            result.addTrackAnswerAnnouncesStream = /a=ssrc:\d+ msid:/.test(answerer.localDescription.sdp) || /a=msid:/.test(answerer.localDescription.sdp);
            await waitUntil(() => remote.video.videoWidth === 320 && remote.video.readyState >= 2, "The answerer did not send a track added after the offer", 20000);
            result.addTrack = {width: remote.video.videoWidth, streams: remote.streams};
            requireCondition(result.addTrackAnswerAnnouncesStream && remote.streams === 1, "The answerer did not announce the stream of its added track");
        }
        {
            const offerer = new RTCPeerConnection();
            const answerer = new RTCPeerConnection();
            cleanup.push(() => offerer.close(), () => answerer.close());
            connectPeers(offerer, answerer);
            offerer.addTransceiver("video", {direction: "recvonly"});
            const remote = watchRemoteVideo(offerer);
            await negotiate(offerer, answerer, async () => {
                answerer.getTransceivers()[0].direction = "sendonly";
            });
            await waitUntil(() => answerer.connectionState === "connected", "The late replacement peers did not connect", 20000);
            const source = createCanvasTrack(6);
            await answerer.getTransceivers()[0].sender.replaceTrack(source.track);
            await waitUntil(() => remote.video.videoWidth === 320 && remote.video.readyState >= 2, "A track replaced after negotiation was not sent by the answerer", 20000);
            result.lateReplaceTrack = {width: remote.video.videoWidth};
        }
        result.passed = true;
        return result;
    };

    const fetchJSON = async (path, body) => {
        const response = await fetch(signaling.url + path, body ? {method: "POST", body: JSON.stringify(body), headers: {"content-type": "text/plain"}} : undefined);
        requireCondition(response.ok, "Signaling request failed: " + path);
        return response.json();
    };

    const localIceDetails = peer => {
        const text = peer.localDescription.sdp;
        const ufrag = (text.match(/a=ice-ufrag:(\S+)/) || [])[1];
        const pwd = (text.match(/a=ice-pwd:(\S+)/) || [])[1];
        const candidates = lines(text).filter(line => line.startsWith("a=candidate:") && / udp /i.test(line));
        return {ufrag, pwd, candidates};
    };

    const opusPayload = sdp => (sdp.match(/a=rtpmap:(\d+) opus\/48000\/2/i) || [])[1];

    const fingerprint = "sha-256 32:C0:9D:17:AD:99:E2:B8:2D:FD:5D:87:D4:36:44:4A:5B:3E:EE:EA:F2:BE:BE:72:3B:66:4C:F2:57:3C:0D:FF";

    const liteAnswer = (offerSdp, agent, pwdOverride) => {
        const mid = (offerSdp.match(/a=mid:(\S+)/) || [])[1];
        const payload = opusPayload(offerSdp);
        return [
            "v=0", "o=- 0 2 IN IP4 127.0.0.1", "s=-", "t=0 0",
            `a=group:BUNDLE ${mid}`, "a=msid-semantic: WMS", "a=ice-lite",
            `m=audio 9 UDP/TLS/RTP/SAVPF ${payload}`, "c=IN IP4 0.0.0.0", "a=rtcp:9 IN IP4 0.0.0.0",
            ...agent.candidates,
            `a=ice-ufrag:${agent.ufrag}`, `a=ice-pwd:${pwdOverride || agent.pwd}`,
            `a=fingerprint:${fingerprint}`, "a=setup:passive", `a=mid:${mid}`, "a=recvonly", "a=rtcp-mux",
            `a=rtpmap:${payload} opus/48000/2`, `a=fmtp:${payload} minptime=10;useinbandfec=1`
        ].join("\r\n") + "\r\n";
    };

    const trackIce = peer => {
        const states = [peer.iceConnectionState];
        peer.addEventListener("iceconnectionstatechange", () => states.push(peer.iceConnectionState));
        return states;
    };

    const runToleratedIceCharacters = async () => {
        const agent = await fetchJSON("/agent");
        requireCondition(/[=#_-]/.test(agent.pwd), "The test agent password has no tolerated character");
        const peer = new RTCPeerConnection();
        cleanup.push(() => peer.close());
        const states = trackIce(peer);
        peer.addTransceiver("audio", {direction: "sendonly"});
        await peer.setLocalDescription(await peer.createOffer());
        await waitForGathering(peer);
        const answer = liteAnswer(peer.localDescription.sdp, agent);
        await peer.setRemoteDescription({type: "answer", sdp: answer});
        const result = {};
        result.remoteDescriptionKeepsOriginal = peer.remoteDescription.sdp.includes(`a=ice-pwd:${agent.pwd}\r\n`) && peer.remoteDescription.sdp.includes(`a=ice-ufrag:${agent.ufrag}\r\n`);
        result.currentRemoteDescriptionKeepsOriginal = !!peer.currentRemoteDescription && peer.currentRemoteDescription.sdp.includes(`a=ice-pwd:${agent.pwd}\r\n`);
        requireCondition(result.remoteDescriptionKeepsOriginal && result.currentRemoteDescriptionKeepsOriginal, "The remote description does not report the original ICE credentials");
        await fetchJSON("/remote", localIceDetails(peer));
        await waitUntil(() => ["connected", "completed"].includes(peer.iceConnectionState), "ICE did not connect with the padded password", 20000);
        result.statesBeforeRenegotiation = [...states];
        const settled = states.length;
        await peer.setLocalDescription(await peer.createOffer());
        await peer.setRemoteDescription({type: "answer", sdp: liteAnswer(peer.localDescription.sdp, agent)});
        await delay(2500);
        result.statesAfterRenegotiation = states.slice(settled);
        result.stayedConnected = ["connected", "completed"].includes(peer.iceConnectionState) && result.statesAfterRenegotiation.every(state => state === "connected" || state === "completed");
        requireCondition(result.stayedConnected, "Renegotiating with unchanged credentials restarted ICE");
        const agentState = await fetchJSON("/state");
        result.agentConnected = agentState.connected === true;
        requireCondition(result.agentConnected, "The remote agent never reached a connected state");
        result.passed = true;
        return result;
    };

    const runLiteOffer = async () => {
        const offerParts = await fetchJSON("/offer");
        const peer = new RTCPeerConnection();
        cleanup.push(() => peer.close());
        const states = trackIce(peer);
        const offer = [
            "v=0", "o=- 1 1 IN IP4 127.0.0.1", "s=-", "t=0 0",
            "a=group:BUNDLE 0", "a=msid-semantic: WMS", "a=ice-lite",
            "m=audio 9 UDP/TLS/RTP/SAVPF 111", "c=IN IP4 0.0.0.0", "a=rtcp:9 IN IP4 0.0.0.0",
            ...offerParts.candidates,
            `a=ice-ufrag:${offerParts.ufrag}`, `a=ice-pwd:${offerParts.pwd}`,
            `a=fingerprint:${fingerprint}`, "a=setup:actpass", "a=mid:0", "a=sendonly", "a=rtcp-mux",
            "a=rtpmap:111 opus/48000/2", "a=fmtp:111 minptime=10;useinbandfec=1", "a=ssrc:7777 cname:lite"
        ].join("\r\n") + "\r\n";
        await peer.setRemoteDescription({type: "offer", sdp: offer});
        await peer.setLocalDescription(await peer.createAnswer());
        await waitForGathering(peer);
        await fetchJSON("/offer-answer", localIceDetails(peer));
        await waitUntil(() => ["connected", "completed"].includes(peer.iceConnectionState), "ICE did not connect as the answerer to a lite offer", 20000);
        let agentState = await fetchJSON("/offer-state");
        const deadline = performance.now() + 10000;
        while (!agentState.connected && performance.now() < deadline) {
            await delay(200);
            agentState = await fetchJSON("/offer-state");
        }
        const result = {states, agentConnected: agentState.connected === true, peerControlling: agentState.peerControlling === true, agentStates: agentState.states};
        requireCondition(result.agentConnected, "The lite offerer never reached a connected state");
        result.passed = true;
        return result;
    };

    const runRejections = async () => {
        const agent = await fetchJSON("/agent");
        const cases = {
            invalidCharacter: agent.pwd.replace(/[=#_-]/g, "!"),
            tooShort: "abc=defg",
            space: agent.pwd.slice(0, 10) + " " + agent.pwd.slice(11)
        };
        const result = {};
        for (const [name, pwd] of Object.entries(cases)) {
            const peer = new RTCPeerConnection();
            cleanup.push(() => peer.close());
            peer.addTransceiver("audio", {direction: "sendonly"});
            await peer.setLocalDescription(await peer.createOffer());
            try {
                await peer.setRemoteDescription({type: "answer", sdp: liteAnswer(peer.localDescription.sdp, agent, pwd)});
                result[name] = "accepted";
            } catch (error) {
                result[name] = error.name;
            }
        }
        result.passed = Object.values(result).every(value => value !== "accepted");
        requireCondition(result.passed, "An ICE password that libwebrtc rejects was accepted");
        return result;
    };

    const codecPayload = (sdp, kind, name) => {
        const block = sdp.split(/\r?\n(?=m=)/).find(item => item.startsWith("m=" + kind + " "));
        const match = block && block.match(new RegExp("a=rtpmap:(\\d+) " + name + "/", "i"));
        return match ? Number(match[1]) : null;
    };

    const firstCodecName = (sdp, kind) => {
        const block = sdp.split(/\r?\n(?=m=)/).find(item => item.startsWith("m=" + kind + " "));
        const payload = block && block.split(/\r?\n/)[0].split(" ")[3];
        const match = block && block.match(new RegExp("a=rtpmap:" + payload + " ([^/]+)/"));
        return match ? match[1] : null;
    };

    const preferFirst = (kind, codecName) => {
        const codecs = RTCRtpReceiver.getCapabilities(kind).codecs;
        const preferred = kind + "/" + codecName.toLowerCase();
        return [...codecs.filter(codec => codec.mimeType.toLowerCase() === preferred), ...codecs.filter(codec => codec.mimeType.toLowerCase() !== preferred)]
            .map(codec => Object.assign({}, codec, {mimeType: codec.mimeType.toLowerCase()}));
    };

    const inboundAbove = async (peer, kind, minimum, name) => {
        const deadline = performance.now() + 20000;
        for (;;) {
            const totals = await inboundStats(peer, kind);
            if (totals.packets > minimum) {
                return totals;
            }
            requireCondition(performance.now() < deadline, name);
            await delay(200);
        }
    };

    const runMeetRenegotiation = async () => {
        const result = {};
        const client = new RTCPeerConnection({bundlePolicy: "max-bundle"});
        const sfu = new RTCPeerConnection({bundlePolicy: "max-bundle"});
        cleanup.push(() => client.close(), () => sfu.close());
        client.addEventListener("icecandidate", event => event.candidate && sfu.addIceCandidate(event.candidate).catch(error => recordFailure("meetRenegotiation-trickle", error)));
        sfu.addEventListener("icecandidate", event => event.candidate && client.addIceCandidate(event.candidate).catch(error => recordFailure("meetRenegotiation-trickle", error)));
        const microphone = createToneTrack(440);
        const camera = createCanvasTrack(8);
        const audio = client.addTransceiver(microphone.track, {direction: "sendrecv", streams: [microphone.stream]});
        const video = client.addTransceiver(camera.track, {direction: "sendrecv", streams: [camera.stream]});
        client.addTransceiver("audio", {direction: "recvonly"});
        client.addTransceiver("video", {direction: "recvonly"});
        await negotiate(client, sfu);
        await waitUntil(() => client.connectionState === "connected", "The Meet-shaped peers did not connect", 20000);
        const opusBefore = codecPayload(client.localDescription.sdp, "audio", "opus");
        const videoCodec = firstCodecName(client.localDescription.sdp, "video");
        requireCondition(opusBefore !== null && videoCodec, "The first negotiation had no Opus or video codec");
        const audioBefore = await inboundAbove(sfu, "audio", 20, "Audio did not flow before renegotiation");
        const videoBefore = await inboundAbove(sfu, "video", 20, "Video did not flow before renegotiation");

        for (const [key, candidate] of [["ssltcpCandidate", "candidate:2130706429 1 ssltcp 2130706429 127.0.0.1 443 typ host tcptype passive generation 0"], ["malformedCandidate", "candidate:1 1 udp"]]) {
            try {
                await client.addIceCandidate({candidate, sdpMLineIndex: 0});
                result[key] = "accepted";
            } catch (error) {
                result[key] = error.name;
            }
        }

        audio.setCodecPreferences(preferFirst("audio", "opus"));
        video.setCodecPreferences(preferFirst("video", videoCodec));
        let offer = null;
        try {
            offer = await client.createOffer();
            result.renegotiationOffer = "created";
        } catch (error) {
            result.renegotiationOffer = error.name;
        }
        requireCondition(offer, "createOffer failed after setCodecPreferences");
        result.opusPayloadKept = codecPayload(offer.sdp, "audio", "opus") === opusBefore;
        result.videoCodecKept = firstCodecName(offer.sdp, "video") === videoCodec;
        await client.setLocalDescription(offer);
        await sfu.setRemoteDescription(offer);
        await sfu.setLocalDescription(await sfu.createAnswer());
        await client.setRemoteDescription(sfu.localDescription);
        await inboundAbove(sfu, "audio", audioBefore.packets + 50, "Audio stopped after renegotiation");
        await inboundAbove(sfu, "video", videoBefore.packets + 50, "Video stopped after renegotiation");
        result.mediaContinued = true;
        result.connection = client.connectionState;

        const replacement = createToneTrack(550);
        await audio.sender.replaceTrack(replacement.track);
        let replacedOffer = null;
        try {
            replacedOffer = await client.createOffer();
            result.offerAfterReplaceTrack = "created";
        } catch (error) {
            result.offerAfterReplaceTrack = error.name;
        }
        requireCondition(replacedOffer, "createOffer failed after replaceTrack");
        await client.setLocalDescription(replacedOffer);
        await sfu.setRemoteDescription(replacedOffer);
        await sfu.setLocalDescription(await sfu.createAnswer());
        await client.setRemoteDescription(sfu.localDescription);
        const afterReplace = await inboundStats(sfu, "audio");
        await inboundAbove(sfu, "audio", afterReplace.packets + 50, "Audio stopped after replaceTrack and renegotiation");
        result.replacedTrackFlowing = true;

        const plain = new RTCPeerConnection();
        const plainPeer = new RTCPeerConnection();
        cleanup.push(() => plain.close(), () => plainPeer.close());
        plain.addEventListener("icecandidate", event => event.candidate && plainPeer.addIceCandidate(event.candidate).catch(error => recordFailure("meetRenegotiation-balanced", error)));
        plainPeer.addEventListener("icecandidate", event => event.candidate && plain.addIceCandidate(event.candidate).catch(error => recordFailure("meetRenegotiation-balanced", error)));
        plain.addTransceiver(createToneTrack(660).track, {direction: "sendrecv"});
        plain.addTransceiver(createCanvasTrack(9).track, {direction: "sendrecv"});
        await plain.setLocalDescription(await plain.createOffer());
        result.balancedOffersBundle = /a=group:BUNDLE \S+ \S+/.test(plain.localDescription.sdp);
        await plainPeer.setRemoteDescription(plain.localDescription);
        await plainPeer.setLocalDescription(await plainPeer.createAnswer());
        await plain.setRemoteDescription(plainPeer.localDescription);
        await waitUntil(() => plain.connectionState === "connected", "Peers with the default bundle policy did not connect", 20000);
        await inboundAbove(plainPeer, "video", 20, "Media did not flow with the default bundle policy");
        result.balancedConnected = true;

        result.passed = result.ssltcpCandidate === "accepted" && result.malformedCandidate !== "accepted" && result.opusPayloadKept && result.videoCodecKept && result.replacedTrackFlowing && result.balancedOffersBundle;
        requireCondition(result.passed, "Meet-shaped renegotiation did not behave like libwebrtc");
        return result;
    };

    for (const [name, runner] of [["sfuLoopback", runSfuLoopback], ["answererSending", runAnswererSending], ["toleratedIceCharacters", runToleratedIceCharacters], ["liteOffer", runLiteOffer], ["rejections", runRejections], ["meetRenegotiation", runMeetRenegotiation]]) {
        try {
            measurements[name] = await runner();
        } catch (error) {
            recordFailure(name, error);
            measurements[name] = {passed: false};
        }
    }
    for (const task of cleanup.reverse()) {
        try {
            task();
        } catch (error) {
        }
    }
    return {probe: "webrtc-remote-interop", measurements, viewport: [innerWidth, innerHeight, devicePixelRatio], visibility: document.visibilityState};
}

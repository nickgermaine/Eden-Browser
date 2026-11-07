async function edenDOMProbe() {
    const measurements = {
        initialCases: [],
        directionOffers: [],
        stabilityOffers: [],
        failures: [],
        negotiatedStages: [],
        transmission: null,
        detachedPlayback: null,
        selectedOfferStage: null,
        senderParameterExposure: null
    };
    const peers = [];
    const sources = [];
    const mediaStreams = [];
    const remoteVideos = new Map();
    const detachedVideos = [];
    const pendingCandidates = new Map();
    const iceErrors = [];
    const canvasWidth = 320;
    const canvasHeight = 180;
    let active = true;
    let animationFrame = 0;
    let stage = "setup";
    let senderPeer = null;
    let receiverPeer = null;

    const fail = (name, error) => {
        measurements.failures.push({
            name,
            error: error && error.name ? error.name : "Error",
            check: error && error.probeCheck ? error.probeCheck : null,
            connections: [senderPeer, receiverPeer].map(peer => peer ? peer.connectionState : null),
            videos: Array.from(remoteVideos, ([mid, video]) => ({mid, width: video.videoWidth, height: video.videoHeight}))
        });
    };

    const requireCondition = (condition, name) => {
        if (!condition) {
            const error = new Error(name);
            error.probeCheck = name;
            throw error;
        }
    };

    const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));

    const waitUntil = async (condition, name, timeout = 20000) => {
        const deadline = performance.now() + timeout;
        while (!condition()) {
            if (performance.now() >= deadline) {
                requireCondition(false, name);
            }
            await delay(50);
        }
    };

    const flushCandidates = async peer => {
        const pending = pendingCandidates.get(peer) || [];
        pendingCandidates.set(peer, []);
        for (const candidate of pending) {
            await peer.addIceCandidate(candidate);
        }
    };

    const exchangeCandidates = (first, second) => {
        for (const [local, remote] of [[first, second], [second, first]]) {
            local.onicecandidate = event => {
                if (!event.candidate) {
                    return;
                }
                if (remote.remoteDescription) {
                    remote.addIceCandidate(event.candidate).catch(error => iceErrors.push(error.name || "Error"));
                    return;
                }
                if (!pendingCandidates.has(remote)) {
                    pendingCandidates.set(remote, []);
                }
                pendingCandidates.get(remote).push(event.candidate);
            };
        }
    };

    const negotiate = async (offerer, answerer, offer) => {
        await offerer.setLocalDescription(offer);
        await answerer.setRemoteDescription(offer);
        await flushCandidates(answerer);
        const answer = await answerer.createAnswer();
        await answerer.setLocalDescription(answer);
        await offerer.setRemoteDescription(answer);
        await flushCandidates(offerer);
        requireCondition(offerer.signalingState === "stable" && answerer.signalingState === "stable", stage + " negotiation did not return to stable");
    };

    const createCanvasSource = seed => {
        const canvas = document.createElement("canvas");
        canvas.width = canvasWidth;
        canvas.height = canvasHeight;
        document.body.appendChild(canvas);
        const context = canvas.getContext("2d");
        requireCondition(Boolean(context), "Canvas context is unavailable");
        const stream = canvas.captureStream(15);
        mediaStreams.push(stream);
        const track = stream.getVideoTracks()[0];
        requireCondition(Boolean(track), "Synthetic canvas track is unavailable");
        const source = {canvas, context, stream, track, seed, frame: 0};
        sources.push(source);
        return source;
    };

    const parseVideoSections = sdp => {
        const sections = [];
        let current = null;
        let sessionDirection = "sendrecv";
        for (const line of sdp.split(/\r?\n/)) {
            if (line.startsWith("m=")) {
                if (current) {
                    sections.push(current);
                }
                const fields = line.slice(2).split(/\s+/);
                current = {
                    kind: fields[0],
                    index: sections.length,
                    direction: sessionDirection,
                    mid: null,
                    sourceIds: new Set(),
                    fidGroups: [],
                    codecs: [],
                    formatPayloads: fields.slice(3).map(Number)
                };
                continue;
            }
            const direction = line.match(/^a=(sendrecv|sendonly|recvonly|inactive)$/);
            if (direction) {
                if (current) {
                    current.direction = direction[1];
                } else {
                    sessionDirection = direction[1];
                }
                continue;
            }
            if (!current || current.kind !== "video") {
                continue;
            }
            const mid = line.match(/^a=mid:([^\s]+)$/);
            if (mid) {
                current.mid = mid[1];
                continue;
            }
            const source = line.match(/^a=ssrc:(\d+)/);
            if (source) {
                current.sourceIds.add(source[1]);
                continue;
            }
            const fid = line.match(/^a=ssrc-group:FID\s+(\d+)\s+(\d+)$/);
            if (fid) {
                current.fidGroups.push({primary: fid[1], retransmission: fid[2]});
                continue;
            }
            const rtpmap = line.match(/^a=rtpmap:(\d+)\s+([^/]+)\/(\d+)(?:\/(\d+))?$/i);
            if (rtpmap) {
                current.codecs.push({
                    payload: Number(rtpmap[1]),
                    name: rtpmap[2],
                    clockRate: Number(rtpmap[3]),
                    channels: rtpmap[4] ? Number(rtpmap[4]) : null,
                    apt: null
                });
                continue;
            }
            const fmtp = line.match(/^a=fmtp:(\d+)\s+(.+)$/i);
            if (fmtp) {
                const codec = current.codecs.find(item => item.payload === Number(fmtp[1]));
                const apt = fmtp[2].match(/(?:^|;)\s*apt=(\d+)(?:;|$)/i);
                if (codec && apt) {
                    codec.apt = Number(apt[1]);
                }
            }
        }
        if (current) {
            sections.push(current);
        }
        return sections.filter(section => section.kind === "video").map(section => ({
            index: section.index,
            mid: section.mid,
            direction: section.direction,
            sourceIds: Array.from(section.sourceIds).sort((left, right) => Number(left) - Number(right)),
            fidGroups: section.fidGroups,
            codecs: section.codecs.filter(codec => section.formatPayloads.includes(codec.payload))
        }));
    };

    const publicSections = sections => sections.map(section => ({
        index: section.index,
        mid: section.mid,
        direction: section.direction,
        sourceIds: section.sourceIds,
        fidGroups: section.fidGroups,
        codecs: section.codecs
    }));

    const validateSections = (sections, expectedDirections, name, allowRetainedSources = false) => {
        requireCondition(sections.length === expectedDirections.length, name + " video media count changed");
        const mids = new Set();
        const allSourceIds = new Set();
        const primaryByMid = new Map();
        for (let index = 0; index < expectedDirections.length; index++) {
            const section = sections[index];
            requireCondition(Boolean(section.mid), name + " video MID is missing");
            requireCondition(!mids.has(section.mid), name + " video MID is duplicated");
            mids.add(section.mid);
            requireCondition(section.direction === expectedDirections[index], name + " video direction changed");
            const sends = section.direction === "sendonly" || section.direction === "sendrecv";
            if (!sends && (!allowRetainedSources || !section.sourceIds.length)) {
                requireCondition(section.sourceIds.length === 0, name + " receive-only video advertises a source");
                requireCondition(section.fidGroups.length === 0, name + " receive-only video advertises an FID group");
                continue;
            }
            requireCondition(section.fidGroups.length === 1, name + " sending video is missing one FID group");
            const fid = section.fidGroups[0];
            requireCondition(fid.primary !== fid.retransmission, name + " FID source IDs are equal");
            requireCondition(section.sourceIds.includes(fid.primary), name + " FID primary source is not advertised");
            requireCondition(section.sourceIds.includes(fid.retransmission), name + " FID retransmission source is not advertised");
            const rtx = section.codecs.find(codec => codec.name.toLowerCase() === "rtx");
            requireCondition(Boolean(rtx), name + " sending video is missing RTX codec metadata");
            requireCondition(
                section.codecs.some(codec => codec.payload === rtx.apt && codec.name.toLowerCase() !== "rtx"),
                name + " RTX codec has no associated primary codec"
            );
            primaryByMid.set(section.mid, fid.primary);
            for (const sourceId of section.sourceIds) {
                requireCondition(!allSourceIds.has(sourceId), name + " video source ID is reused across senders");
                allSourceIds.add(sourceId);
            }
        }
        return primaryByMid;
    };

    const comparePrimaryIds = (baseline, candidate, name, allowAdditional = false) => {
        requireCondition(allowAdditional || baseline.size === candidate.size, name + " sender count changed");
        for (const [mid, sourceId] of baseline) {
            requireCondition(candidate.get(mid) === sourceId, name + " primary source ID changed");
        }
    };

    const captureStableOffer = async (name, expectedDirections, stablePrimaryIds, allowAdditional = false) => {
        stage = name;
        const offer = await senderPeer.createOffer();
        const sections = parseVideoSections(offer.sdp);
        const record = {stage: name, applied: false, sections: publicSections(sections)};
        measurements.stabilityOffers.push(record);
        const primaryByMid = validateSections(sections, expectedDirections, name);
        if (stablePrimaryIds) {
            comparePrimaryIds(stablePrimaryIds, primaryByMid, name, allowAdditional);
        }
        return {offer, sections, primaryByMid, record};
    };

    const ssrcExposure = encoding => {
        const present = Object.prototype.hasOwnProperty.call(encoding, "ssrc");
        return {present, value: present ? encoding.ssrc : null};
    };

    const senderSsrcChecks = [];

    const checkSenderSsrc = (name, encoding, advertisedPrimary) => {
        requireCondition(Boolean(encoding), name + " sender encoding is unavailable");
        const current = ssrcExposure(encoding);
        senderSsrcChecks.push({stage: name, present: current.present, value: current.present ? current.value : null});
        requireCondition(current.present === senderSsrcChecks[0].present, name + " changed whether the sender SSRC parameter is exposed");
        if (!current.present || current.value === 0) {
            requireCondition(senderSsrcChecks.every(item => !item.value), name + " cleared the negotiated sender SSRC parameter");
            return;
        }
        requireCondition(Boolean(advertisedPrimary) && String(current.value) === advertisedPrimary, name + " exposed a sender SSRC that is not the advertised primary");
    };

    const codecForRtp = (report, item) => {
        if (!item.codecId) {
            return null;
        }
        const codec = report.get(item.codecId);
        return codec && codec.mimeType ? codec.mimeType.toLowerCase() : null;
    };

    const primaryRtpStat = (report, type) => {
        const items = [];
        report.forEach(item => {
            if (item.type !== type || (item.kind || item.mediaType) !== "video" || item.ssrc === undefined) {
                return;
            }
            const codec = codecForRtp(report, item);
            if (!codec || !codec.endsWith("/rtx")) {
                items.push({ssrc: String(item.ssrc), codec});
            }
        });
        return items.length === 1 ? items[0] : null;
    };

    const makeLocalPeer = () => {
        const peer = new RTCPeerConnection({iceServers: [], bundlePolicy: "max-bundle"});
        peers.push(peer);
        return peer;
    };

    const runInitialCase = async scenario => {
        const record = {name: scenario.name, direction: scenario.direction, attached: scenario.attached, offers: []};
        measurements.initialCases.push(record);
        let peer = null;
        let answerer = null;
        try {
            stage = scenario.name;
            peer = makeLocalPeer();
            answerer = makeLocalPeer();
            peer.addTransceiver("audio", {direction: scenario.direction});
            let transceiver;
            if (scenario.attached) {
                const source = createCanvasSource(scenario.seed);
                transceiver = peer.addTransceiver(source.track, {direction: scenario.direction});
            } else {
                transceiver = peer.addTransceiver("video", {direction: scenario.direction});
            }
            peer.createDataChannel("eden-source-identity");
            const firstOffer = await peer.createOffer();
            const firstSections = parseVideoSections(firstOffer.sdp);
            const firstRecord = {stage: "initial", applied: false, sections: publicSections(firstSections)};
            record.offers.push(firstRecord);
            const expected = [scenario.direction];
            const firstPrimary = validateSections(firstSections, expected, scenario.name);
            const firstMid = firstSections[0].mid;
            if (scenario.direction === "recvonly") {
                requireCondition(firstPrimary.size === 0, scenario.name + " receive-only offer has a primary source");
            } else {
                requireCondition(firstPrimary.has(firstMid), scenario.name + " first offer has no primary source");
            }
            await negotiate(peer, answerer, firstOffer);
            firstRecord.applied = true;
            const repeatedOffer = await peer.createOffer();
            const repeatedSections = parseVideoSections(repeatedOffer.sdp);
            record.offers.push({stage: "repeated", applied: false, sections: publicSections(repeatedSections)});
            const repeatedPrimary = validateSections(repeatedSections, expected, scenario.name + " repeated");
            comparePrimaryIds(firstPrimary, repeatedPrimary, scenario.name + " repeated");
            if (scenario.direction === "recvonly") {
                let sendingPrimary = null;
                for (const direction of ["sendonly", "recvonly", "sendrecv", "inactive", "sendonly"]) {
                    transceiver.direction = direction;
                    const offer = await peer.createOffer();
                    const sections = parseVideoSections(offer.sdp);
                    const directionRecord = {direction, applied: false, sections: publicSections(sections)};
                    measurements.directionOffers.push(directionRecord);
                    const primary = validateSections(sections, [direction], "direction-" + direction, true);
                    if (primary.size) {
                        if (sendingPrimary) {
                            comparePrimaryIds(sendingPrimary, primary, "direction-" + direction);
                        } else {
                            sendingPrimary = primary;
                        }
                    }
                    await negotiate(peer, answerer, offer);
                    directionRecord.applied = true;
                }
            }
            if (transceiver.sender.track) {
                requireCondition(transceiver.sender.track.kind === "video", scenario.name + " attached a non-video track");
            }
        } finally {
            for (const item of [peer, answerer]) {
                if (item) {
                    item.close();
                }
            }
        }
    };

    try {
        document.body.replaceChildren();
        document.body.style.margin = "0";
        document.body.style.display = "flex";
        document.body.style.flexWrap = "wrap";
        animationFrame = requestAnimationFrame(function paint() {
            if (!active) {
                return;
            }
            for (const source of sources) {
                source.context.fillStyle = (source.frame + source.seed) % 2 ? "#124f9c" : "#d94b37";
                source.context.fillRect(0, 0, canvasWidth, canvasHeight);
                source.context.fillStyle = "#f4e8b8";
                source.context.fillRect((source.frame * 7) % (canvasWidth - 40), 30, 40, 40);
                source.frame++;
            }
            animationFrame = requestAnimationFrame(paint);
        });

        for (const scenario of [
            {name: "trackless-sendonly", direction: "sendonly", attached: false, seed: 1},
            {name: "trackless-sendrecv", direction: "sendrecv", attached: false, seed: 2},
            {name: "canvas-sendonly", direction: "sendonly", attached: true, seed: 3},
            {name: "canvas-sendrecv", direction: "sendrecv", attached: true, seed: 4},
            {name: "receiveonly", direction: "recvonly", attached: false, seed: 5}
        ]) {
            await runInitialCase(scenario);
        }

        stage = "stability-setup";
        const secondSource = createCanvasSource(7);
        const replacementSource = createCanvasSource(8);
        senderPeer = makeLocalPeer();
        senderPeer.createDataChannel("eden-source-identity");
        const firstTransceiver = senderPeer.addTransceiver("video", {direction: "sendonly"});
        senderPeer.addTransceiver("audio", {direction: "sendrecv"});
        checkSenderSsrc("before-offer", firstTransceiver.sender.getParameters().encodings[0], null);

        receiverPeer = makeLocalPeer();
        exchangeCandidates(senderPeer, receiverPeer);
        receiverPeer.ontrack = event => {
            if (event.track.kind !== "video") {
                return;
            }
            const remoteVideo = document.createElement("video");
            remoteVideo.autoplay = true;
            remoteVideo.muted = true;
            remoteVideo.playsInline = true;
            remoteVideo.srcObject = new MediaStream([event.track]);
            remoteVideos.set(event.transceiver.mid, remoteVideo);
            document.body.appendChild(remoteVideo);
            remoteVideo.play().catch(() => {});
        };

        const initial = await captureStableOffer("initial", ["sendonly"], null);
        stage = "initial-negotiation";
        await negotiate(senderPeer, receiverPeer, initial.offer);
        initial.record.applied = true;
        measurements.negotiatedStages.push("initial");
        const negotiatedPrimary = initial.primaryByMid.get(firstTransceiver.mid);
        requireCondition(Boolean(negotiatedPrimary), "Initial negotiation has no primary source for the sender");
        checkSenderSsrc("initial-negotiation", firstTransceiver.sender.getParameters().encodings[0], negotiatedPrimary);
        await captureStableOffer("repeated", ["sendonly"], initial.primaryByMid);

        stage = "codec-preferences";
        const capabilities = RTCRtpSender.getCapabilities("video");
        requireCondition(Boolean(capabilities) && Array.isArray(capabilities.codecs), "Video codec capabilities are unavailable");
        const vp8 = capabilities.codecs.find(codec => codec.mimeType && codec.mimeType.toLowerCase() === "video/vp8");
        requireCondition(Boolean(vp8), "VP8 video codec capability is unavailable");
        const retransmission = capabilities.codecs.filter(codec => codec.mimeType && codec.mimeType.toLowerCase() === "video/rtx");
        requireCondition(retransmission.length > 0, "RTX video codec capability is unavailable");
        const preferredCodecs = [vp8, ...retransmission];
        firstTransceiver.setCodecPreferences(preferredCodecs);
        const codecOffer = await captureStableOffer("codec-preferences", ["sendonly"], initial.primaryByMid);
        comparePrimaryIds(initial.primaryByMid, codecOffer.primaryByMid, "Codec preferences");

        stage = "sender-parameters";
        const parameters = firstTransceiver.sender.getParameters();
        requireCondition(Array.isArray(parameters.encodings) && parameters.encodings.length > 0, "Sender encodings are unavailable");
        checkSenderSsrc("before-parameter-update", parameters.encodings[0], negotiatedPrimary);
        parameters.encodings[0].maxBitrate = 300000;
        await firstTransceiver.sender.setParameters(parameters);
        checkSenderSsrc("parameter-update", firstTransceiver.sender.getParameters().encodings[0], negotiatedPrimary);
        const parameterOffer = await captureStableOffer("sender-parameters", ["sendonly"], initial.primaryByMid);
        comparePrimaryIds(initial.primaryByMid, parameterOffer.primaryByMid, "Sender parameters");

        stage = "replace-track";
        await firstTransceiver.sender.replaceTrack(replacementSource.track);
        checkSenderSsrc("replace-track", firstTransceiver.sender.getParameters().encodings[0], negotiatedPrimary);
        const replacementOffer = await captureStableOffer("replace-track", ["sendonly"], initial.primaryByMid);
        comparePrimaryIds(initial.primaryByMid, replacementOffer.primaryByMid, "replaceTrack");

        stage = "multiple-sender-identities";
        const multiplePeer = makeLocalPeer();
        try {
            const first = multiplePeer.addTransceiver("video", {direction: "sendonly"});
            const second = multiplePeer.addTransceiver(secondSource.track, {direction: "sendrecv"});
            first.setCodecPreferences(preferredCodecs);
            second.setCodecPreferences(preferredCodecs);
            const offer = await multiplePeer.createOffer();
            const sections = parseVideoSections(offer.sdp);
            validateSections(sections, ["sendonly", "sendrecv"], "multiple-sender-identities");
            measurements.multipleSenderOffer = {sections: publicSections(sections)};
        } finally {
            multiplePeer.close();
        }
        const finalOffer = replacementOffer;
        measurements.selectedOfferStage = "replace-track";

        stage = "negotiation";
        await negotiate(senderPeer, receiverPeer, finalOffer.offer);
        finalOffer.record.applied = true;
        measurements.negotiatedStages.push("replace-track");
        const renegotiated = await captureStableOffer("renegotiated", ["sendonly"], initial.primaryByMid);
        comparePrimaryIds(initial.primaryByMid, renegotiated.primaryByMid, "Renegotiation");
        await waitUntil(
            () => senderPeer.connectionState === "connected" && receiverPeer.connectionState === "connected",
            "Local peer connection did not connect"
        );

        const senderTransceivers = [firstTransceiver];
        const senderMidMap = new Map(senderPeer.getTransceivers().map(transceiver => [transceiver.mid, transceiver]));
        const receiverMidMap = new Map(receiverPeer.getTransceivers().map(transceiver => [transceiver.mid, transceiver]));
        const selectedPrimaryByMid = finalOffer.primaryByMid;
        const expectedMids = Array.from(selectedPrimaryByMid.keys());
        await waitUntil(
            () => expectedMids.every(mid => {
                const remoteVideo = remoteVideos.get(mid);
                return remoteVideo && remoteVideo.videoWidth === canvasWidth && remoteVideo.videoHeight === canvasHeight;
            }),
            "Synthetic video geometry was not received",
            30000
        );
        let pairedStats = null;
        const statsDeadline = performance.now() + 15000;
        while (performance.now() < statsDeadline) {
            const current = [];
            let complete = true;
            for (const transceiver of senderTransceivers) {
                const mid = transceiver.mid;
                const sourceTransceiver = senderMidMap.get(mid);
                const remoteTransceiver = receiverMidMap.get(mid);
                if (!sourceTransceiver || !remoteTransceiver) {
                    complete = false;
                    break;
                }
                const outboundReport = await sourceTransceiver.sender.getStats();
                const inboundReport = await remoteTransceiver.receiver.getStats();
                const outbound = primaryRtpStat(outboundReport, "outbound-rtp");
                const inbound = primaryRtpStat(inboundReport, "inbound-rtp");
                let decodedFrames = 0;
                inboundReport.forEach(item => {
                    if (item.type === "inbound-rtp" && (item.kind || item.mediaType) === "video") {
                        decodedFrames = Math.max(decodedFrames, item.framesDecoded || 0);
                    }
                });
                if (!outbound || !inbound || !decodedFrames) {
                    complete = false;
                    break;
                }
                const expectedPrimary = selectedPrimaryByMid.get(mid);
                requireCondition(Boolean(expectedPrimary), "Selected offer has no primary source ID");
                requireCondition(outbound.ssrc === expectedPrimary, "Outbound RTP source differs from the advertised primary");
                requireCondition(inbound.ssrc === expectedPrimary, "Inbound RTP source differs from the advertised primary");
                const remoteVideo = remoteVideos.get(mid);
                current.push({
                    mid,
                    advertisedPrimary: expectedPrimary,
                    outboundSsrc: outbound.ssrc,
                    outboundCodec: outbound.codec,
                    inboundSsrc: inbound.ssrc,
                    inboundCodec: inbound.codec,
                    geometry: {width: remoteVideo.videoWidth, height: remoteVideo.videoHeight},
                    framesDecoded: decodedFrames
                });
            }
            if (complete && current.length === senderTransceivers.length) {
                pairedStats = current;
                break;
            }
            await delay(100);
        }
        requireCondition(Boolean(pairedStats), "Paired peer RTP statistics were not received");

        stage = "detached-muted-playback";
        const receivedTrack = receiverMidMap.get(firstTransceiver.mid).receiver.track;
        requireCondition(Boolean(receivedTrack) && receivedTrack.kind === "video", "Received video track is unavailable");
        const detachedVideo = document.createElement("video");
        detachedVideo.muted = true;
        detachedVideo.playsInline = true;
        detachedVideo.srcObject = new MediaStream([receivedTrack]);
        detachedVideos.push(detachedVideo);
        const detachedPlayback = detachedVideo.play();
        await waitUntil(
            () => detachedVideo.readyState >= HTMLMediaElement.HAVE_CURRENT_DATA && detachedVideo.videoWidth === canvasWidth && detachedVideo.videoHeight === canvasHeight,
            "Detached muted remote video did not start",
            15000
        );
        await detachedPlayback;
        requireCondition(!detachedVideo.isConnected, "Detached remote video was attached to the document");
        measurements.detachedPlayback = {
            connected: detachedVideo.isConnected,
            muted: detachedVideo.muted,
            paused: detachedVideo.paused,
            readyState: detachedVideo.readyState,
            geometry: {width: detachedVideo.videoWidth, height: detachedVideo.videoHeight}
        };
        requireCondition(iceErrors.length === 0, "A local ICE candidate was rejected");
        checkSenderSsrc("final", firstTransceiver.sender.getParameters().encodings[0], negotiatedPrimary);
        measurements.senderParameterExposure = {stable: true, present: senderSsrcChecks[0].present, stages: senderSsrcChecks};
        measurements.transmission = {selectedOfferStage: measurements.selectedOfferStage, senders: pairedStats};
    } catch (error) {
        fail(stage, error);
        measurements.transportStats = [];
        for (const peer of [senderPeer, receiverPeer]) {
            if (!peer) {
                continue;
            }
            const report = await peer.getStats();
            measurements.transportStats.push(Array.from(report.values())
                .filter(item => item.type === "outbound-rtp" || item.type === "inbound-rtp")
                .map(item => ({type: item.type, kind: item.kind, ssrc: item.ssrc, packetsSent: item.packetsSent, packetsReceived: item.packetsReceived})));
        }
    } finally {
        active = false;
        if (animationFrame) {
            cancelAnimationFrame(animationFrame);
        }
        for (const peer of peers) {
            peer.close();
        }
        for (const stream of mediaStreams) {
            stream.getTracks().forEach(track => track.stop());
        }
        remoteVideos.forEach(remoteVideo => {
            remoteVideo.pause();
            remoteVideo.srcObject = null;
        });
        for (const detachedVideo of detachedVideos) {
            detachedVideo.pause();
            detachedVideo.srcObject = null;
        }
        document.body.replaceChildren();
        document.body.style.cssText = "";
    }
    return {
        probe: "webrtc-source-identity",
        measurements,
        viewport: [innerWidth, innerHeight, devicePixelRatio],
        visibility: document.visibilityState
    };
}

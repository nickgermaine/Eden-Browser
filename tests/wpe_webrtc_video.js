async function edenDOMProbe() {
    const measurements = {failures: []};
    const sources = [];
    const peers = [];
    const videos = [];
    let painting = true;
    let stage = "setup";

    const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));

    const makeSource = (width, height, color) => {
        const canvas = document.createElement("canvas");
        canvas.width = width;
        canvas.height = height;
        document.body.appendChild(canvas);
        const source = {canvas, context: canvas.getContext("2d"), color, frame: 0};
        source.track = canvas.captureStream(30).getVideoTracks()[0];
        sources.push(source);
        return source;
    };

    const paint = () => {
        if (!painting) {
            return;
        }
        for (const source of sources) {
            const {context, canvas} = source;
            if (source.color) {
                context.fillStyle = source.color;
                context.fillRect(0, 0, canvas.width, canvas.height);
                context.fillStyle = "#808080";
                context.fillRect((source.frame * 5) % (canvas.width - 24), 4, 24, 24);
            } else {
                context.fillStyle = `hsl(${(source.frame * 7) % 360}, 80%, 50%)`;
                context.fillRect(0, 0, canvas.width, canvas.height);
                for (let index = 0; index < 40; index++) {
                    context.fillStyle = `hsl(${(index * 37 + source.frame * 3) % 360}, 90%, ${30 + (index % 5) * 10}%)`;
                    context.fillRect((index * 97 + source.frame * 11) % canvas.width, (index * 53 + source.frame * 5) % canvas.height, 60, 40);
                }
            }
            source.frame++;
        }
        requestAnimationFrame(paint);
    };

    const makePeer = () => {
        const peer = new RTCPeerConnection();
        peers.push(peer);
        return peer;
    };

    const mungeSimulcast = (answerSdp, offerSdp) => {
        const simulcast = (offerSdp.match(/a=simulcast:send (\S+)/) || [])[1];
        if (!simulcast) {
            return answerSdp;
        }
        const rids = (offerSdp.match(/a=rid:\S+ send/g) || []).map(line => line.replace(" send", " recv"));
        const lines = answerSdp.split("\r\n");
        const video = lines.findIndex(line => line.startsWith("m=video"));
        let end = lines.findIndex((line, index) => index > video && line.startsWith("m="));
        if (end < 0) {
            end = lines[lines.length - 1] === "" ? lines.length - 1 : lines.length;
        }
        lines.splice(end, 0, ...rids, "a=simulcast:recv " + simulcast);
        return lines.join("\r\n");
    };

    const connect = async (sender, receiver, mungeAnswer) => {
        sender.onicecandidate = event => event.candidate && receiver.addIceCandidate(event.candidate).catch(() => {});
        receiver.onicecandidate = event => event.candidate && sender.addIceCandidate(event.candidate).catch(() => {});
        const offer = await sender.createOffer();
        await sender.setLocalDescription(offer);
        await receiver.setRemoteDescription(offer);
        const answer = await receiver.createAnswer();
        await receiver.setLocalDescription(answer);
        await sender.setRemoteDescription(mungeAnswer ? {type: "answer", sdp: mungeAnswer(answer.sdp, offer.sdp)} : answer);
        for (let attempt = 0; attempt < 200 && sender.connectionState !== "connected"; attempt++) {
            await delay(50);
        }
        return sender.connectionState;
    };

    const outbound = async peer => {
        const report = await peer.getStats();
        const rows = [];
        report.forEach(item => {
            if (item.type === "outbound-rtp" && item.kind === "video") {
                const codec = item.codecId && report.get(item.codecId);
                rows.push({
                    rid: item.rid || null,
                    ssrc: item.ssrc,
                    framesEncoded: item.framesEncoded || 0,
                    frameWidth: item.frameWidth || 0,
                    frameHeight: item.frameHeight || 0,
                    bytesSent: item.bytesSent || 0,
                    keyFramesEncoded: item.keyFramesEncoded || 0,
                    targetBitrate: item.targetBitrate || 0,
                    scalabilityMode: item.scalabilityMode || null,
                    encoderImplementation: item.encoderImplementation || null,
                    codec: codec ? codec.mimeType : null
                });
            }
        });
        return rows.sort((left, right) => (left.rid || "").localeCompare(right.rid || ""));
    };

    const inbound = async peer => {
        const report = await peer.getStats();
        const rows = [];
        report.forEach(item => {
            if (item.type === "inbound-rtp" && item.kind === "video") {
                const codec = item.codecId && report.get(item.codecId);
                rows.push({
                    framesDecoded: item.framesDecoded || 0,
                    frameWidth: item.frameWidth || 0,
                    frameHeight: item.frameHeight || 0,
                    decoderImplementation: item.decoderImplementation || null,
                    codec: codec ? codec.mimeType : null
                });
            }
        });
        return rows;
    };

    const kilobitsPerSecond = async (peer, seconds) => {
        const before = (await outbound(peer)).reduce((sum, row) => sum + row.bytesSent, 0);
        await delay(seconds * 1000);
        const after = (await outbound(peer)).reduce((sum, row) => sum + row.bytesSent, 0);
        return Math.round((after - before) * 8 / seconds / 1000);
    };

    const codecsFor = (mimeType, fmtp) => {
        const codecs = RTCRtpSender.getCapabilities("video").codecs;
        const chosen = codecs.filter(codec => codec.mimeType.toLowerCase() === mimeType.toLowerCase() && (!fmtp || (codec.sdpFmtpLine || "").includes(fmtp)));
        return chosen.length ? [...chosen, ...codecs.filter(codec => codec.mimeType.toLowerCase() === "video/rtx")] : null;
    };

    const waitForDecodedFrames = async (peer, minimum, timeout) => {
        const deadline = performance.now() + timeout;
        let rows = [];
        while (performance.now() < deadline) {
            rows = await inbound(peer);
            if (rows.length && rows.every(row => row.framesDecoded >= minimum)) {
                break;
            }
            await delay(100);
        }
        return rows;
    };

    const run = async (name, body) => {
        stage = name;
        const firstPeer = peers.length;
        const firstSource = sources.length;
        const firstVideo = videos.length;
        try {
            measurements[name] = await body();
        } catch (error) {
            measurements.failures.push({name, error: String(error)});
        } finally {
            for (const peer of peers.splice(firstPeer)) {
                peer.close();
            }
            for (const source of sources.splice(firstSource)) {
                source.track.stop();
                source.canvas.remove();
            }
            for (const video of videos.splice(firstVideo)) {
                video.pause();
                video.srcObject = null;
            }
        }
    };

    try {
        document.body.replaceChildren();
        requestAnimationFrame(paint);

        await run("capabilities", async () => ({
            send: RTCRtpSender.getCapabilities("video").codecs.map(codec => codec.mimeType + (codec.sdpFmtpLine ? " " + codec.sdpFmtpLine : "")),
            receive: RTCRtpReceiver.getCapabilities("video").codecs.map(codec => codec.mimeType + (codec.sdpFmtpLine ? " " + codec.sdpFmtpLine : ""))
        }));

        await run("simulcast", async () => {
            const source = makeSource(1280, 720);
            const sender = makePeer();
            const receiver = makePeer();
            const transceiver = sender.addTransceiver(source.track, {direction: "sendonly", sendEncodings: [{rid: "q", scaleResolutionDownBy: 4}, {rid: "h", scaleResolutionDownBy: 2}, {rid: "f"}]});
            transceiver.setCodecPreferences(codecsFor("video/VP8"));
            const connection = await connect(sender, receiver, mungeSimulcast);
            await delay(8000);
            return {connection, layers: await outbound(sender), kbps: await kilobitsPerSecond(sender, 3)};
        });

        for (const [name, mimeType, fmtp] of [["cappedVP8", "video/VP8"], ["cappedVP9", "video/VP9", "profile-id=0"], ["cappedAV1", "video/AV1"], ["cappedH264", "video/H264", "42e01f"]]) {
            await run(name, async () => {
                const codecs = codecsFor(mimeType, fmtp);
                if (!codecs) {
                    return {unsupported: true};
                }
                const source = makeSource(1280, 720);
                const sender = makePeer();
                const receiver = makePeer();
                const transceiver = sender.addTransceiver(source.track, {direction: "sendonly"});
                transceiver.setCodecPreferences(codecs);
                const connection = await connect(sender, receiver);
                await delay(3000);
                const uncappedKbps = await kilobitsPerSecond(sender, 3);
                const parameters = transceiver.sender.getParameters();
                parameters.encodings[0].maxBitrate = 300000;
                await transceiver.sender.setParameters(parameters);
                await delay(3000);
                const cappedStart = (await outbound(sender))[0];
                const cappedKbps = await kilobitsPerSecond(sender, 4);
                const cappedEnd = (await outbound(sender))[0];
                const restored = transceiver.sender.getParameters();
                restored.encodings[0].maxBitrate = 2500000;
                await transceiver.sender.setParameters(restored);
                source.canvas.width = 640;
                source.canvas.height = 360;
                const followsSource = row => row && row.frameWidth > 0 && row.frameWidth <= 640 && row.frameHeight <= 360 && Math.abs(row.frameWidth / row.frameHeight - 16 / 9) < 0.05;
                const resizeDeadline = performance.now() + 8000;
                let resizedOutbound = (await outbound(sender))[0];
                while (performance.now() < resizeDeadline && !followsSource(resizedOutbound)) {
                    await delay(200);
                    resizedOutbound = (await outbound(sender))[0];
                }
                await delay(1000);
                const decodedAtResize = (await inbound(receiver))[0];
                await delay(2000);
                const resized = await inbound(receiver);
                const framesAfterResize = resized[0] && decodedAtResize ? resized[0].framesDecoded - decodedAtResize.framesDecoded : 0;
                transceiver.sender.track.enabled = false;
                await delay(1000);
                const mutedBefore = (await inbound(receiver))[0];
                await delay(3000);
                const mutedAfter = (await inbound(receiver))[0];
                const unmutedStart = (await outbound(sender))[0];
                transceiver.sender.track.enabled = true;
                await delay(2000);
                const unmuted = (await inbound(receiver))[0];
                const unmutedEnd = (await outbound(sender))[0];
                return {
                    connection,
                    uncappedKbps,
                    cappedKbps,
                    outbound: await outbound(sender),
                    resized: resized[0] || null,
                    resizedOutbound: resizedOutbound || null,
                    framesAfterResize,
                    mutedFrames: mutedBefore && mutedAfter ? mutedAfter.framesDecoded - mutedBefore.framesDecoded : null,
                    unmutedFrames: unmuted && mutedAfter ? unmuted.framesDecoded - mutedAfter.framesDecoded : null,
                    cappedKeyFrames: cappedStart && cappedEnd ? cappedEnd.keyFramesEncoded - cappedStart.keyFramesEncoded : null,
                    unmutedKeyFrames: unmutedStart && unmutedEnd ? unmutedEnd.keyFramesEncoded - unmutedStart.keyFramesEncoded : null
                };
            });
        }

        for (const [name, mimeType, fmtp] of [["decodeVP9Profile2", "video/VP9", "profile-id=2"], ["decodeAV1", "video/AV1"]]) {
            await run(name, async () => {
                const codecs = codecsFor(mimeType, fmtp);
                if (!codecs) {
                    return {unsupported: true};
                }
                const source = makeSource(640, 360);
                const sender = makePeer();
                const receiver = makePeer();
                const transceiver = sender.addTransceiver(source.track, {direction: "sendonly"});
                transceiver.setCodecPreferences(codecs);
                const connection = await connect(sender, receiver);
                return {connection, inbound: await waitForDecodedFrames(receiver, 30, 15000)};
            });
        }

        for (const mode of ["L1T2", "L1T3"]) {
            for (const [codecName, mimeType, fmtp] of [["VP8", "video/VP8"], ["VP9", "video/VP9", "profile-id=0"], ["AV1", "video/AV1"]]) {
                await run(`scalability${codecName}${mode}`, async () => {
                    const codecs = codecsFor(mimeType, fmtp);
                    if (!codecs) {
                        return {unsupported: true};
                    }
                    const source = makeSource(640, 360);
                    const sender = makePeer();
                    const receiver = makePeer();
                    const transceiver = sender.addTransceiver(source.track, {direction: "sendonly", sendEncodings: [{scalabilityMode: mode}]});
                    transceiver.setCodecPreferences(codecs);
                    const connection = await connect(sender, receiver);
                    const decoded = await waitForDecodedFrames(receiver, 30, 15000);
                    return {connection, parameters: transceiver.sender.getParameters().encodings.map(encoding => encoding.scalabilityMode || null), outbound: await outbound(sender), inbound: decoded};
                });
            }
        }

        const transformWorker = new Worker(URL.createObjectURL(new Blob([`
            onrtctransform = event => {
                const transformer = event.transformer;
                let frames = 0;
                transformer.readable.pipeThrough(new TransformStream({
                    transform(frame, controller) {
                        frames++;
                        controller.enqueue(frame);
                    }
                })).pipeTo(transformer.writable);
                setInterval(() => postMessage({side: transformer.options.side, frames}), 250);
            };
        `], {type: "text/javascript"})));
        const transformCounts = {};
        transformWorker.onmessage = event => {
            transformCounts[event.data.side] = event.data.frames;
        };

        for (const [name, mimeType, fmtp] of [["transformH264", "video/H264", "42e01f"], ["transformVP8", "video/VP8"]]) {
            await run(name, async () => {
                const codecs = codecsFor(mimeType, fmtp);
                if (!codecs) {
                    return {unsupported: true};
                }
                for (const key of Object.keys(transformCounts)) {
                    delete transformCounts[key];
                }
                const source = makeSource(640, 360);
                const sender = makePeer();
                const receiver = makePeer();
                const transceiver = sender.addTransceiver(source.track, {direction: "sendonly"});
                transceiver.setCodecPreferences(codecs);
                transceiver.sender.transform = new RTCRtpScriptTransform(transformWorker, {side: "sender"});
                receiver.ontrack = event => {
                    event.receiver.transform = new RTCRtpScriptTransform(transformWorker, {side: "receiver"});
                };
                const connection = await connect(sender, receiver);
                const decoded = await waitForDecodedFrames(receiver, 60, 15000);
                await delay(500);
                return {connection, inbound: decoded, transformed: {...transformCounts}};
            });
        }
        transformWorker.terminate();

        await run("concurrentSenders", async () => {
            const colors = {red: [220, 30, 30], blue: [30, 30, 220]};
            const red = makeSource(640, 360, "rgb(220, 30, 30)");
            const blue = makeSource(640, 360, "rgb(30, 30, 220)");
            const sender = makePeer();
            const receiver = makePeer();
            const redStream = new MediaStream([red.track]);
            const blueStream = new MediaStream([blue.track]);
            sender.addTrack(red.track, redStream);
            sender.addTrack(blue.track, blueStream);
            const received = new Map();
            receiver.ontrack = event => {
                const video = document.createElement("video");
                video.muted = true;
                video.playsInline = true;
                video.srcObject = new MediaStream([event.track]);
                videos.push(video);
                video.play().catch(() => {});
                received.set(event.streams[0].id, video);
            };
            const connection = await connect(sender, receiver);
            const samples = {red: [], blue: []};
            const canvas = document.createElement("canvas");
            canvas.width = 64;
            canvas.height = 36;
            const context = canvas.getContext("2d", {willReadFrequently: true});
            const deadline = performance.now() + 20000;
            while (performance.now() < deadline && (samples.red.length < 20 || samples.blue.length < 20)) {
                for (const [name, stream] of [["red", redStream], ["blue", blueStream]]) {
                    const video = received.get(stream.id);
                    if (!video || video.readyState < HTMLMediaElement.HAVE_CURRENT_DATA || !video.videoWidth) {
                        continue;
                    }
                    context.drawImage(video, 0, 0, canvas.width, canvas.height);
                    const pixel = context.getImageData(canvas.width / 2, canvas.height - 4, 1, 1).data;
                    samples[name].push([pixel[0], pixel[1], pixel[2]]);
                }
                await delay(100);
            }
            const distance = (left, right) => Math.sqrt(left.reduce((sum, value, index) => sum + (value - right[index]) ** 2, 0));
            const summary = {};
            for (const name of ["red", "blue"]) {
                const other = name === "red" ? "blue" : "red";
                summary[name] = {
                    samples: samples[name].length,
                    matching: samples[name].filter(sample => distance(sample, colors[name]) < distance(sample, colors[other])).length
                };
            }
            return {connection, summary};
        });
    } catch (error) {
        measurements.failures.push({name: stage, error: String(error)});
    } finally {
        painting = false;
        for (const peer of peers) {
            peer.close();
        }
        for (const source of sources) {
            source.track.stop();
        }
        for (const video of videos) {
            video.pause();
            video.srcObject = null;
        }
        document.body.replaceChildren();
    }
    return {probe: "webrtc-video", measurements, visibility: document.visibilityState};
}

async function canvasThreadPixels(expectedThreaded) {
    let checks = 0;
    const assert = (value, label) => {
        if (!value) {
            throw new Error(label);
        }
        ++checks;
    };
    const create = (control, width = 192, height = 128) => {
        const canvas = typeof document === "undefined" ? new OffscreenCanvas(width, height) : document.createElement("canvas");
        canvas.width = width;
        canvas.height = height;
        const context = canvas.getContext("2d", {renderingModeForTesting: "Accelerated", disableCanvasThreadForTesting: control});
        assert(context.isCanvasThreadForTesting() === (!control && expectedThreaded), "backend selection");
        return {canvas, context};
    };
    const equal = (first, second, label) => {
        assert(first.length === second.length, label + " length");
        for (let index = 0; index < first.length; ++index) {
            if (first[index] !== second[index]) {
                throw new Error(label + " byte " + index + ": " + first[index] + " versus " + second[index]);
            }
        }
        ++checks;
    };
    const pixels = item => item.context.getImageData(0, 0, item.canvas.width, item.canvas.height).data;
    const candidate = create(false);
    const control = create(true);
    if (typeof document !== "undefined") {
        document.body.replaceChildren(candidate.canvas, control.canvas);
    }
    const compare = label => equal(pixels(candidate), pixels(control), label);
    const both = draw => {
        draw(candidate.context, candidate.canvas);
        draw(control.context, control.canvas);
    };
    both(context => {
        context.fillStyle = "rgba(25,120,220,0.5)";
        context.fillRect(0, 0, 192, 128);
        context.save();
        context.translate(17.25, 11.5);
        context.rotate(0.12);
        context.beginPath();
        context.rect(0, 0, 140, 80);
        context.clip();
        for (let i = 0; i < 600; ++i) {
            context.fillStyle = `rgba(${i % 255},80,190,0.4)`;
            context.beginPath();
            context.arc((i * 37) % 180, (i * 19) % 120, 4.5, 0, 2 * Math.PI);
            context.fill();
        }
        context.restore();
        context.lineWidth = 3.25;
        context.strokeStyle = "#20d04f";
        context.strokeRect(2.5, 3.5, 130, 97);
    });
    compare("paths, clips and transform");
    both((context, canvas) => {
        const gradient = context.createLinearGradient(0, 0, 192, 128);
        gradient.addColorStop(0, "red");
        gradient.addColorStop(1, "transparent");
        context.fillStyle = gradient;
        context.globalCompositeOperation = "multiply";
        context.fillRect(0, 0, 160, 110);
        context.globalCompositeOperation = "source-over";
        context.drawImage(canvas, 0, 0, 80, 60, 100, 60, 80, 60);
    });
    compare("gradient, blend and self copy");
    both(context => {
        const data = context.createImageData(17, 13);
        for (let index = 0; index < data.data.length; ++index) {
            data.data[index] = (index * 17) % 256;
        }
        context.putImageData(data, -3, 4, 2, 1, 12, 10);
    });
    compare("putImageData clipping and alpha");
    equal(candidate.context.getImageData(-9, -11, 31, 29).data,
        control.context.getImageData(-9, -11, 31, 29).data, "readback outside bounds");
    const snapshot = await createImageBitmap(candidate.canvas).catch(error => {
        throw new Error("canvas snapshot: " + error);
    });
    const expectedSnapshot = new Uint8ClampedArray(pixels(control));
    both(context => {
        context.fillStyle = "#aa33dd";
        context.fillRect(0, 0, 192, 128);
    });
    const snapshotTarget = create(false);
    snapshotTarget.context.drawImage(snapshot, 0, 0);
    equal(pixels(snapshotTarget), expectedSnapshot, "snapshot survives source mutation");
    snapshot.close();
    const tile = create(false, 8, 8);
    tile.context.fillStyle = "#31be68";
    tile.context.fillRect(0, 0, 8, 8);
    both(context => {
        context.fillStyle = context.createPattern(tile.canvas, "repeat");
        context.fillRect(0, 0, 180, 100);
        context.drawImage(tile.canvas, 2, 3, 40, 40);
    });
    compare("cross-canvas image and pattern");
    const softwareCanvas = typeof document === "undefined" ? new OffscreenCanvas(192, 128) : document.createElement("canvas");
    softwareCanvas.width = 192;
    softwareCanvas.height = 128;
    const softwareContext = softwareCanvas.getContext("2d", {renderingModeForTesting: "Unaccelerated", willReadFrequently: true});
    assert(!softwareContext.isCanvasThreadForTesting(), "software destination backend");
    softwareContext.drawImage(candidate.canvas, 0, 0);
    equal(softwareContext.getImageData(0, 0, 192, 128).data, pixels(candidate), "software destination drawImage");
    softwareContext.fillStyle = softwareContext.createPattern(tile.canvas, "repeat");
    softwareContext.fillRect(0, 0, 192, 128);
    equal(softwareContext.getImageData(17, 19, 1, 1).data, new Uint8Array([49, 190, 104, 255]), "software destination pattern");
    softwareContext.clearRect(0, 0, 192, 128);
    softwareContext.strokeStyle = softwareContext.createPattern(tile.canvas, "repeat");
    softwareContext.lineWidth = 8;
    softwareContext.strokeRect(8, 8, 176, 112);
    equal(softwareContext.getImageData(8, 19, 1, 1).data, new Uint8Array([49, 190, 104, 255]), "software destination stroke pattern");
    const encode = async canvas => {
        if (typeof canvas.convertToBlob === "function") {
            return canvas.convertToBlob();
        }
        const dataImage = new Image();
        const dataURL = canvas.toDataURL();
        dataImage.src = dataURL;
        await dataImage.decode().catch(error => {
            throw new Error("data URL decode: " + error + " length=" + dataURL.length + " prefix=" + dataURL.slice(0, 48));
        });
        const decoded = create(false);
        decoded.context.drawImage(dataImage, 0, 0);
        equal(pixels(decoded), pixels(candidate), "toDataURL pixels");
        return new Promise(resolve => canvas.toBlob(resolve));
    };
    const blob = await encode(candidate.canvas).catch(error => {
        throw new Error("canvas encode: " + error);
    });
    assert(blob && blob.size > 0, "PNG encoding");
    const encodedSnapshot = await createImageBitmap(blob).catch(error => {
        throw new Error("blob bitmap decode: " + error);
    });
    const decoded = create(false);
    decoded.context.drawImage(encodedSnapshot, 0, 0);
    equal(pixels(decoded), pixels(candidate), "toBlob pixels");
    encodedSnapshot.close();
    const upload = canvas => {
        const glCanvas = new OffscreenCanvas(192, 128);
        const gl = glCanvas.getContext("webgl");
        assert(!!gl, "WebGL available");
        const texture = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, texture);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, canvas);
        assert(gl.getError() === gl.NO_ERROR, "canvas texture upload");
        const framebuffer = gl.createFramebuffer();
        gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, texture, 0);
        assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE, "texture framebuffer");
        const data = new Uint8Array(192 * 128 * 4);
        gl.readPixels(0, 0, 192, 128, gl.RGBA, gl.UNSIGNED_BYTE, data);
        assert(gl.getError() === gl.NO_ERROR, "texture readback");
        gl.deleteFramebuffer(framebuffer);
        gl.deleteTexture(texture);
        gl.getExtension("WEBGL_lose_context")?.loseContext();
        return data;
    };
    const webglProbe = new OffscreenCanvas(1, 1).getContext("webgl");
    if (typeof document !== "undefined") {
        assert(!!webglProbe, "document WebGL probe available");
    }
    let webglUploads = 0;
    if (webglProbe) {
        const uploaded = upload(candidate.canvas);
        equal(uploaded, upload(control.canvas), "WebGL upload equivalence");
        equal(uploaded, pixels(candidate), "WebGL upload source pixels");
        webglUploads = 2;
        webglProbe.getExtension("WEBGL_lose_context")?.loseContext();
    }
    const nextFrame = () => typeof document !== "undefined"
        ? new Promise(resolve => requestAnimationFrame(resolve)) : new Promise(resolve => setTimeout(resolve, 0));
    for (let frame = 0; frame < 12; ++frame) {
        if (typeof document !== "undefined" && frame === 6) {
            for (const item of [candidate, control]) {
                item.canvas.width = 61;
                item.canvas.height = 43;
            }
        }
        both(context => {
            context.fillStyle = `rgb(${frame * 19},40,180)`;
            context.fillRect(frame * 9, frame * 7, 70, 40);
            for (let index = 0; index < 96; ++index) {
                context.beginPath();
                context.arc((index * 37 + frame * 11) % 192, (index * 19 + frame * 7) % 128, 3.5, 0, 2 * Math.PI);
                context.fill();
            }
        });
        await nextFrame();
    }
    compare("multiple queued frames and resize during animation");
    for (const item of [candidate, control]) {
        item.canvas.width = 61;
        item.canvas.height = 43;
    }
    compare("resize clears pending drawing");
    both(context => {
        context.fillStyle = "#4180c2";
        context.fillRect(0, 0, 61, 43);
    });
    compare("draw after resize");
    return {checks, webglUploads};
}

async function canvasThreadRegression(expectedThreaded) {
    const {checks, webglUploads} = await canvasThreadPixels(expectedThreaded);
    if (expectedThreaded) {
        const presentationCanvas = document.createElement("canvas");
        presentationCanvas.width = 64;
        presentationCanvas.height = 64;
        document.body.append(presentationCanvas);
        const presentationContext = presentationCanvas.getContext("2d", {renderingModeForTesting: "Accelerated"});
        if (!presentationContext.isCanvasThreadForTesting()) {
            throw new Error("presentation probe backend");
        }
        const beforePresentation = presentationContext.synchronousCanvasOperationsForTesting();
        presentationContext.fillStyle = "#d128b4";
        presentationContext.fillRect(0, 0, 64, 64);
        await new Promise(resolve => requestAnimationFrame(() => setTimeout(resolve, 0)));
        if (presentationContext.synchronousCanvasOperationsForTesting() !== beforePresentation) {
            throw new Error("canvas presentation used a synchronous worker operation");
        }
        const pixel = presentationContext.getImageData(4, 4, 1, 1).data;
        if (pixel[0] !== 209 || pixel[1] !== 40 || pixel[2] !== 180 || pixel[3] !== 255) {
            throw new Error("canvas presentation readback lost pixels");
        }
        if (presentationContext.synchronousCanvasOperationsForTesting() <= beforePresentation) {
            throw new Error("canvas readback did not synchronize with the worker");
        }
        presentationCanvas.remove();
    }
    const create = (width, height) => {
        const canvas = document.createElement("canvas");
        canvas.width = width;
        canvas.height = height;
        return {canvas, context: canvas.getContext("2d", {renderingModeForTesting: "Accelerated"})};
    };
    const workerSource = `${canvasThreadPixels.toString()};onmessage=async event=>{try{
        const result=await canvasThreadPixels(event.data);
        if(event.data){
            const canvas=new OffscreenCanvas(19,13);
            const context=canvas.getContext('2d',{renderingModeForTesting:'Accelerated'});
            if(context.isCanvasThreadForTesting()!==event.data){throw new Error('handoff backend selection')}
            context.fillStyle='#167ac4';
            context.fillRect(0,0,19,13);
            result.bitmap=await createImageBitmap(canvas).catch(error=>{throw new Error('worker transfer bitmap: '+error)});
            postMessage(result,[result.bitmap]);
        }else{
            postMessage(result);
        }
    }catch(error){postMessage({error:String(error)})}}`;
    const workerUrl = URL.createObjectURL(new Blob([workerSource], {type: "text/javascript"}));
    const worker = new Worker(workerUrl);
    let workerChecks;
    let workerWebglUploads;
    let workerBitmap;
    try {
        workerChecks = await new Promise((resolve, reject) => {
            worker.onmessage = event => {
                if (event.data.error) {
                    reject(new Error(event.data.error));
                    return;
                }
                workerBitmap = event.data.bitmap;
                workerWebglUploads = event.data.webglUploads;
                resolve(event.data.checks);
            };
            worker.onerror = event => reject(new Error(event.message));
            worker.postMessage(expectedThreaded);
        });
    } finally {
        worker.terminate();
        URL.revokeObjectURL(workerUrl);
    }
    if (expectedThreaded) {
        const bitmapTarget = create(19, 13);
        bitmapTarget.context.drawImage(workerBitmap, 0, 0);
        const workerPixel = Array.from(bitmapTarget.context.getImageData(1, 1, 1, 1).data).join(",");
        workerBitmap.close();
        if (workerPixel !== "22,122,196,255") {
            throw new Error("worker image survives worker shutdown: " + workerPixel);
        }
    }
    document.body.replaceChildren();
    document.body.style.backgroundColor = "rgb(32,208,64)";
    const placeholder = document.createElement("canvas");
    placeholder.width = 512;
    placeholder.height = 512;
    placeholder.style.cssText = "position:fixed;left:0;top:0;z-index:10000";
    document.body.append(placeholder);
    const offscreen = placeholder.transferControlToOffscreen();
    const pendingUrl = URL.createObjectURL(new Blob([`onmessage=async event=>{
        try{
            const {canvas,expectedThreaded}=event.data;
            const context=canvas.getContext('2d',{renderingModeForTesting:'Accelerated',disableCanvasThreadForTesting:!expectedThreaded});
            if(context.isCanvasThreadForTesting()!==expectedThreaded){throw new Error('placeholder backend selection')}
            for(let frame=0;frame<12;++frame){
                context.fillStyle='rgb('+frame*19+',40,180)';
                for(let index=0;index<1024;++index){context.fillRect((index*37)%512,(index*19)%512,18,18)}
                await new Promise(resolve=>setTimeout(resolve,0));
            }
            postMessage('queued');
        }catch(error){postMessage({error:String(error)})}
    }`], {type: "text/javascript"}));
    const pendingWorker = new Worker(pendingUrl);
    try {
        await new Promise((resolve, reject) => {
            pendingWorker.onmessage = event => event.data.error ? reject(new Error(event.data.error)) : resolve();
            pendingWorker.onerror = event => reject(new Error(event.message));
            pendingWorker.postMessage({canvas: offscreen, expectedThreaded}, [offscreen]);
        });
        for (let frame = 0; frame < 3; ++frame) {
            await new Promise(resolve => requestAnimationFrame(resolve));
        }
        window.canvasThreadPresentationReady = true;
        await new Promise(resolve => {
            window.canvasThreadResume = () => {
                pendingWorker.terminate();
                URL.revokeObjectURL(pendingUrl);
                let frames = 0;
                const finishAfterWorkerCleanupFrames = () => {
                    ++frames;
                    if (frames === 3) {
                        resolve();
                    } else {
                        requestAnimationFrame(finishAfterWorkerCleanupFrames);
                    }
                };
                requestAnimationFrame(finishAfterWorkerCleanupFrames);
            };
        });
    } finally {
        pendingWorker.terminate();
        URL.revokeObjectURL(pendingUrl);
        window.canvasThreadResume = null;
    }
    if (!expectedThreaded) {
        return {checks, workerChecks, webglUploads, workerWebglUploads, lossSkipped: true};
    }
    const lossWorkerSource = `${canvasThreadLossSession.toString()};onmessage=()=>{
        try {
            self.session=canvasThreadLossSession(new OffscreenCanvas(32,32),new OffscreenCanvas(32,32));
            session.restored.then(result=>postMessage({result}),error=>postMessage({error:String(error)}));
            postMessage({ready:true});
        } catch(error) {postMessage({error:String(error)})}
    }`;
    const lossWorkerUrl = URL.createObjectURL(new Blob([lossWorkerSource], {type: "text/javascript"}));
    const lossWorker = new Worker(lossWorkerUrl);
    let resolveWorker;
    let rejectWorker;
    const workerRestored = new Promise((resolve, reject) => { resolveWorker = resolve; rejectWorker = reject; });
    let workerTimer;
    try {
        await new Promise((resolve, reject) => {
            workerTimer = setTimeout(() => reject(new Error("worker loss setup timed out")), 5000);
            lossWorker.onmessage = event => {
                if (event.data.ready) {
                    clearTimeout(workerTimer);
                    resolve();
                } else if (event.data.error) {
                    reject(new Error(event.data.error));
                    rejectWorker(new Error(event.data.error));
                } else {
                    resolveWorker(event.data.result);
                }
            };
            lossWorker.onerror = event => {
                reject(new Error(event.message));
                rejectWorker(new Error(event.message));
            };
            lossWorker.postMessage(true);
        });
        const session = canvasThreadLossSession(document.createElement("canvas"), document.createElement("canvas"));
        const completion = Promise.all([session.restored, workerRestored]);
        if (!session.context.loseContextForTesting()) {
            throw new Error("context loss injection did not reach the GPU backend");
        }
        const [result, workerLoss] = await completion;
        return {checks, workerChecks, webglUploads, workerWebglUploads, ...result, workerLoss, lossSkipped: false};
    } finally {
        clearTimeout(workerTimer);
        lossWorker.terminate();
        URL.revokeObjectURL(lossWorkerUrl);
    }
}

function canvasThreadLossSession(canvas, canceledCanvas) {
    canvas.width = canceledCanvas.width = 32;
    canvas.height = canceledCanvas.height = 32;
    const context = canvas.getContext("2d", {renderingModeForTesting: "Accelerated"});
    const canceledContext = canceledCanvas.getContext("2d", {renderingModeForTesting: "Accelerated"});
    context.fillRect(0, 0, 32, 32);
    canceledContext.fillRect(0, 0, 32, 32);
    if (!context.isCanvasThreadForTesting() || !canceledContext.isCanvasThreadForTesting()) {
        throw new Error("loss fixture requires the shared GPU backend");
    }
    let lost = 0;
    let canceled = 0;
    let restoredCanceled = false;
    let lostWhileActive = false;
    canvas.oncontextlost = () => {
        ++lost;
        lostWhileActive ||= !context.isContextLost();
    };
    canceledCanvas.oncontextlost = event => {
        event.preventDefault();
        ++canceled;
    };
    canceledCanvas.oncontextrestored = () => { restoredCanceled = true; };
    const restored = new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("restoration timed out")), 5000);
        canvas.oncontextrestored = () => {
            clearTimeout(timeout);
            setTimeout(() => {
                try {
                    if (context.isContextLost() || context.isCanvasThreadForTesting() || lost !== 1 || lostWhileActive) {
                        throw new Error("restoration state and fallback");
                    }
                    const blank = context.getImageData(0, 0, 32, 32).data;
                    if (blank.some(value => value !== 0)) {
                        throw new Error("restored canvas is not blank");
                    }
                    context.fillStyle = "#167ac4";
                    context.fillRect(0, 0, 32, 32);
                    const pixel = Array.from(context.getImageData(1, 1, 1, 1).data).join(",");
                    if (pixel !== "22,122,196,255") {
                        throw new Error("draw after restoration: " + pixel);
                    }
                    if (canceled !== 1 || restoredCanceled || !canceledContext.isContextLost()) {
                        throw new Error("canceled restoration");
                    }
                    resolve({lost, canceled});
                } catch (error) {
                    reject(error);
                }
            }, 0);
        };
    });
    return {context, restored};
}

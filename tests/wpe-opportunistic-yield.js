async function edenDOMProbe() {
    const startedAt = performance.now();
    const maxRuntimeMs = 110000;
    const busyDurationMs = 24000;
    const asyncErrors = [];
    const assertions = [];
    const timeouts = new Set();
    const intervals = new Set();
    const idleRequests = [];
    const frames = new Set();
    const frameRecords = new Map();
    const bridgeName = '__edenOpportunisticYieldBridge_' + Math.random().toString(36).slice(2);
    const previousBridgeDescriptor = Object.getOwnPropertyDescriptor(window, bridgeName);
    let cleaning = false;
    let selfTimeoutFired = false;
    let busyTimer = 0;
    let busyTimerTicks = 0;
    let busyStartedAt = 0;
    let nextChurnAt = 0;
    let churnBatches = 0;
    let churnNodesCreated = 0;
    let maxLiveChurnNodes = 0;
    let allocatedObjects = 0;
    const allocationRing = new Array(8);
    const objectsPerBatch = 4096;
    let timeoutCallbackCount = 0;
    let timeoutCallbackTimedOut = false;
    let timeoutCallbackElapsedMs = null;
    let cancelledPendingCallbackCount = 0;
    let cancelledRunnableCallbackCount = 0;
    let rootCallbackCount = 0;
    let reentrantRequests = 0;
    let reentrantCallbackCount = 0;
    let postChurnCallbackCount = 0;
    let postChurnCallbackTimedOut = false;
    let postChurnCallbackElapsedMs = null;
    const progressSamples = [];
    const mount = document.createElement('main');
    const churnHost = document.createElement('div');
    const churnNodesPerBatch = 8;
    const churnLiveNodeLimit = 24;
    const churnAllocationLimit = 4096;

    function elapsedMs() {
        return performance.now() - startedAt;
    }

    function recordAsyncError(name, error) {
        asyncErrors.push({
            name,
            message: String(error && (error.stack || error.message) || error),
            elapsedMs: elapsedMs()
        });
    }

    function guarded(name, callback) {
        return function (...args) {
            try {
                return callback.apply(this, args);
            } catch (error) {
                recordAsyncError(name, error);
                return undefined;
            }
        };
    }

    function assert(name, condition, observed) {
        const passed = Boolean(condition);
        const record = {name, passed, observed};
        assertions.push(record);
        if (!passed) {
            throw new Error('Assertion failed: ' + name + ' observed=' + JSON.stringify(observed));
        }
    }

    function checkRuntime() {
        if (selfTimeoutFired || elapsedMs() > maxRuntimeMs) {
            throw new Error('Fixture self-timeout after ' + maxRuntimeMs + ' ms');
        }
        if (asyncErrors.length) {
            throw new Error('Asynchronous callback failed: ' + JSON.stringify(asyncErrors));
        }
    }

    function scheduleTimeout(callback, delayMs) {
        let timer = 0;
        timer = setTimeout(guarded('fixture-timeout', function () {
            timeouts.delete(timer);
            if (!cleaning) {
                callback();
            }
        }), delayMs);
        timeouts.add(timer);
        return timer;
    }

    function pause(delayMs) {
        return new Promise(resolve => scheduleTimeout(resolve, delayMs));
    }

    async function waitFor(condition, limitMs, name) {
        const waitStartedAt = performance.now();
        while (!condition()) {
            checkRuntime();
            if (performance.now() - waitStartedAt > limitMs) {
                throw new Error('Timed out waiting for ' + name + ' after ' + limitMs + ' ms');
            }
            await pause(20);
        }
        checkRuntime();
    }

    function requestMainIdle(callback, options) {
        const request = {handle: 0, active: true};
        request.handle = window.requestIdleCallback(guarded('main-idle-callback', function (deadline) {
            request.active = false;
            callback(deadline);
        }), options);
        idleRequests.push(request);
        return request;
    }

    function cancelMainIdle(request) {
        if (request && request.active) {
            window.cancelIdleCallback(request.handle);
            request.active = false;
        }
    }

    function churnDOM() {
        const allocation = new Array(objectsPerBatch);
        for (let index = 0; index < objectsPerBatch; ++index) {
            allocation[index] = {index, batch: churnBatches, value: 'allocation-' + churnBatches + '-' + index};
        }
        allocationRing[churnBatches % allocationRing.length] = allocation;
        allocatedObjects += objectsPerBatch;
        for (let index = 0; index < churnNodesPerBatch; ++index) {
            if (churnHost.firstChild) {
                churnHost.removeChild(churnHost.firstChild);
            }
        }
        for (let index = 0; index < churnNodesPerBatch; ++index) {
            const node = document.createElement('span');
            node.textContent = 'churn-' + churnBatches + '-' + index;
            churnHost.appendChild(node);
            churnNodesCreated += 1;
        }
        churnBatches += 1;
        const liveNodes = churnHost.childElementCount;
        maxLiveChurnNodes = Math.max(maxLiveChurnNodes, liveNodes);
        if (liveNodes > churnLiveNodeLimit || churnNodesCreated > churnAllocationLimit) {
            throw new Error('DOM churn exceeded its allocation bound');
        }
    }

    function frameSource(token, replacement) {
        const bridge = 'parent[' + JSON.stringify(bridgeName) + ']';
        const tokenSource = JSON.stringify(token);
        if (replacement) {
            return '<!doctype html><script>const report = ' + bridge + '.bind(parent);' +
                'report(' + tokenSource + ', "replacement-ready")<\/script>';
        }
        return '<!doctype html><script>' +
            'const report = ' + bridge + '.bind(parent);' +
            'window.addEventListener("error", function (event) {' +
            'report(' + tokenSource + ', "frame-error", String(event.error && (event.error.stack || event.error.message) || event.message));' +
            '}, true);' +
            'window.addEventListener("pagehide", function () {report(' + tokenSource + ', "pagehide");}, {once:true});' +
            'window.requestIdleCallback(function () {' +
            'try {report(' + tokenSource + ', "idle");} catch (error) {' +
            'report(' + tokenSource + ', "callback-error", String(error && (error.stack || error.message) || error));' +
            '}}, {timeout:5000});' +
            'report(' + tokenSource + ', "scheduled")' +
            '<\/script>';
    }

    function bridgeFrame(token, eventName, detail) {
        try {
            const record = frameRecords.get(token);
            if (!record) {
                return;
            }
            if (eventName === 'scheduled') {
                record.scheduledAtMs = elapsedMs();
                if (record.action === 'navigate') {
                    record.navigationStartedAtMs = elapsedMs();
                    record.frame.srcdoc = frameSource(token, true);
                } else {
                    record.removalStartedAtMs = elapsedMs();
                    record.frame.remove();
                }
            } else if (eventName === 'pagehide') {
                record.pagehideAtMs = elapsedMs();
            } else if (eventName === 'idle') {
                record.idleCallbackCount += 1;
                record.idleAtMs = elapsedMs();
                if (record.pagehideAtMs !== null) {
                    record.idleAfterPagehideCount += 1;
                }
            } else if (eventName === 'replacement-ready') {
                record.replacementReadyAtMs = elapsedMs();
            } else if (eventName === 'frame-error' || eventName === 'callback-error') {
                const frameError = {eventName, message: String(detail), elapsedMs: elapsedMs()};
                record.errors.push(frameError);
                recordAsyncError('iframe-' + eventName, detail);
            }
        } catch (error) {
            recordAsyncError('iframe-' + eventName, error);
        }
    }

    function createPendingFrame(action) {
        const token = action + '-' + Math.random().toString(36).slice(2);
        const frame = document.createElement('iframe');
        frame.setAttribute('title', 'opportunistic yield ' + action + ' fixture');
        frame.setAttribute('aria-hidden', 'true');
        frame.style.position = 'absolute';
        frame.style.width = '1px';
        frame.style.height = '1px';
        frame.style.left = '-10000px';
        frame.style.top = '-10000px';
        const record = {
            action,
            frame,
            scheduledAtMs: null,
            navigationStartedAtMs: null,
            removalStartedAtMs: null,
            pagehideAtMs: null,
            replacementReadyAtMs: null,
            idleAtMs: null,
            idleCallbackCount: 0,
            idleAfterPagehideCount: 0,
            errors: []
        };
        frameRecords.set(token, record);
        frames.add(frame);
        mount.appendChild(frame);
        frame.srcdoc = frameSource(token, false);
        return record;
    }

    function onWindowError(event) {
        recordAsyncError('window-error', event.error || event.message || 'unknown error');
    }

    function onUnhandledRejection(event) {
        recordAsyncError('unhandled-rejection', event.reason);
    }

    try {
        if (typeof window.requestIdleCallback !== 'function' || typeof window.cancelIdleCallback !== 'function') {
            throw new Error('requestIdleCallback and cancelIdleCallback are required for this fixture');
        }
        Object.defineProperty(window, bridgeName, {
            configurable: true,
            value: bridgeFrame,
            writable: true
        });
        window.addEventListener('error', onWindowError, true);
        window.addEventListener('unhandledrejection', onUnhandledRejection, true);
        scheduleTimeout(function () {
            selfTimeoutFired = true;
        }, maxRuntimeMs);
        mount.style.position = 'absolute';
        mount.style.left = '0';
        mount.style.top = '0';
        mount.style.width = Math.max(1, innerWidth) + 'px';
        churnHost.style.display = 'block';
        churnHost.style.width = '100%';
        mount.appendChild(churnHost);
        document.body.appendChild(mount);
        for (let index = 0; index < churnLiveNodeLimit; ++index) {
            const node = document.createElement('span');
            node.textContent = 'seed-' + index;
            churnHost.appendChild(node);
            churnNodesCreated += 1;
        }
        maxLiveChurnNodes = churnHost.childElementCount;

        const cancelledPendingHandle = requestMainIdle(function () {
            cancelledPendingCallbackCount += 1;
        }, {timeout: 5000});
        cancelMainIdle(cancelledPendingHandle);

        let cancelledRunnableHandle = null;
        requestMainIdle(function () {
            rootCallbackCount += 1;
            cancelMainIdle(cancelledRunnableHandle);
            reentrantRequests += 1;
            requestMainIdle(function () {
                reentrantCallbackCount += 1;
                if (reentrantCallbackCount < 3) {
                    reentrantRequests += 1;
                    requestMainIdle(function () {
                        reentrantCallbackCount += 1;
                        if (reentrantCallbackCount < 3) {
                            reentrantRequests += 1;
                            requestMainIdle(function () {
                                reentrantCallbackCount += 1;
                            }, {timeout: 9000});
                        }
                    }, {timeout: 9000});
                }
            }, {timeout: 9000});
        }, {timeout: 9000});
        cancelledRunnableHandle = requestMainIdle(function () {
            cancelledRunnableCallbackCount += 1;
        }, {timeout: 9000});

        busyStartedAt = performance.now();
        nextChurnAt = busyStartedAt;
        busyTimer = setInterval(guarded('busy-timer', function () {
            busyTimerTicks += 1;
            const currentTime = performance.now();
            if (currentTime >= nextChurnAt) {
                churnDOM();
                nextChurnAt = currentTime + 100;
            }
        }), 0);
        intervals.add(busyTimer);

        const timeoutRequestedAt = performance.now();
        requestMainIdle(function (deadline) {
            timeoutCallbackCount += 1;
            timeoutCallbackTimedOut = Boolean(deadline.didTimeout);
            timeoutCallbackElapsedMs = performance.now() - timeoutRequestedAt;
        }, {timeout: 50});
        const forcedTimeoutTarget = timeoutRequestedAt + 150;
        while (performance.now() < forcedTimeoutTarget) {
        }

        const navigationFrame = createPendingFrame('navigate');
        const destructionFrame = createPendingFrame('destroy');
        await waitFor(function () {
            return navigationFrame.replacementReadyAtMs !== null && destructionFrame.removalStartedAtMs !== null;
        }, 10000, 'iframe navigation and destruction');
        await waitFor(function () {
            return timeoutCallbackCount > 0;
        }, 10000, 'timeout-backed idle callback');

        let priorTicks = 0;
        let nextSampleAt = 5000;
        while (performance.now() - busyStartedAt < busyDurationMs) {
            await pause(100);
            checkRuntime();
            const busyElapsed = performance.now() - busyStartedAt;
            if (busyElapsed >= nextSampleAt) {
                progressSamples.push({
                    elapsedMs: busyElapsed,
                    timerTicks: busyTimerTicks,
                    churnBatches,
                    liveChurnNodes: churnHost.childElementCount
                });
                assert('busy-timer-forward-progress-' + nextSampleAt, busyTimerTicks > priorTicks, {
                    priorTicks,
                    timerTicks: busyTimerTicks,
                    elapsedMs: busyElapsed
                });
                priorTicks = busyTimerTicks;
                nextSampleAt += 5000;
            }
        }
        clearInterval(busyTimer);
        intervals.delete(busyTimer);
        busyTimer = 0;

        if (nextSampleAt <= busyDurationMs) {
            progressSamples.push({
                elapsedMs: performance.now() - busyStartedAt,
                timerTicks: busyTimerTicks,
                churnBatches,
                liveChurnNodes: churnHost.childElementCount
            });
            assert('busy-timer-forward-progress-final', busyTimerTicks > priorTicks, {
                priorTicks,
                timerTicks: busyTimerTicks,
                elapsedMs: performance.now() - busyStartedAt
            });
        }

        assert('busy-timer-ran-throughout-churn', busyTimerTicks > 0 && churnBatches > 0, {
            busyTimerTicks,
            churnBatches,
            busyDurationMs: performance.now() - busyStartedAt
        });
        assert('dom-churn-live-node-bound', maxLiveChurnNodes <= churnLiveNodeLimit, {
            maxLiveChurnNodes,
            churnLiveNodeLimit
        });
        assert('dom-churn-allocation-bound', churnNodesCreated <= churnAllocationLimit, {
            churnNodesCreated,
            churnAllocationLimit
        });
        assert('timeout-backed-idle-progress', timeoutCallbackCount === 1 && timeoutCallbackTimedOut, {
            timeoutCallbackCount,
            timeoutCallbackTimedOut,
            timeoutCallbackElapsedMs
        });
        assert('pending-idle-cancellation', cancelledPendingCallbackCount === 0, {
            cancelledPendingCallbackCount
        });
        await waitFor(function () {
            return reentrantCallbackCount === 3;
        }, 20000, 'reentrant idle chain after churn stops');
        assert('runnable-idle-cancellation', rootCallbackCount === 1 && cancelledRunnableCallbackCount === 0, {
            rootCallbackCount,
            cancelledRunnableCallbackCount
        });
        assert('reentrant-idle-scheduling', reentrantRequests === 3 && reentrantCallbackCount === 3, {
            reentrantRequests,
            reentrantCallbackCount
        });
        assert('navigation-replaced-the-iframe-document', navigationFrame.scheduledAtMs !== null &&
            navigationFrame.navigationStartedAtMs !== null && navigationFrame.pagehideAtMs !== null &&
            navigationFrame.replacementReadyAtMs !== null, {
            scheduledAtMs: navigationFrame.scheduledAtMs,
            navigationStartedAtMs: navigationFrame.navigationStartedAtMs,
            pagehideAtMs: navigationFrame.pagehideAtMs,
            replacementReadyAtMs: navigationFrame.replacementReadyAtMs
        });
        assert('navigated-idle-callback-did-not-run-after-pagehide', navigationFrame.idleAfterPagehideCount === 0, {
            idleCallbackCount: navigationFrame.idleCallbackCount,
            pagehideAtMs: navigationFrame.pagehideAtMs,
            idleAtMs: navigationFrame.idleAtMs,
            idleAfterPagehideCount: navigationFrame.idleAfterPagehideCount,
            errors: navigationFrame.errors
        });
        assert('destroyed-iframe-idle-callback-stayed-pending', destructionFrame.scheduledAtMs !== null &&
            destructionFrame.removalStartedAtMs !== null && destructionFrame.idleCallbackCount === 0, {
            scheduledAtMs: destructionFrame.scheduledAtMs,
            removalStartedAtMs: destructionFrame.removalStartedAtMs,
            idleCallbackCount: destructionFrame.idleCallbackCount
        });

        const postChurnRequestedAt = performance.now();
        requestMainIdle(function (deadline) {
            postChurnCallbackCount += 1;
            postChurnCallbackTimedOut = Boolean(deadline.didTimeout);
            postChurnCallbackElapsedMs = performance.now() - postChurnRequestedAt;
        });
        await waitFor(function () {
            return postChurnCallbackCount === 1;
        }, 30000, 'idle callback after timers and DOM churn stop');

        assert('post-churn-idle-completion', postChurnCallbackCount === 1, {
            postChurnCallbackCount,
            postChurnCallbackTimedOut,
            postChurnCallbackElapsedMs,
            churnBatches
        });
        assert('asynchronous-errors-captured', asyncErrors.length === 0, asyncErrors);

        return {
            probe: 'opportunistic-yield',
            visibility: document.visibilityState,
            viewport: {
                width: innerWidth,
                height: innerHeight,
                devicePixelRatio,
                visual: window.visualViewport ? {
                    width: window.visualViewport.width,
                    height: window.visualViewport.height,
                    scale: window.visualViewport.scale,
                    offsetLeft: window.visualViewport.offsetLeft,
                    offsetTop: window.visualViewport.offsetTop
                } : null
            },
            measurements: {
                elapsedMs: elapsedMs(),
                maxRuntimeMs,
                busyDurationMs: performance.now() - busyStartedAt,
                busyTimerTicks,
                progressSamples,
                domChurn: {
                    batches: churnBatches,
                    nodesCreated: churnNodesCreated,
                    maxLiveNodes: maxLiveChurnNodes,
                    liveNodeLimit: churnLiveNodeLimit,
                    allocationLimit: churnAllocationLimit
                },
                heapChurn: {
                    objectsCreated: allocatedObjects,
                    maximumRetainedObjects: allocationRing.length * objectsPerBatch
                },
                idleCallbacks: {
                    timeoutCount: timeoutCallbackCount,
                    timeoutDidTimeout: timeoutCallbackTimedOut,
                    timeoutElapsedMs: timeoutCallbackElapsedMs,
                    cancelledPendingCount: cancelledPendingCallbackCount,
                    rootCount: rootCallbackCount,
                    cancelledRunnableCount: cancelledRunnableCallbackCount,
                    reentrantRequests,
                    reentrantCount: reentrantCallbackCount,
                    afterChurnCount: postChurnCallbackCount,
                    afterChurnDidTimeout: postChurnCallbackTimedOut,
                    afterChurnElapsedMs: postChurnCallbackElapsedMs
                },
                frames: {
                    navigation: {
                        scheduledAtMs: navigationFrame.scheduledAtMs,
                        navigationStartedAtMs: navigationFrame.navigationStartedAtMs,
                        pagehideAtMs: navigationFrame.pagehideAtMs,
                        replacementReadyAtMs: navigationFrame.replacementReadyAtMs,
                        idleCallbackCount: navigationFrame.idleCallbackCount,
                        idleAfterPagehideCount: navigationFrame.idleAfterPagehideCount,
                        errors: navigationFrame.errors
                    },
                    destruction: {
                        scheduledAtMs: destructionFrame.scheduledAtMs,
                        removalStartedAtMs: destructionFrame.removalStartedAtMs,
                        pagehideAtMs: destructionFrame.pagehideAtMs,
                        idleCallbackCount: destructionFrame.idleCallbackCount,
                        errors: destructionFrame.errors
                    }
                },
                assertions,
                asynchronousErrors: asyncErrors
            }
        };
    } finally {
        cleaning = true;
        for (const timer of timeouts) {
            clearTimeout(timer);
        }
        timeouts.clear();
        for (const interval of intervals) {
            clearInterval(interval);
        }
        intervals.clear();
        for (const request of idleRequests) {
            cancelMainIdle(request);
        }
        if (busyTimer) {
            clearInterval(busyTimer);
        }
        for (const frame of frames) {
            frame.remove();
        }
        frames.clear();
        allocationRing.fill(null);
        mount.remove();
        window.removeEventListener('error', onWindowError, true);
        window.removeEventListener('unhandledrejection', onUnhandledRejection, true);
        if (previousBridgeDescriptor) {
            Object.defineProperty(window, bridgeName, previousBridgeDescriptor);
        } else {
            delete window[bridgeName];
        }
    }
}

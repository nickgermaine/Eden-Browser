const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '../src/engine/cef/cefrendererapp.cpp'), 'utf8');

function displayEnvironment(sourceChoice = 'window') {
    const calls = [];
    const constraints = [];
    let stopped = 0;
    let failure;
    const track = {
        applyConstraints: async value => { constraints.push(value); if (failure) { throw failure; } },
        stop: () => { stopped++; }
    };
    const stream = {getVideoTracks: () => [track], getTracks: () => [track]};
    const mediaDevices = {
        getDisplayMedia: async value => { calls.push(['screen', value]); return stream; },
        getUserMedia: async value => { calls.push(['window', value]); return stream; }
    };
    let requests = 0;
    const finished = [];
    const context = vm.createContext({
        DOMException,
        navigator: {mediaDevices, userActivation: {isActive: true}},
        document: {hasFocus: () => true, permissionsPolicy: {allowsFeature: () => true}},
        window: {__edenPortalCapture: true, __edenRequestDisplayCapture: async () => { requests++; return {source: sourceChoice, id: requests}; },
            __edenFinishDisplayCapture: id => finished.push(id)}
    });
    vm.runInContext(hookScript('kDisplayCaptureHookScript'), context);
    return {mediaDevices, calls, constraints, context, stream, finished,
        get requests() { return requests; }, get stopped() { return stopped; },
        fail: error => { failure = error; }};
}

test('application capture separates native desktop constraints from Meet track constraints', async () => {
    const fixture = displayEnvironment();
    const requested = {width: {ideal: 1920}, height: {max: 1080}, frameRate: {ideal: 30, max: 30}};
    const stream = await fixture.mediaDevices.getDisplayMedia({video: requested, audio: true});
    assert.equal(stream, fixture.stream);
    assert.deepEqual(Object.keys(fixture.calls[0][1].video), ['mandatory']);
    assert.equal(fixture.calls[0][1].audio, false);
    assert.match(fixture.calls[0][1].video.mandatory.chromeMediaSourceId, /^window:[1-9][0-9]*:0$/);
    assert.deepEqual(JSON.parse(JSON.stringify(fixture.constraints)), [requested]);
});

test('application capture stops acquired tracks when constraints fail', async () => {
    const fixture = displayEnvironment();
    const failure = new DOMException('Too small', 'OverconstrainedError');
    fixture.fail(failure);
    await assert.rejects(fixture.mediaDevices.getDisplayMedia({video: {width: {max: 1}}}), error => error === failure);
    assert.equal(fixture.stopped, 1);
    assert.deepEqual(fixture.finished, [1]);
});

test('invalid capture constraints and inactive documents cannot open the chooser', async () => {
    for (const video of [false, {width: {exact: 100}}, {frameRate: {min: 10}}, {advanced: []}]) {
        const fixture = displayEnvironment();
        await assert.rejects(fixture.mediaDevices.getDisplayMedia({video}));
        assert.equal(fixture.requests, 0);
    }
    const fixture = displayEnvironment();
    fixture.context.navigator.userActivation.isActive = false;
    await assert.rejects(fixture.mediaDevices.getDisplayMedia({video: true}), {name: 'InvalidStateError'});
    assert.equal(fixture.requests, 0);
});

test('failed native capture expires its unused authorization', async () => {
    const fixture = displayEnvironment();
    fixture.mediaDevices.getUserMedia = () => { throw new DOMException('Rejected', 'NotAllowedError'); };
    await assert.rejects(fixture.mediaDevices.getDisplayMedia({video: true}), {name: 'NotAllowedError'});
    assert.deepEqual(fixture.finished, [1]);
});

test('screen capture uses consent without requiring a second transient activation', async () => {
    const fixture = displayEnvironment('screen');
    await fixture.mediaDevices.getDisplayMedia({video: {width: {ideal: 1920}}, audio: true});
    assert.match(fixture.calls[0][1].video.mandatory.chromeMediaSourceId, /^screen:[1-9][0-9]*:0$/);
    assert.equal(fixture.calls[0][1].audio, false);
    const cancelled = displayEnvironment('');
    await assert.rejects(cancelled.mediaDevices.getDisplayMedia({video: true}), {name: 'NotAllowedError'});
    assert.equal(cancelled.calls.length, 0);
});

class TestClipboardItem {
    constructor(text) { this.text = text; }
    async getType() { return new Blob([await this.text], {type: 'text/plain'}); }
}

function hookScript(name, backend = 'cef') {
    const sourceText = backend === 'qt'
        ? fs.readFileSync(path.join(__dirname, '../src/engine/qtwebengine/qtwebengineview.cpp'), 'utf8')
        : source;
    const prefix = backend === 'qt'
        ? 'static const QString formHookScript = QStringLiteral(R"JS('
        : 'constexpr const char *' + name + ' = R"JS(';
    const offset = sourceText.indexOf(prefix);
    assert.notEqual(offset, -1);
    const begin = offset + prefix.length;
    const end = sourceText.indexOf(')JS"', begin);
    assert.notEqual(end, -1);
    return sourceText.slice(begin, end);
}

function clipboardMirror() {
    return {
        reports: [], pending: null, serial: 0,
        report(...args) {
            if (!args.length) { return this.pending = 'operation:' + ++this.serial; }
            if (args.length === 1) { this.pending = null; this.reports.push(args[0]); return; }
            if (args[0] === this.pending) { this.pending = null; this.reports.push(args[1]); }
        }
    };
}

function clipboardEnvironment(clipboard, mirror = clipboardMirror()) {
    const reports = mirror.reports;
    const listeners = {};
    const context = vm.createContext({
        ClipboardItem: TestClipboardItem,
        Blob,
        navigator: {clipboard},
        document: {},
        window: {
            __edenReportCopy: (...args) => mirror.report(...args),
            addEventListener: (name, listener) => { listeners[name] = listener; },
            getSelection: () => ({toString: () => 'selection'})
        }
    });
    vm.runInContext(hookScript('kClipboardHookScript'), context);
    return {reports, listeners, context};
}

test('rejected clipboard writes never reach the shell', async () => {
    const denied = new Error('NotAllowedError');
    const clipboard = {writeText: () => Promise.reject(denied), write: () => Promise.reject(denied)};
    const {reports} = clipboardEnvironment(clipboard);
    await assert.rejects(clipboard.writeText('blocked text'), error => error === denied);
    const items = [new TestClipboardItem('blocked item')];
    await assert.rejects(clipboard.write(items), error => error === denied);
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(reports, []);
});

test('successful clipboard writes preserve native results and mirror text', async () => {
    let finish;
    let nativeValue;
    const clipboard = {
        writeText: value => { nativeValue = value; return new Promise(resolve => { finish = resolve; }); },
        write: async () => undefined
    };
    const {reports} = clipboardEnvironment(clipboard);
    let conversions = 0;
    const pending = clipboard.writeText({toString: () => { conversions++; return 'copied text'; }});
    assert.deepEqual(reports, []);
    finish(undefined);
    assert.equal(await pending, undefined);
    assert.equal(nativeValue, 'copied text');
    assert.equal(conversions, 1);
    const items = [new TestClipboardItem('copied item')];
    assert.equal(await clipboard.write(items), undefined);
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(reports, ['copied text', 'copied item']);
});

test('synthetic copy and cut cannot overwrite the clipboard', () => {
    const {reports, listeners} = clipboardEnvironment({});
    for (const name of ['copy', 'cut']) {
        listeners[name]({isTrusted: false, clipboardData: {getData: () => 'blocked'}});
        listeners[name]({isTrusted: true, clipboardData: {getData: () => name}});
    }
    assert.deepEqual(reports, ['copy', 'cut']);
});

test('page changes to Promise.then cannot approve rejected writes', async () => {
    const denied = new Error('NotAllowedError');
    const clipboard = {writeText: () => Promise.reject(denied)};
    const {reports, context} = clipboardEnvironment(clipboard);
    vm.runInContext('Promise.prototype.then = function(fulfilled) { fulfilled(); };', context);
    await assert.rejects(clipboard.writeText('blocked'), error => error === denied);
    assert.deepEqual(reports, []);
});

test('mutating an empty write cannot add text after permission checks', async () => {
    const clipboard = {write: async items => { assert.equal(items.length, 0); }};
    const {reports} = clipboardEnvironment(clipboard);
    const items = [];
    const pending = clipboard.write(items);
    items.push(new TestClipboardItem('blocked'));
    await pending;
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(reports, []);
});

test('native writes and mirroring consume the same item sequence', async () => {
    let iterations = 0;
    const clipboard = {write: async items => { assert.equal(Array.from(items).length, 0); }};
    const {reports} = clipboardEnvironment(clipboard);
    const items = {
        [Symbol.iterator]() {
            iterations++;
            return (iterations === 1 ? [] : [new TestClipboardItem('blocked')])[Symbol.iterator]();
        }
    };
    await clipboard.write(items);
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(iterations, 1);
    assert.deepEqual(reports, []);
});

test('item method overrides cannot change mirrored native contents', async () => {
    const clipboard = {write: async () => undefined};
    const {reports} = clipboardEnvironment(clipboard);
    const item = new TestClipboardItem('native text');
    item.getType = async () => new Blob(['forged text']);
    await clipboard.write([item]);
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(reports, ['native text']);
});

test('page callbacks cannot recover and replay the trusted event listener', () => {
    const {reports, listeners, context} = clipboardEnvironment({});
    vm.runInContext(`window.getSelection = function selected() {
        window.leakedListener = selected.caller;
        return {toString: function() { return 'user selection'; }};
    };`, context);
    listeners.copy({isTrusted: true});
    const leaked = context.window.leakedListener;
    if (leaked) {
        leaked({isTrusted: true, clipboardData: {getData: () => 'forged clipboard'}});
    }
    assert.equal(leaked, null);
    assert.deepEqual(reports, ['user selection']);
});

test('late extraction cannot overwrite a newer writeText', async () => {
    let finish;
    const clipboard = {write: async () => undefined, writeText: async () => undefined};
    const {reports} = clipboardEnvironment(clipboard);
    await clipboard.write([new TestClipboardItem(new Promise(resolve => { finish = resolve; }))]);
    await clipboard.writeText('new');
    finish('old');
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(reports, ['new']);
});

test('clipboard ordering is shared across independent frame contexts', async () => {
    let finishOld;
    let finishNew;
    const mirror = clipboardMirror();
    const first = {write: async () => undefined};
    const second = {write: async () => undefined};
    clipboardEnvironment(first, mirror);
    clipboardEnvironment(second, mirror);
    await first.write([new TestClipboardItem(new Promise(resolve => { finishOld = resolve; }))]);
    await second.write([new TestClipboardItem(new Promise(resolve => { finishNew = resolve; }))]);
    finishNew('second frame');
    await new Promise(resolve => setImmediate(resolve));
    finishOld('first frame');
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(mirror.reports, ['second frame']);
});

test('a newer trusted copy invalidates asynchronous extraction', async () => {
    let finish;
    const clipboard = {write: async () => undefined};
    const {reports, listeners} = clipboardEnvironment(clipboard);
    await clipboard.write([new TestClipboardItem(new Promise(resolve => { finish = resolve; }))]);
    listeners.copy({isTrusted: true, clipboardData: {getData: () => 'user copy'}});
    finish('old');
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(reports, ['user copy']);
});

test('denied writes and empty item lists preserve the current extraction', async () => {
    let finish;
    const clipboard = {write: async () => undefined, writeText: () => Promise.reject(new Error('denied'))};
    const {reports} = clipboardEnvironment(clipboard);
    await clipboard.write([new TestClipboardItem(new Promise(resolve => { finish = resolve; }))]);
    await assert.rejects(clipboard.writeText('denied'));
    await clipboard.write([]);
    finish('approved');
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(reports, ['approved']);
});

function formEnvironment(definitions, backend) {
    const reports = [];
    const credentials = [];
    const listeners = {};
    const windowListeners = {};
    const frames = new Map();
    let nextFrame = 0;
    let geometryReads = 0;
    const fields = definitions.map(definition => ({
        tagName: 'INPUT', type: 'text', name: '', id: '', placeholder: '', autocomplete: '',
        value: '', disabled: false, readOnly: false,
        getAttribute: () => null,
        getClientRects: () => { geometryReads++; return [{}]; },
        getBoundingClientRect: () => { geometryReads++; return {x: 20, y: 30, width: 100, height: 20}; },
        ...definition
    }));
    const document = {
        activeElement: fields[0],
        hasFocus: () => false,
        querySelectorAll: () => fields,
        addEventListener: (name, listener) => { listeners[name] = listener; }
    };
    const context = vm.createContext({
        document,
        origin: 'https://forms.example',
        qt: {webChannelTransport: {}},
        QWebChannel: function(transport, ready) {
            ready({objects:{forms:{
                registerDocument(origin, id, callback) {
                    callback({origin, documentId:id || '00000000-0000-4000-8000-000000000001', token:'fixture-token'});
                },
                report(token, id, origin, kind, values) {
                    if (kind === 'credential') { credentials.push(Array.from(values)); }
                    if (kind === 'field') { reports.push(Array.from(values)); }
                }
            }}});
        },
        console: {info() { throw new Error('Form reports must not use the console'); }},
        window: {
            __edenReportCredential: (...args) => credentials.push(args),
            __edenReportFormField: (...args) => reports.push(args),
            addEventListener: (name, listener) => { windowListeners[name] = listener; },
            requestAnimationFrame: callback => { frames.set(++nextFrame, callback); return nextFrame; },
            cancelAnimationFrame: id => frames.delete(id)
        }
    });
    context.window.top = context.window;
    vm.runInContext(hookScript('kFormHookScript', backend), context);
    return {
        fields, document, reports, credentials, listeners, windowListeners,
        geometryReads: () => geometryReads,
        scheduledFrames: () => frames.size,
        flush: () => {
            const callbacks = Array.from(frames.values());
            frames.clear();
            for (const callback of callbacks) { callback(); }
        }
    };
}

for (const backend of ['cef', 'qt']) {
    test(backend + ': synthetic inputs and clicks perform no form work', () => {
        const env = formEnvironment([{type: 'password', value: 'test-value'}], backend);
        const button = {
            tagName: 'BUTTON', type: 'submit',
            closest(selector) { return selector === 'form' ? null : this; }, getAttribute: () => null
        };
        for (let index = 0; index < 1000; index++) {
            env.listeners.input({isTrusted: false, target: env.fields[0]});
            env.listeners.click({isTrusted: false, target: button});
        }
        env.flush();
        assert.equal(env.geometryReads(), 0);
        assert.deepEqual(env.reports, []);
        assert.deepEqual(env.credentials, []);
        assert.equal(env.scheduledFrames(), 0);
    });

    test(backend + ': ordinary text editing skips geometry reads and bridge calls', () => {
        const env = formEnvironment([{name: 'todo', value: 'item'}], backend);
        for (let index = 0; index < 1000; index++) {
            env.listeners.input({isTrusted: true, target: env.fields[0]});
        }
        env.flush();
        assert.equal(env.geometryReads(), 0);
        assert.deepEqual(env.reports, []);
    });

    test(backend + ': relevant input bursts report their final value once per frame', () => {
        const env = formEnvironment([{type: 'email', name: 'email'}], backend);
        for (let index = 0; index < 1000; index++) {
            env.fields[0].value = String(index);
            env.listeners.input({isTrusted: true, target: env.fields[0]});
        }
        assert.equal(env.scheduledFrames(), 1);
        assert.equal(env.geometryReads(), 0);
        env.flush();
        assert.equal(env.geometryReads(), 1);
        assert.equal(env.reports.length, 1);
        assert.equal(env.reports[0][3], '999');
        env.listeners.input({isTrusted: true, target: env.fields[0]});
        env.flush();
        assert.equal(env.reports.length, 1);
    });

    test(backend + ': leaving an autofill field clears suggestions and cancels pending reports', () => {
        const env = formEnvironment([{type: 'email', value: 'person'}, {name: 'todo'}], backend);
        env.listeners.focusin({isTrusted: true, target: env.fields[0]});
        env.flush();
        env.listeners.input({isTrusted: true, target: env.fields[0]});
        env.document.activeElement = env.fields[1];
        env.listeners.focusin({isTrusted: true, target: env.fields[1]});
        env.flush();
        assert.equal(env.reports.length, 2);
        assert.deepEqual(env.reports[1], ['', '', '', '', 0, 0, 0, 0]);
        assert.equal(env.geometryReads(), 1);
        env.windowListeners.blur();
        assert.equal(env.reports.length, 2);
    });

    test(backend + ': password reports stay redacted and trusted submission still captures credentials', () => {
        const env = formEnvironment([
            {type: 'email', value: 'person@example.test'},
            {type: 'password', value: 'test-value'}
        ], backend);
        env.document.activeElement = env.fields[1];
        env.listeners.focusin({isTrusted: true, target: env.fields[1]});
        env.flush();
        assert.equal(env.reports[0][3], '');
        env.listeners.submit({isTrusted: true, target: env.document});
        assert.deepEqual(env.credentials, [['person@example.test', 'test-value']]);
    });
}

test('qt: password one-time-code fields never enter field reports', () => {
    const env = formEnvironment([{type: 'password', autocomplete: 'one-time-code', value: 'fixture-code'}], 'qt');
    env.listeners.focusin({isTrusted: true, target: env.fields[0]});
    env.flush();
    assert.equal(env.reports[0][3], '');
    env.listeners.submit({isTrusted: true, target: env.document});
    assert.deepEqual(env.credentials, []);
});


test('waiting in the source chooser does not lose approved capture', async () => {
    const fixture = displayEnvironment('screen');
    const pending = fixture.mediaDevices.getDisplayMedia({video: true});
    fixture.context.navigator.userActivation.isActive = false;
    assert.equal(await pending, fixture.stream);
    assert.match(fixture.calls[0][1].video.mandatory.chromeMediaSourceId, /^screen:/);
    assert.deepEqual(fixture.finished, [1]);
});

function mediaEnvironment() {
    const reports = [];
    class Track extends EventTarget {
        constructor(kind) { super(); this.type = kind; this.state = 'live'; }
        get kind() { return this.type; }
        get readyState() { return this.state; }
        getSettings() { return {}; }
        stop() { this.state = 'ended'; }
        clone() { return new Track(this.type); }
    }
    class Stream {
        constructor(tracks) { this.tracks = tracks; }
        getTracks() { return this.tracks; }
        clone() { return new Stream(this.tracks.map(track => new Track(track.kind))); }
    }
    const window = new EventTarget();
    const mediaDevices = {
        getUserMedia: async () => new Stream([new Track('video'), new Track('audio')]),
        getDisplayMedia: async () => new Stream([new Track('video'), new Track('audio')])
    };
    const scriptSource = fs.readFileSync(path.join(__dirname, '../src/engine/mediaactivityscript.h'), 'utf8');
    const script = scriptSource.split('R"JS(')[1].split(')JS"')[0];
    const context = vm.createContext({window, EventTarget, MediaStream: Stream, MediaStreamTrack: Track,
        navigator: {mediaDevices}, report: value => reports.push(value)});
    vm.runInContext(script + '(report)', context);
    return {reports, window, mediaDevices};
}

test('activity tracks camera, microphone, screen, shared audio, clones and stop', async () => {
    const fixture = mediaEnvironment();
    assert.deepEqual(fixture.reports, [0]);
    const call = await fixture.mediaDevices.getUserMedia({audio:true,video:true});
    assert.equal(fixture.reports.at(-1), 3);
    const clone = call.getTracks()[1].clone();
    call.getTracks().forEach(track => track.stop());
    assert.equal(fixture.reports.at(-1), 2);
    const share = await fixture.mediaDevices.getDisplayMedia({video:true,audio:true});
    assert.equal(fixture.reports.at(-1), 14);
    clone.stop();
    assert.equal(fixture.reports.at(-1), 12);
    share.getTracks().forEach(track => track.stop());
    assert.equal(fixture.reports.at(-1), 0);
});

test('activity clears on navigation and restores live tracks after history restore', async () => {
    const fixture = mediaEnvironment();
    const call = await fixture.mediaDevices.getUserMedia({video:true,audio:true});
    fixture.window.dispatchEvent(new Event('pagehide'));
    assert.equal(fixture.reports.at(-1), 0);
    fixture.window.dispatchEvent(new Event('pageshow'));
    assert.equal(fixture.reports.at(-1), 3);
    call.getTracks()[0].state = 'ended';
    call.getTracks()[0].dispatchEvent(new Event('ended'));
    assert.equal(fixture.reports.at(-1), 2);
    call.getTracks()[1].stop();
    assert.equal(fixture.reports.at(-1), 0);
});

test('activity persists while a cloned stream is still capturing', async () => {
    const fixture = mediaEnvironment();
    const original = await fixture.mediaDevices.getDisplayMedia({video:true,audio:true});
    const copy = original.clone();
    original.getTracks().forEach(track => track.stop());
    assert.equal(fixture.reports.at(-1), 12);
    copy.getTracks()[0].stop();
    assert.equal(fixture.reports.at(-1), 8);
    copy.getTracks()[1].stop();
    assert.equal(fixture.reports.at(-1), 0);
});

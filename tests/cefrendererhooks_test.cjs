const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '../src/engine/cef/cefrendererapp.cpp'), 'utf8');

class TestClipboardItem {
    constructor(text) { this.text = text; }
    async getType() { return new Blob([this.text], {type: 'text/plain'}); }
}

function hookScript(name) {
    const prefix = 'constexpr const char *' + name + ' = R"JS(';
    const offset = source.indexOf(prefix);
    assert.notEqual(offset, -1);
    const begin = offset + prefix.length;
    const end = source.indexOf(')JS";', begin);
    assert.notEqual(end, -1);
    return source.slice(begin, end);
}

function clipboardEnvironment(clipboard) {
    const reports = [];
    const listeners = {};
    const context = vm.createContext({
        ClipboardItem: TestClipboardItem,
        Blob,
        navigator: {clipboard},
        document: {},
        window: {
            __edenReportCopy: value => reports.push(value),
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

function formEnvironment(definitions) {
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
        querySelectorAll: () => fields,
        addEventListener: (name, listener) => { listeners[name] = listener; }
    };
    const context = vm.createContext({
        document,
        window: {
            __edenReportCredential: (...args) => credentials.push(args),
            __edenReportFormField: (...args) => reports.push(args),
            addEventListener: (name, listener) => { windowListeners[name] = listener; },
            requestAnimationFrame: callback => { frames.set(++nextFrame, callback); return nextFrame; },
            cancelAnimationFrame: id => frames.delete(id)
        }
    });
    vm.runInContext(hookScript('kFormHookScript'), context);
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

test('synthetic inputs and clicks perform no form work', () => {
    const env = formEnvironment([{type: 'password', value: 'test-value'}]);
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

test('ordinary text editing skips geometry reads and bridge calls', () => {
    const env = formEnvironment([{name: 'todo', value: 'item'}]);
    for (let index = 0; index < 1000; index++) {
        env.listeners.input({isTrusted: true, target: env.fields[0]});
    }
    env.flush();
    assert.equal(env.geometryReads(), 0);
    assert.deepEqual(env.reports, []);
});

test('relevant input bursts report their final value once per frame', () => {
    const env = formEnvironment([{type: 'email', name: 'email'}]);
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

test('leaving an autofill field clears suggestions and cancels pending reports', () => {
    const env = formEnvironment([{type: 'email', value: 'person'}, {name: 'todo'}]);
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

test('password reports stay redacted and trusted submission still captures credentials', () => {
    const env = formEnvironment([
        {type: 'email', value: 'person@example.test'},
        {type: 'password', value: 'test-value'}
    ]);
    env.document.activeElement = env.fields[1];
    env.listeners.focusin({isTrusted: true, target: env.fields[1]});
    env.flush();
    assert.equal(env.reports[0][3], '');
    env.listeners.submit({isTrusted: true, target: env.document});
    assert.deepEqual(env.credentials, [['person@example.test', 'test-value']]);
});

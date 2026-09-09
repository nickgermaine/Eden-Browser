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

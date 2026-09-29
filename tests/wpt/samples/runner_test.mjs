import assert from 'node:assert/strict';
import {chmod, mkdir, mkdtemp, readFile, rm, writeFile} from 'node:fs/promises';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import {test} from 'node:test';
import {fileURLToPath} from 'node:url';
import {attributable, capabilitiesFor, collect, discover, expect, main, parseOptions, rewritePage, runSuite, startStaticServer} from './run.mjs';
import {Driver, TimeoutError, WebDriverError} from './webdriver.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));

const FIXTURE_TEST = `
'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('../../../../../test/webdriver');

let driver;
const url = \`\${process.env.BASEURL}/src/content/fixture/basic/index.html\`;

describe('fixture sample', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('clicks and waits for page state', async () => {
    await driver.findElement(webdriver.By.id('startButton')).click();
    await driver.wait(() => driver.executeScript(() => window.readyCounter > 2));
    expect(await driver.findElement(webdriver.By.id('startButton')).isEnabled()).toBe(true);
  });

  it('types into a field and reads its value', async () => {
    await driver.findElement(webdriver.By.id('dataChannelSend')).sendKeys('hello', '\\n');
    const value = await driver.findElement(webdriver.By.id('dataChannelSend')).getAttribute('value');
    expect(value).toBe('hello');
    expect(value).not.toBe('');
  });

  it('opens and switches tabs', async () => {
    const first = await driver.getWindowHandle();
    await driver.switchTo().newWindow('tab');
    const second = await driver.getWindowHandle();
    expect(second).not.toBe(first);
    await driver.switchTo().window(first);
    expect(await driver.getWindowHandle()).toBe(first);
  });

  it('double clicks an element', async () => {
    const element = await driver.findElement(webdriver.By.css('#servers>option'));
    await driver.actions({async: true}).doubleClick(element).perform();
  });

  it('reports a missing element', async () => {
    await driver.findElement(webdriver.By.id('missing')).click();
  });

  it('reports a page error from a wait condition', async () => {
    await driver.wait(() => driver.executeScript(() => throwInPage));
  });

  it('times out', async () => {
    await new Promise(() => {});
  }, 1000);

  it.skip('is skipped', async () => {});
});
`;

async function fixture() {
    const root = await mkdtemp(path.join(os.tmpdir(), 'eden-samples-runner-'));
    const samples = path.join(root, 'samples');
    await mkdir(path.join(samples, 'src/content/fixture/basic/js'), {recursive: true});
    await writeFile(path.join(samples, 'src/content/fixture/basic/index.html'), '<!doctype html><title>fixture</title>');
    await writeFile(path.join(samples, 'src/content/fixture/basic/js/test.js'), FIXTURE_TEST);
    await mkdir(path.join(samples, 'src/content/fixture/untested'), {recursive: true});
    await writeFile(path.join(samples, 'src/content/fixture/untested/index.html'), '<!doctype html>');
    const tests = path.join(root, 'tests');
    await mkdir(path.join(tests, 'extra/fixture'), {recursive: true});
    await writeFile(path.join(tests, 'extra/fixture/untested.test.js'), `
describe('extra check', () => {
  it('runs', () => { expect(process.env.BASEURL.startsWith('http://127.0.0.1:')).toBe(true); });
  it('runs from the samples root', () => { expect(require('fs').existsSync(process.cwd() + '/src/content/fixture/basic/index.html')).toBe(true); });
});
`);
    await writeFile(path.join(tests, 'extra/fixture/broken.test.js'), 'describe("broken", () => { it("x", () => {}); });\nthis is not javascript');
    const log = path.join(root, 'webdriver.jsonl');
    const driver = path.join(root, 'fake-webdriver');
    await writeFile(driver, `#!/bin/sh\nexec "${process.execPath}" "${path.join(here, 'fixtures/fake-webdriver.mjs')}" "$@"\n`);
    await chmod(driver, 0o755);
    return {root, samples, tests, log, driver};
}

test('upstream sample tests run unmodified against a W3C WebDriver server', async () => {
    const {root, samples, tests, log, driver} = await fixture();
    try {
        process.env.FAKE_WEBDRIVER_LOG = log;
        const output = path.join(root, 'out/results.json');
        const before = process.cwd();
        await main(['--browser', 'eden', '--webdriver', driver, '--binary', '/opt/eden/eden-wpt-host', '--samples', samples, '--tests', tests,
            '--output', output, '--test-timeout', '5000']);
        const report = JSON.parse(await readFile(output, 'utf8'));
        const statuses = Object.fromEntries(report.results.map(result => [`${result.file} | ${result.name}`, result.status]));
        assert.deepEqual(statuses, {
            'fixture/basic | fixture sample > clicks and waits for page state': 'PASS',
            'fixture/basic | fixture sample > types into a field and reads its value': 'PASS',
            'fixture/basic | fixture sample > opens and switches tabs': 'PASS',
            'fixture/basic | fixture sample > double clicks an element': 'PASS',
            'fixture/basic | fixture sample > reports a missing element': 'FAIL',
            'fixture/basic | fixture sample > reports a page error from a wait condition': 'FAIL',
            'fixture/basic | fixture sample > times out': 'TIMEOUT',
            'fixture/basic | fixture sample > is skipped': 'SKIP',
            'fixture/untested | extra check > runs': 'PASS',
            'fixture/untested | extra check > runs from the samples root': 'PASS',
        });
        const messages = Object.fromEntries(report.results.map(result => [result.name, result.message]));
        assert.match(messages['fixture sample > reports a missing element'], /^no such element: Unable to locate \*\[id="missing"\]/);
        assert.match(messages['fixture sample > reports a page error from a wait condition'], /javascript error: ReferenceError: pc1 is not defined/);
        assert.match(messages['fixture sample > times out'], /Exceeded timeout of 1000 ms/);
        assert.deepEqual(report.files.map(file => [file.file, file.source, file.status]), [
            ['fixture/basic', 'upstream', 'OK'],
            ['fixture/broken', 'eden', 'ERROR'],
            ['fixture/untested', 'eden', 'OK'],
        ]);
        assert.equal(report.run_info.browser, 'eden');
        assert.equal(process.cwd(), before);

        const commands = (await readFile(log, 'utf8')).trim().split('\n').map(line => JSON.parse(line));
        const created = commands.find(command => command.method === 'POST' && command.path === '/session');
        assert.deepEqual(created.body.capabilities.alwaysMatch, {
            browserName: 'Eden',
            acceptInsecureCerts: true,
            'wpe:browserOptions': {binary: '/opt/eden/eden-wpt-host', args: ['--automation']},
        });
        const session = `/session/session-1`;
        const navigations = commands.filter(command => command.path === `${session}/url`);
        assert.equal(navigations.length, 7);
        assert.match(navigations[0].body.url, /^http:\/\/127\.0\.0\.1:\d+\/src\/content\/fixture\/basic\/index\.html$/);
        assert.ok(commands.some(command => command.path === `${session}/element/start/click` && command.method === 'POST'));
        assert.deepEqual(commands.find(command => command.path === `${session}/element/input/value`).body, {text: 'hello\n'});
        const polls = commands.filter(command => command.path === `${session}/execute/sync` && command.body.script.includes('readyCounter'));
        assert.equal(polls.length, 3);
        assert.equal(polls[0].body.script, 'return (() => window.readyCounter > 2).apply(null, arguments);');
        const attribute = commands.find(command => command.path === `${session}/execute/sync` && command.body.script.includes('willvalidate'));
        assert.deepEqual(attribute.body.args, [{'element-6066-11e4-a52e-4f735466cecf': 'input'}, 'value']);
        assert.deepEqual(commands.find(command => command.path === `${session}/window/new`).body, {type: 'tab'});
        const actions = commands.find(command => command.path === `${session}/actions`).body.actions;
        assert.equal(actions.length, 1);
        assert.deepEqual(actions[0].actions.map(action => action.type), ['pointerMove', 'pointerDown', 'pointerUp', 'pointerDown', 'pointerUp']);
        assert.deepEqual(actions[0].actions[0].origin, {'element-6066-11e4-a52e-4f735466cecf': 'option'});
        assert.ok(commands.some(command => command.method === 'DELETE' && command.path === session));
    } finally {
        delete process.env.FAKE_WEBDRIVER_LOG;
        await rm(root, {recursive: true, force: true});
    }
});

test('each browser gets its own session capabilities', () => {
    const eden = capabilitiesFor('eden', '/build/eden-wpt-host');
    assert.equal(eden.browserName, 'Eden');
    assert.deepEqual(eden['wpe:browserOptions'].args, ['--automation']);
    const chromium = capabilitiesFor('chromium', '/usr/bin/chromium');
    assert.equal(chromium.browserName, 'chrome');
    assert.equal(eden.unhandledPromptBehavior, undefined);
    assert.equal(chromium.unhandledPromptBehavior, undefined);
    assert.equal(chromium['goog:chromeOptions'].binary, '/usr/bin/chromium');
    for (const flag of ['--headless=new', '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', '--autoplay-policy=no-user-gesture-required']) {
        assert.ok(chromium['goog:chromeOptions'].args.includes(flag), flag);
    }
});

test('options are validated', () => {
    const base = ['--webdriver', 'w', '--binary', 'b', '--samples', 's', '--tests', 't', '--output', 'o.json'];
    assert.equal(parseOptions(['--browser', 'chromium', ...base]).testTimeout, 30000);
    assert.throws(() => parseOptions(['--browser', 'firefox', ...base]), /eden or chromium/);
    assert.throws(() => parseOptions(['--browser', 'eden', ...base, '--test-timeout', '10']), /between 1000 and 600000/);
    assert.throws(() => parseOptions(['--browser', 'eden', '--webdriver', 'w']), /--binary is required/);
});

test('expect matchers follow jest', () => {
    expect(3).toBe(3);
    expect({a: [1, 2]}).toEqual({a: [1, 2]});
    expect('1 ').not.toBe('9 ');
    expect(640).toBeGreaterThan(320);
    expect('Using VP8').toMatch(/^Using/);
    expect([1, 2]).toContain(2);
    expect(null).toBeNull();
    assert.throws(() => expect(1).toBe(2), /expect\(1\)\.toBe\(2\)/);
    assert.throws(() => expect('').not.toBe(''), /expect\(""\)\.not\.toBe\(""\)/);
    assert.throws(() => expect(1).toBeGreaterThan(1), /toBeGreaterThan/);
});

test('waits follow selenium semantics', async () => {
    const driver = new Driver('http://127.0.0.1:9', 'session', {});
    let calls = 0;
    assert.equal(await driver.wait(() => ++calls >= 3 && 'done', 0, undefined, 1), 'done');
    assert.equal(calls, 3);
    await assert.rejects(driver.wait(() => false, 50, 'never', 5), error => error instanceof TimeoutError && /never\nWait timed out after \d+ms/.test(error.message));
    await assert.rejects(driver.wait(() => {
        throw new WebDriverError('javascript error', 'boom');
    }), /boom/);
    assert.equal(await driver.wait(Promise.resolve(7), 100), 7);
    const closed = new Driver('http://127.0.0.1:9', 'closed', {});
    closed.closed = true;
    await assert.rejects(closed.get('about:blank'), /already quit/);
});

test('hooks run in jest order and a failing beforeAll fails its tests', async () => {
    const order = [];
    const suite = collect(`
describe('outer', () => {
  beforeAll(() => order.push('beforeAll'));
  beforeEach(() => order.push('outer beforeEach'));
  afterEach(() => order.push('outer afterEach'));
  afterAll(() => order.push('afterAll'));
  describe('inner', () => {
    beforeEach(() => order.push('inner beforeEach'));
    afterEach(() => order.push('inner afterEach'));
    it('one', () => order.push('one'));
  });
  it('two', done => { order.push('two'); done(); });
});
describe('broken setup', () => {
  beforeAll(() => { throw new Error('no browser'); });
  it('three', () => order.push('three'));
});
`, path.join(here, 'inline.test.js'), () => () => ({}), {order});
    const results = [];
    await runSuite(suite, 1000, result => results.push(result));
    assert.deepEqual(order, ['beforeAll', 'outer beforeEach', 'inner beforeEach', 'one', 'inner afterEach', 'outer afterEach',
        'outer beforeEach', 'two', 'outer afterEach', 'afterAll']);
    assert.deepEqual(results.map(result => [result.name, result.status]), [
        ['outer > inner > one', 'PASS'],
        ['outer > two', 'PASS'],
        ['broken setup > three', 'FAIL'],
    ]);
    assert.match(results[2].message, /beforeAll failed: no browser/);
});

test('the static server serves sample files and nothing outside them', async () => {
    const root = await mkdtemp(path.join(os.tmpdir(), 'eden-samples-server-'));
    await mkdir(path.join(root, 'site/src'), {recursive: true});
    await writeFile(path.join(root, 'site/src/index.html'), '<p>hi</p>');
    await writeFile(path.join(root, 'secret.txt'), 'secret');
    const {server, origin} = await startStaticServer(path.join(root, 'site'));
    try {
        const page = await fetch(`${origin}/src/`);
        assert.equal(page.status, 200);
        assert.equal(page.headers.get('content-type'), 'text/html; charset=utf-8');
        assert.equal(await page.text(), '<p>hi</p>');
        assert.equal((await fetch(`${origin}/src/missing.js`)).status, 404);
        const raw = target => new Promise((resolve, reject) => {
            http.get(`${origin}${target}`, response => {
                response.resume();
                response.on('end', () => resolve(response.statusCode));
            }).on('error', reject);
        });
        assert.equal(await raw('/..%2fsecret.txt'), 403);
        assert.equal(await raw('/src/..%2f..%2fsecret.txt'), 403);
        assert.equal(await raw('/src/index.html'), 200);
    } finally {
        server.close();
        await rm(root, {recursive: true, force: true});
    }
});

test('pages load pinned local copies instead of remote scripts and fonts', async () => {
    const page = '<link href="https://fonts.googleapis.com/css?family=Roboto:300,400" rel="stylesheet">'
        + '<link href="//fonts.googleapis.com/css?family=Inconsolata" rel="stylesheet">'
        + '<script src="https://webrtc.github.io/adapter/adapter-latest.js"></script>';
    assert.equal(rewritePage(page, '/opt/adapter.js'), '<link href="/__eden/empty.css" rel="stylesheet">'
        + '<link href="/__eden/empty.css" rel="stylesheet">'
        + '<script src="/__eden/adapter.js"></script>');
    assert.match(rewritePage(page, null), /https:\/\/webrtc\.github\.io\/adapter\/adapter-latest\.js/);
    const root = await mkdtemp(path.join(os.tmpdir(), 'eden-samples-stubs-'));
    await mkdir(path.join(root, 'site/src/js/lib'), {recursive: true});
    await writeFile(path.join(root, 'site/src/js/lib/ga.js'), "document.write('<script src=\"https://www.google-analytics.com/analytics.js\"></script>');");
    await writeFile(path.join(root, 'site/src/index.html'), page);
    await writeFile(path.join(root, 'adapter.js'), 'window.adapter = {pinned: true};');
    const {server, origin} = await startStaticServer(path.join(root, 'site'), {adapter: path.join(root, 'adapter.js')});
    try {
        const html = await (await fetch(`${origin}/src/index.html`)).text();
        assert.ok(!html.includes('googleapis') && !html.includes('webrtc.github.io'));
        assert.equal(await (await fetch(`${origin}/__eden/adapter.js`)).text(), 'window.adapter = {pinned: true};');
        const css = await fetch(`${origin}/__eden/empty.css`);
        assert.equal(css.headers.get('content-type'), 'text/css; charset=utf-8');
        assert.equal(await css.text(), '');
        const analytics = await (await fetch(`${origin}/src/js/lib/ga.js`)).text();
        assert.ok(!analytics.includes('google-analytics.com'));
        assert.match(analytics, /window\.ga = window\.ga \|\| function/);
    } finally {
        server.close();
        await rm(root, {recursive: true, force: true});
    }
});

test('late errors from an earlier file\'s sessions are not blamed on the current check', () => {
    const current = new Set([new Driver('http://127.0.0.1:9', 'current', {})]);
    assert.equal(attributable(new WebDriverError('invalid session id', 'gone', 404, 'earlier'), current), false);
    assert.equal(attributable(new WebDriverError('no such element', 'missing', 404, 'current'), current), true);
    assert.equal(attributable(new Error('page script threw'), current), true);
    assert.equal(attributable(new WebDriverError('unknown error', 'no session', 500, null), current), true);
});

test('discovery pairs tests with samples and honours filters', async () => {
    const {root, samples, tests} = await fixture();
    try {
        const all = await discover(samples, tests, []);
        assert.deepEqual(all.map(entry => [entry.sample, entry.source]), [
            ['fixture/basic', 'upstream'],
            ['fixture/broken', 'eden'],
            ['fixture/untested', 'eden'],
        ]);
        assert.deepEqual((await discover(samples, tests, ['untested'])).map(entry => entry.sample), ['fixture/untested']);
    } finally {
        await rm(root, {recursive: true, force: true});
    }
});

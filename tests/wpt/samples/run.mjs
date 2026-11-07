import {createReadStream} from 'node:fs';
import {mkdir, readFile, readdir, stat, writeFile} from 'node:fs/promises';
import {createServer} from 'node:http';
import {createRequire} from 'node:module';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {parseArgs} from 'node:util';
import vm from 'node:vm';
import * as webdriver from './webdriver.mjs';

const TYPES = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.mjs': 'text/javascript; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.json': 'application/json',
    '.png': 'image/png',
    '.jpg': 'image/jpeg',
    '.jpeg': 'image/jpeg',
    '.gif': 'image/gif',
    '.svg': 'image/svg+xml',
    '.ico': 'image/x-icon',
    '.webm': 'video/webm',
    '.mp4': 'video/mp4',
    '.ogg': 'audio/ogg',
    '.mp3': 'audio/mpeg',
    '.wav': 'audio/wav',
    '.wasm': 'application/wasm',
    '.txt': 'text/plain; charset=utf-8',
    '.wgsl': 'text/plain; charset=utf-8',
};

const ANALYTICS_STUB = "window.GoogleAnalyticsObject = 'ga';\nwindow.ga = window.ga || function() { (window.ga.q = window.ga.q || []).push(arguments); };\nwindow.ga.l = Date.now();\n";
const ANALYTICS_PATHS = new Set(['/src/js/lib/ga.js']);
const ADAPTER_URL = 'https://webrtc.github.io/adapter/adapter-latest.js';
const FONTS = /(?:https?:)?\/\/fonts\.googleapis\.com\/[^"'\s>]*/g;

const CHROMIUM_FLAGS = [
    '--headless=new',
    '--allow-insecure-localhost',
    '--use-fake-device-for-media-stream',
    '--use-fake-ui-for-media-stream',
    '--allow-file-access-from-files',
    '--autoplay-policy=no-user-gesture-required',
    '--no-first-run',
    '--no-default-browser-check',
    '--window-size=800,600',
];

export function parseOptions(argv) {
    const {values} = parseArgs({
        args: argv,
        options: {
            browser: {type: 'string'},
            webdriver: {type: 'string'},
            binary: {type: 'string'},
            samples: {type: 'string'},
            tests: {type: 'string'},
            output: {type: 'string'},
            adapter: {type: 'string'},
            'test-timeout': {type: 'string', default: '30000'},
            filter: {type: 'string', multiple: true, default: []},
        },
        strict: true,
    });
    for (const name of ['browser', 'webdriver', 'binary', 'samples', 'tests', 'output']) {
        if (!values[name]) {
            throw new Error(`--${name} is required`);
        }
    }
    if (!['eden', 'chromium'].includes(values.browser)) {
        throw new Error('--browser must be eden or chromium');
    }
    const timeout = Number(values['test-timeout']);
    if (!Number.isInteger(timeout) || timeout < 1000 || timeout > 600000) {
        throw new Error('--test-timeout must be between 1000 and 600000 milliseconds');
    }
    return {...values, testTimeout: timeout};
}

export function capabilitiesFor(browser, binary) {
    const shared = {acceptInsecureCerts: true};
    if (browser === 'eden') {
        return {browserName: 'Eden', ...shared, 'wpe:browserOptions': {binary, args: ['--automation']}};
    }
    return {browserName: 'chrome', ...shared, 'goog:chromeOptions': {binary, args: CHROMIUM_FLAGS}};
}

export function rewritePage(html, adapter) {
    const fonts = html.replace(FONTS, '/__eden/empty.css');
    return adapter ? fonts.split(ADAPTER_URL).join('/__eden/adapter.js') : fonts;
}


export function startStaticServer(root, {adapter = null} = {}) {
    const base = path.resolve(root);
    const server = createServer(async (request, response) => {
        try {
            const url = new URL(request.url, 'http://127.0.0.1');
            const send = (type, body) => {
                response.writeHead(200, {'content-type': type, 'content-length': Buffer.byteLength(body), 'cache-control': 'no-store'});
                response.end(request.method === 'HEAD' ? undefined : body);
            };
            if (url.pathname === '/__eden/empty.css') {
                send('text/css; charset=utf-8', '');
                return;
            }
            if (url.pathname === '/__eden/adapter.js' && adapter) {
                send('text/javascript; charset=utf-8', await readFile(adapter, 'utf8'));
                return;
            }
            if (ANALYTICS_PATHS.has(url.pathname)) {
                send('text/javascript; charset=utf-8', ANALYTICS_STUB);
                return;
            }
            let target = path.resolve(base, '.' + decodeURIComponent(url.pathname));
            if (target !== base && !target.startsWith(base + path.sep)) {
                response.writeHead(403).end();
                return;
            }
            let info = await stat(target).catch(() => null);
            if (info?.isDirectory()) {
                target = path.join(target, 'index.html');
                info = await stat(target).catch(() => null);
            }
            if (!info?.isFile()) {
                response.writeHead(404, {'content-type': 'text/plain; charset=utf-8'}).end('Not found');
                return;
            }
            if (path.extname(target).toLowerCase() === '.html') {
                send(TYPES['.html'], rewritePage(await readFile(target, 'utf8'), adapter));
                return;
            }
            response.writeHead(200, {
                'content-type': TYPES[path.extname(target).toLowerCase()] ?? 'application/octet-stream',
                'content-length': info.size,
                'cache-control': 'no-store',
            });
            if (request.method === 'HEAD') {
                response.end();
                return;
            }
            createReadStream(target).pipe(response);
        } catch (error) {
            response.writeHead(500, {'content-type': 'text/plain; charset=utf-8'}).end(String(error));
        }
    });
    return new Promise((resolve, reject) => {
        server.on('error', reject);
        server.listen(0, '127.0.0.1', () => resolve({server, origin: `http://127.0.0.1:${server.address().port}`}));
    });
}

function format(value) {
    try {
        return typeof value === 'string' ? JSON.stringify(value) : JSON.stringify(value) ?? String(value);
    } catch {
        return String(value);
    }
}

function equal(left, right) {
    if (Object.is(left, right)) {
        return true;
    }
    if (typeof left !== 'object' || typeof right !== 'object' || !left || !right) {
        return false;
    }
    if (Array.isArray(left) !== Array.isArray(right)) {
        return false;
    }
    const keys = Object.keys(left);
    if (keys.length !== Object.keys(right).length) {
        return false;
    }
    return keys.every(key => equal(left[key], right[key]));
}

export function expect(actual) {
    const build = negate => {
        const check = (condition, description) => {
            if (Boolean(condition) === negate) {
                throw new Error(`expect(${format(actual)})${negate ? '.not' : ''}.${description}`);
            }
        };
        return {
            toBe: expected => check(Object.is(actual, expected), `toBe(${format(expected)})`),
            toEqual: expected => check(equal(actual, expected), `toEqual(${format(expected)})`),
            toStrictEqual: expected => check(equal(actual, expected), `toStrictEqual(${format(expected)})`),
            toBeGreaterThan: expected => check(actual > expected, `toBeGreaterThan(${format(expected)})`),
            toBeGreaterThanOrEqual: expected => check(actual >= expected, `toBeGreaterThanOrEqual(${format(expected)})`),
            toBeLessThan: expected => check(actual < expected, `toBeLessThan(${format(expected)})`),
            toBeLessThanOrEqual: expected => check(actual <= expected, `toBeLessThanOrEqual(${format(expected)})`),
            toBeTruthy: () => check(actual, 'toBeTruthy()'),
            toBeFalsy: () => check(!actual, 'toBeFalsy()'),
            toBeDefined: () => check(actual !== undefined, 'toBeDefined()'),
            toBeUndefined: () => check(actual === undefined, 'toBeUndefined()'),
            toBeNull: () => check(actual === null, 'toBeNull()'),
            toContain: expected => check(actual != null && actual.includes(expected), `toContain(${format(expected)})`),
            toMatch: expected => check(typeof actual === 'string' && (expected instanceof RegExp ? expected.test(actual) : actual.includes(expected)), `toMatch(${expected})`),
            toHaveLength: expected => check(actual != null && actual.length === expected, `toHaveLength(${format(expected)})`),
        };
    };
    const matchers = build(false);
    matchers.not = build(true);
    return matchers;
}

function createSuite(name, parent) {
    return {name, parent, children: [], beforeAll: [], afterAll: [], beforeEach: [], afterEach: [], skip: parent?.skip ?? false};
}

export function collect(source, filename, requireFor, globals) {
    const root = createSuite('', null);
    let current = root;
    const hook = kind => (fn, timeout) => current[kind].push({fn, timeout});
    const test = skip => (name, fn, timeout) => current.children.push({type: 'test', name, fn, timeout, skip: skip || current.skip});
    const describe = skip => (name, fn) => {
        const suite = createSuite(name, current);
        suite.skip = skip || current.skip;
        current.children.push({type: 'suite', suite});
        const previous = current;
        current = suite;
        try {
            fn();
        } finally {
            current = previous;
        }
    };
    const it = Object.assign(test(false), {skip: test(true), only: test(false), todo: name => current.children.push({type: 'test', name, skip: true})});
    const describeApi = Object.assign(describe(false), {skip: describe(true), only: describe(false)});
    const module = {exports: {}};
    const parameters = ['require', 'module', 'exports', '__filename', '__dirname', 'describe', 'it', 'test', 'beforeAll', 'afterAll', 'beforeEach', 'afterEach', 'expect', ...Object.keys(globals)];
    const wrapper = vm.runInThisContext(`(function (${parameters.join(', ')}) {${source}\n})`, {filename});
    wrapper(requireFor(filename), module, module.exports, filename, path.dirname(filename), describeApi, it, it,
        hook('beforeAll'), hook('afterAll'), hook('beforeEach'), hook('afterEach'), expect, ...Object.values(globals));
    return root;
}

function withTimeout(fn, timeout, description) {
    let timer;
    const invoke = () => {
        if (fn.length > 0) {
            return new Promise((resolve, reject) => {
                const done = error => (error ? reject(error instanceof Error ? error : new Error(String(error))) : resolve());
                done.fail = error => reject(error instanceof Error ? error : new Error(String(error)));
                fn(done);
            });
        }
        return fn();
    };
    return Promise.race([
        Promise.resolve().then(invoke),
        new Promise((resolve, reject) => {
            timer = setTimeout(() => {
                const error = new Error(`Exceeded timeout of ${timeout} ms for ${description}`);
                error.timeout = true;
                reject(error);
            }, timeout);
        }),
    ]).finally(() => clearTimeout(timer));
}

function titleOf(suite, name) {
    const names = [];
    for (let node = suite; node; node = node.parent) {
        if (node.name) {
            names.unshift(node.name);
        }
    }
    return [...names, name].join(' > ');
}

function hooksFor(suite, kind) {
    const chain = [];
    for (let node = suite; node; node = node.parent) {
        chain.unshift(node);
    }
    const hooks = chain.flatMap(node => node[kind]);
    return kind === 'afterEach' ? hooks.reverse() : hooks;
}

function hasTests(suite) {
    return suite.children.some(child => (child.type === 'test' ? !child.skip : hasTests(child.suite)));
}

function describeError(error) {
    if (!error) {
        return 'Unknown error';
    }
    const name = error.name && error.name !== 'Error' ? `${error.name}: ` : '';
    return `${name}${error.message ?? String(error)}`;
}

export async function runSuite(suite, timeout, record) {
    if (!hasTests(suite)) {
        for (const child of suite.children) {
            if (child.type === 'test') {
                record({name: titleOf(suite, child.name), status: 'SKIP', message: null, duration: 0});
            } else {
                await runSuite(child.suite, timeout, record);
            }
        }
        return;
    }
    let setupError = null;
    for (const hook of suite.beforeAll) {
        try {
            await withTimeout(hook.fn, hook.timeout ?? timeout, `beforeAll in ${titleOf(suite, '') || 'the file'}`);
        } catch (error) {
            setupError = error;
            break;
        }
    }
    for (const child of suite.children) {
        if (child.type === 'suite') {
            if (setupError) {
                failAll(child.suite, setupError, record);
            } else {
                await runSuite(child.suite, timeout, record);
            }
            continue;
        }
        const name = titleOf(suite, child.name);
        if (child.skip || !child.fn) {
            record({name, status: 'SKIP', message: null, duration: 0});
            continue;
        }
        if (setupError) {
            record({name, status: 'FAIL', message: `beforeAll failed: ${describeError(setupError)}`, duration: 0});
            continue;
        }
        const started = Date.now();
        let failure = null;
        try {
            for (const hook of hooksFor(suite, 'beforeEach')) {
                await withTimeout(hook.fn, hook.timeout ?? timeout, `beforeEach of "${name}"`);
            }
            await withTimeout(child.fn, child.timeout ?? timeout, `"${name}"`);
        } catch (error) {
            failure = error;
        }
        for (const hook of hooksFor(suite, 'afterEach')) {
            try {
                await withTimeout(hook.fn, hook.timeout ?? timeout, `afterEach of "${name}"`);
            } catch (error) {
                failure ??= error;
            }
        }
        record({
            name,
            status: failure ? (failure.timeout ? 'TIMEOUT' : 'FAIL') : 'PASS',
            message: failure ? describeError(failure) : null,
            duration: Date.now() - started,
        });
    }
    for (const hook of suite.afterAll) {
        try {
            await withTimeout(hook.fn, hook.timeout ?? timeout, `afterAll in ${titleOf(suite, '') || 'the file'}`);
        } catch (error) {
            record({name: titleOf(suite, 'afterAll'), status: 'FAIL', message: describeError(error), duration: 0});
        }
    }
}

function failAll(suite, error, record) {
    for (const child of suite.children) {
        if (child.type === 'suite') {
            failAll(child.suite, error, record);
        } else {
            record({name: titleOf(suite, child.name), status: child.skip ? 'SKIP' : 'FAIL', message: child.skip ? null : `beforeAll failed: ${describeError(error)}`, duration: 0});
        }
    }
}

async function walk(directory, predicate) {
    const found = [];
    const entries = await readdir(directory, {withFileTypes: true}).catch(() => []);
    for (const entry of entries.sort((left, right) => left.name.localeCompare(right.name))) {
        const target = path.join(directory, entry.name);
        if (entry.isDirectory()) {
            if (entry.name !== 'node_modules') {
                found.push(...await walk(target, predicate));
            }
        } else if (predicate(target)) {
            found.push(target);
        }
    }
    return found;
}

export async function discover(samples, tests, filters) {
    const content = path.join(samples, 'src/content');
    const upstream = (await walk(content, file => file.endsWith(`${path.sep}js${path.sep}test.js`)))
        .map(file => ({file, sample: path.relative(content, path.dirname(path.dirname(file))).split(path.sep).join('/'), source: 'upstream'}));
    const extraRoot = path.join(tests, 'extra');
    const extra = (await walk(extraRoot, file => file.endsWith('.test.js')))
        .map(file => ({file, sample: path.relative(extraRoot, file).replace(/\.test\.js$/, '').split(path.sep).join('/'), source: 'eden'}));
    const selected = [...upstream, ...extra].filter(entry => !filters.length || filters.some(filter => entry.sample.includes(filter)));
    return selected.sort((left, right) => left.sample.localeCompare(right.sample) || left.source.localeCompare(right.source));
}

export function attributable(error, sessions) {
    if (!(error instanceof webdriver.WebDriverError) || !error.sessionId) {
        return true;
    }
    return [...sessions].some(driver => driver.sessionId === error.sessionId);
}

export async function main(argv) {
    const options = parseOptions(argv);
    const log = [];
    const logChunk = chunk => log.push(String(chunk));
    const {server, origin} = await startStaticServer(options.samples, {adapter: options.adapter ?? null});
    const driverArguments = options.browser === 'eden' ? ['--host=127.0.0.1'] : [];
    const service = await webdriver.startWebDriver(options.webdriver, driverArguments, logChunk);
    const results = [];
    const files = [];
    const sessions = new Set();
    const helper = {
        async buildDriver() {
            const driver = await webdriver.createSession(service.base, capabilitiesFor(options.browser, options.binary));
            sessions.add(driver);
            return driver;
        },
    };
    const shim = {By: webdriver.By, Key: webdriver.Key, WebElement: webdriver.WebElement, error: {TimeoutError: webdriver.TimeoutError, WebDriverError: webdriver.WebDriverError}};
    const requireFor = filename => {
        const nodeRequire = createRequire(filename);
        return specifier => {
            if (specifier === 'selenium-webdriver') {
                return shim;
            }
            if (/(^|\/)test\/webdriver(\.js)?$/.test(specifier)) {
                return helper;
            }
            return nodeRequire(specifier);
        };
    };
    let current = null;
    const stray = error => {
        if (current && attributable(error, sessions)) {
            current.stray.push(describeError(error));
        } else {
            log.push(`Unattributed error: ${describeError(error)}\n`);
        }
    };
    process.on('unhandledRejection', stray);
    process.on('uncaughtException', stray);
    process.env.BASEURL = origin;
    process.env.BROWSER = options.browser === 'eden' ? 'eden' : 'chrome';
    const directory = process.cwd();
    process.chdir(path.resolve(options.samples));
    try {
        for (const entry of await discover(options.samples, options.tests, options.filter)) {
            const file = entry.sample;
            current = {stray: []};
            const started = Date.now();
            try {
                const source = await readFile(entry.file, 'utf8');
                const suite = collect(source, entry.file, requireFor, {process});
                await runSuite(suite, options.testTimeout, result => {
                    const stray = current.stray.splice(0);
                    if (stray.length && result.status === 'PASS') {
                        result.status = 'FAIL';
                        result.message = `Uncaught error during the test: ${stray.join('; ')}`;
                    }
                    results.push({file, source: entry.source, ...result});
                });
                files.push({file, source: entry.source, status: 'OK', message: null, duration: Date.now() - started});
                const mine = results.filter(result => result.file === file && result.source === entry.source);
                console.log(`${file} (${entry.source}): ${mine.filter(result => result.status === 'PASS').length}/${mine.length} passed in ${Math.round((Date.now() - started) / 1000)} s`);
            } catch (error) {
                files.push({file, source: entry.source, status: 'ERROR', message: describeError(error), duration: Date.now() - started});
                console.log(`${file} (${entry.source}): could not run: ${describeError(error)}`);
            } finally {
                for (const driver of sessions) {
                    await driver.quit().catch(error => log.push(`Could not quit a session for ${file}: ${describeError(error)}\n`));
                }
                sessions.clear();
                current = null;
            }
        }
    } finally {
        process.chdir(directory);
        await service.stop();
        server.close();
    }
    await mkdir(path.dirname(options.output), {recursive: true});
    await writeFile(options.output, JSON.stringify({
        run_info: {browser: options.browser, binary: options.binary, webdriver: options.webdriver, test_timeout: options.testTimeout},
        results,
        files,
    }, null, 2) + '\n');
    await writeFile(options.output.replace(/\.json$/, '') + '.webdriver.log', log.join(''));
    const passed = results.filter(result => result.status === 'PASS').length;
    console.log(`${passed}/${results.length} sample checks passed in ${options.browser}, ${files.filter(file => file.status !== 'OK').length} files failed to run`);
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    main(process.argv.slice(2)).then(() => process.exit(0), error => {
        console.error(error?.stack ?? String(error));
        process.exit(1);
    });
}

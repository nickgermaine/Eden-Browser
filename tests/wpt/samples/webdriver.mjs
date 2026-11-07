import {spawn} from 'node:child_process';
import net from 'node:net';

export const ELEMENT = 'element-6066-11e4-a52e-4f735466cecf';
const COMMAND_TIMEOUT = 180000;

export class WebDriverError extends Error {
    constructor(code, message, status, sessionId = null) {
        super(message);
        this.name = code;
        this.code = code;
        this.status = status;
        this.sessionId = sessionId;
    }
}

export class TimeoutError extends Error {
    constructor(message) {
        super(message);
        this.name = 'TimeoutError';
    }
}

export async function request(base, method, path, body) {
    const init = {method, signal: AbortSignal.timeout(COMMAND_TIMEOUT)};
    if (method === 'POST') {
        init.headers = {'content-type': 'application/json; charset=utf-8'};
        init.body = JSON.stringify(body ?? {});
    }
    const response = await fetch(base + path, init);
    const text = await response.text();
    let payload;
    try {
        payload = JSON.parse(text);
    } catch {
        throw new WebDriverError('unknown error', `${method} ${path} answered ${response.status}: ${text.slice(0, 300)}`, response.status);
    }
    const value = payload?.value;
    if (!response.ok || (value && typeof value === 'object' && typeof value.error === 'string')) {
        throw new WebDriverError(value?.error ?? 'unknown error', value?.message ?? text.slice(0, 300), response.status);
    }
    return value ?? null;
}

function cssEscape(value) {
    return String(value).replace(/[^a-zA-Z0-9_\u00a0-\uffff-]/g, character => `\\${character.codePointAt(0).toString(16)} `);
}

class Locator {
    constructor(using, value) {
        this.using = using;
        this.value = value;
    }

    toString() {
        return `By(${this.using}, ${this.value})`;
    }
}

export const By = {
    id: value => new Locator('css selector', `*[id="${String(value).replace(/["\\]/g, '\\$&')}"]`),
    css: value => new Locator('css selector', value),
    name: value => new Locator('css selector', `*[name="${String(value).replace(/["\\]/g, '\\$&')}"]`),
    className: value => new Locator('css selector', `.${cssEscape(value)}`),
    tagName: value => new Locator('tag name', value),
    xpath: value => new Locator('xpath', value),
    linkText: value => new Locator('link text', value),
    partialLinkText: value => new Locator('partial link text', value),
};

export const Key = {
    ENTER: '\uE007',
    RETURN: '\uE006',
    TAB: '\uE004',
    BACK_SPACE: '\uE003',
    ESCAPE: '\uE00C',
    SPACE: '\uE00D',
    ARROW_DOWN: '\uE015',
    ARROW_UP: '\uE013',
    ARROW_LEFT: '\uE012',
    ARROW_RIGHT: '\uE014',
    SHIFT: '\uE008',
    CONTROL: '\uE009',
    ALT: '\uE00A',
    META: '\uE03D',
};

function locatorOf(locator) {
    if (locator instanceof Locator) {
        return {using: locator.using, value: locator.value};
    }
    if (locator && typeof locator === 'object' && typeof locator.using === 'string') {
        return {using: locator.using, value: locator.value};
    }
    throw new TypeError(`Unsupported locator ${locator}`);
}

function scriptOf(script) {
    return typeof script === 'function' ? `return (${script}).apply(null, arguments);` : String(script);
}

function keysOf(keys) {
    return keys.map(key => String(key)).join('');
}

const ATTRIBUTE_ATOM = function(element, attribute) {
    const booleans = new Set(['allowfullscreen', 'allowpaymentrequest', 'allowusermedia', 'async', 'autofocus', 'autoplay', 'checked',
        'compact', 'complete', 'controls', 'declare', 'default', 'defaultchecked', 'defaultselected', 'defer', 'disabled', 'ended',
        'formnovalidate', 'hidden', 'indeterminate', 'iscontenteditable', 'ismap', 'itemscope', 'loop', 'multiple', 'muted',
        'nohref', 'nomodule', 'noresize', 'noshade', 'novalidate', 'nowrap', 'open', 'paused', 'playsinline', 'pubdate',
        'readonly', 'required', 'reversed', 'scoped', 'seamless', 'seeking', 'selected', 'truespeed', 'typemustmatch', 'willvalidate']);
    const aliases = {class: 'className', readonly: 'readOnly'};
    const name = attribute.toLowerCase();
    if (name === 'style') {
        return element.style ? element.style.cssText : element.getAttribute('style');
    }
    if ((name === 'selected' || name === 'checked') && ('checked' in element || 'selected' in element)) {
        const selected = element.tagName === 'OPTION' ? element.selected : element.checked;
        return selected ? 'true' : null;
    }
    if ((element.tagName === 'IMG' && name === 'src') || (element.tagName === 'A' && name === 'href')) {
        return element.getAttribute(name) ? element[name] : null;
    }
    const property = aliases[attribute] ?? attribute;
    if (booleans.has(name)) {
        return element.getAttribute(attribute) !== null || !!element[property] ? 'true' : null;
    }
    let value;
    try {
        value = element[property];
    } catch {
        value = undefined;
    }
    if (value === null || value === undefined || typeof value === 'object' || typeof value === 'function') {
        value = element.getAttribute(attribute);
    }
    return value === null || value === undefined ? null : String(value);
};

export class WebElement {
    constructor(driver, id) {
        this.driver = driver;
        this.id = id;
    }

    getId() {
        return Promise.resolve(this.id);
    }

    toJSON() {
        return {[ELEMENT]: this.id};
    }

    command(method, path, body) {
        return this.driver.command(method, `/element/${encodeURIComponent(this.id)}${path}`, body);
    }

    async click() {
        await this.command('POST', '/click', {});
    }

    async sendKeys(...keys) {
        const text = keysOf(await Promise.all(keys));
        await this.command('POST', '/value', {text});
    }

    async clear() {
        await this.command('POST', '/clear', {});
    }

    isEnabled() {
        return this.command('GET', '/enabled');
    }

    isDisplayed() {
        return this.command('GET', '/displayed');
    }

    isSelected() {
        return this.command('GET', '/selected');
    }

    getText() {
        return this.command('GET', '/text');
    }

    getTagName() {
        return this.command('GET', '/name');
    }

    getProperty(name) {
        return this.command('GET', `/property/${encodeURIComponent(name)}`);
    }

    getCssValue(name) {
        return this.command('GET', `/css/${encodeURIComponent(name)}`);
    }

    getRect() {
        return this.command('GET', '/rect');
    }

    getAttribute(name) {
        return this.driver.executeScript(`return (${ATTRIBUTE_ATOM}).apply(null, arguments);`, this, name);
    }

    findElement(locator) {
        return new WebElementPromise(this.driver, this.command('POST', '/element', locatorOf(locator)).then(value => this.driver.element(value)));
    }

    async findElements(locator) {
        const values = await this.command('POST', '/elements', locatorOf(locator));
        return values.map(value => this.driver.element(value));
    }
}

export class WebElementPromise {
    constructor(driver, promise) {
        this.driver = driver;
        this.promise = promise;
        this.promise.catch(() => {});
    }

    then(resolve, reject) {
        return this.promise.then(resolve, reject);
    }

    catch(reject) {
        return this.promise.catch(reject);
    }

    finally(callback) {
        return this.promise.finally(callback);
    }

    getId() {
        return this.promise.then(element => element.getId());
    }
}

for (const method of ['click', 'sendKeys', 'clear', 'isEnabled', 'isDisplayed', 'isSelected', 'getText', 'getTagName', 'getProperty',
    'getCssValue', 'getRect', 'getAttribute', 'findElements']) {
    WebElementPromise.prototype[method] = function(...values) {
        return this.promise.then(element => element[method](...values));
    };
}
WebElementPromise.prototype.findElement = function(locator) {
    return new WebElementPromise(this.driver, this.promise.then(element => element.findElement(locator)));
};

class Alert {
    constructor(driver) {
        this.driver = driver;
    }

    getText() {
        return this.driver.command('GET', '/alert/text');
    }

    async accept() {
        await this.driver.command('POST', '/alert/accept', {});
    }

    async dismiss() {
        await this.driver.command('POST', '/alert/dismiss', {});
    }

    async sendKeys(text) {
        await this.driver.command('POST', '/alert/text', {text: String(text)});
    }
}

class TargetLocator {
    constructor(driver) {
        this.driver = driver;
    }

    async window(handle) {
        await this.driver.command('POST', '/window', {handle});
    }

    async newWindow(type) {
        const created = await this.driver.command('POST', '/window/new', {type});
        await this.window(created.handle);
    }

    async frame(target) {
        const id = target instanceof WebElement || target instanceof WebElementPromise ? await target : target;
        await this.driver.command('POST', '/frame', {id: id instanceof WebElement ? id.toJSON() : id ?? null});
    }

    async parentFrame() {
        await this.driver.command('POST', '/frame/parent', {});
    }

    async defaultContent() {
        await this.driver.command('POST', '/frame', {id: null});
    }

    activeElement() {
        return new WebElementPromise(this.driver, this.driver.command('GET', '/element/active').then(value => this.driver.element(value)));
    }

    async alert() {
        const alert = new Alert(this.driver);
        await alert.getText();
        return alert;
    }
}

class Actions {
    constructor(driver) {
        this.driver = driver;
        this.pointer = [];
        this.keyboard = [];
    }

    sync() {
        while (this.keyboard.length < this.pointer.length) {
            this.keyboard.push({type: 'pause', duration: 0});
        }
        while (this.pointer.length < this.keyboard.length) {
            this.pointer.push({type: 'pause', duration: 0});
        }
    }

    move({origin = 'viewport', x = 0, y = 0, duration = 100} = {}) {
        const target = origin instanceof WebElement ? origin.toJSON() : origin;
        this.pointer.push({type: 'pointerMove', origin: target, x, y, duration});
        this.keyboard.push({type: 'pause', duration: 0});
        return this;
    }

    press(button = 0) {
        this.pointer.push({type: 'pointerDown', button});
        this.keyboard.push({type: 'pause', duration: 0});
        return this;
    }

    release(button = 0) {
        this.pointer.push({type: 'pointerUp', button});
        this.keyboard.push({type: 'pause', duration: 0});
        return this;
    }

    click(element, button = 0) {
        if (element) {
            this.move({origin: element});
        }
        return this.press(button).release(button);
    }

    doubleClick(element) {
        return this.click(element).click();
    }

    contextClick(element) {
        return this.click(element, 2);
    }

    pause(duration = 0) {
        this.pointer.push({type: 'pause', duration});
        this.keyboard.push({type: 'pause', duration});
        return this;
    }

    keyDown(value) {
        this.keyboard.push({type: 'keyDown', value});
        this.pointer.push({type: 'pause', duration: 0});
        return this;
    }

    keyUp(value) {
        this.keyboard.push({type: 'keyUp', value});
        this.pointer.push({type: 'pause', duration: 0});
        return this;
    }

    sendKeys(...keys) {
        for (const character of keysOf(keys)) {
            this.keyDown(character).keyUp(character);
        }
        return this;
    }

    async perform() {
        this.sync();
        const actions = [];
        if (this.pointer.some(action => action.type !== 'pause')) {
            actions.push({type: 'pointer', id: 'mouse', parameters: {pointerType: 'mouse'}, actions: this.pointer});
        }
        if (this.keyboard.some(action => action.type !== 'pause')) {
            actions.push({type: 'key', id: 'keyboard', actions: this.keyboard});
        }
        if (actions.length) {
            await this.driver.command('POST', '/actions', {actions});
        }
        this.pointer = [];
        this.keyboard = [];
    }

    async clear() {
        await this.driver.command('DELETE', '/actions');
    }
}

export class Driver {
    constructor(base, sessionId, capabilities) {
        this.base = base;
        this.sessionId = sessionId;
        this.capabilities = capabilities;
        this.closed = false;
    }

    command(method, path, body) {
        if (this.closed) {
            return Promise.reject(new WebDriverError('invalid session id', 'The session was already quit', undefined, this.sessionId));
        }
        return request(this.base, method, `/session/${encodeURIComponent(this.sessionId)}${path}`, body).catch(error => {
            if (error instanceof WebDriverError) {
                error.sessionId = this.sessionId;
            }
            throw error;
        });
    }

    element(value) {
        if (!value || typeof value !== 'object' || !(ELEMENT in value)) {
            throw new WebDriverError('unknown error', `Not an element reference: ${JSON.stringify(value)}`);
        }
        return new WebElement(this, value[ELEMENT]);
    }

    async toWire(value) {
        const resolved = value instanceof WebElementPromise ? await value : value;
        if (resolved instanceof WebElement) {
            return resolved.toJSON();
        }
        if (Array.isArray(resolved)) {
            return Promise.all(resolved.map(item => this.toWire(item)));
        }
        if (resolved && typeof resolved === 'object') {
            const entries = await Promise.all(Object.entries(resolved).map(async ([key, item]) => [key, await this.toWire(item)]));
            return Object.fromEntries(entries);
        }
        return resolved;
    }

    fromWire(value) {
        if (Array.isArray(value)) {
            return value.map(item => this.fromWire(item));
        }
        if (value && typeof value === 'object') {
            if (ELEMENT in value) {
                return new WebElement(this, value[ELEMENT]);
            }
            return Object.fromEntries(Object.entries(value).map(([key, item]) => [key, this.fromWire(item)]));
        }
        return value;
    }

    getCapabilities() {
        return Promise.resolve(new Map(Object.entries(this.capabilities ?? {})));
    }

    async get(url) {
        await this.command('POST', '/url', {url});
    }

    getCurrentUrl() {
        return this.command('GET', '/url');
    }

    getTitle() {
        return this.command('GET', '/title');
    }

    getPageSource() {
        return this.command('GET', '/source');
    }

    navigate() {
        return {
            refresh: () => this.command('POST', '/refresh', {}),
            back: () => this.command('POST', '/back', {}),
            forward: () => this.command('POST', '/forward', {}),
            to: url => this.get(url),
        };
    }

    findElement(locator) {
        return new WebElementPromise(this, this.command('POST', '/element', locatorOf(locator)).then(value => this.element(value)));
    }

    async findElements(locator) {
        const values = await this.command('POST', '/elements', locatorOf(locator));
        return values.map(value => this.element(value));
    }

    async executeScript(script, ...args) {
        return this.fromWire(await this.command('POST', '/execute/sync', {script: scriptOf(script), args: await this.toWire(args)}));
    }

    async executeAsyncScript(script, ...args) {
        return this.fromWire(await this.command('POST', '/execute/async', {script: scriptOf(script), args: await this.toWire(args)}));
    }

    wait(condition, timeout = 0, message = undefined, pollTimeout = 200) {
        const started = Date.now();
        if (condition && typeof condition.then === 'function' && typeof condition !== 'function') {
            if (!timeout) {
                return Promise.resolve(condition);
            }
            return Promise.race([
                Promise.resolve(condition),
                new Promise((resolve, reject) => setTimeout(() => reject(new TimeoutError(message ?? `Timed out waiting for promise to resolve after ${timeout}ms`)), timeout)),
            ]);
        }
        const evaluate = typeof condition === 'function' ? () => condition(this) : () => condition.fn(this);
        return new Promise((resolve, reject) => {
            const poll = () => {
                Promise.resolve().then(evaluate).then(value => {
                    const elapsed = Date.now() - started;
                    if (value) {
                        resolve(value);
                    } else if (timeout && elapsed >= timeout) {
                        reject(new TimeoutError(`${message ? message + '\n' : ''}Wait timed out after ${elapsed}ms`));
                    } else {
                        setTimeout(poll, pollTimeout);
                    }
                }, reject);
            };
            poll();
        });
    }

    sleep(milliseconds) {
        return new Promise(resolve => setTimeout(resolve, milliseconds));
    }

    getWindowHandle() {
        return this.command('GET', '/window');
    }

    getAllWindowHandles() {
        return this.command('GET', '/window/handles');
    }

    switchTo() {
        return new TargetLocator(this);
    }

    actions() {
        return new Actions(this);
    }

    manage() {
        return {
            window: () => ({
                getRect: () => this.command('GET', '/window/rect'),
                setRect: rect => this.command('POST', '/window/rect', rect),
                maximize: () => this.command('POST', '/window/maximize', {}),
                minimize: () => this.command('POST', '/window/minimize', {}),
            }),
            setTimeouts: timeouts => this.command('POST', '/timeouts', timeouts),
            getTimeouts: () => this.command('GET', '/timeouts'),
        };
    }

    async close() {
        await this.command('DELETE', '/window');
    }

    async quit() {
        if (this.closed) {
            return;
        }
        this.closed = true;
        await request(this.base, 'DELETE', `/session/${encodeURIComponent(this.sessionId)}`);
    }
}

export async function createSession(base, capabilities) {
    const value = await request(base, 'POST', '/session', {capabilities: {alwaysMatch: capabilities}});
    return new Driver(base, value.sessionId, value.capabilities);
}

export function freePort() {
    return new Promise((resolve, reject) => {
        const server = net.createServer();
        server.unref();
        server.on('error', reject);
        server.listen(0, '127.0.0.1', () => {
            const {port} = server.address();
            server.close(() => resolve(port));
        });
    });
}

export async function startWebDriver(binary, extraArguments, log) {
    const port = await freePort();
    const child = spawn(binary, [`--port=${port}`, ...extraArguments], {stdio: ['ignore', 'pipe', 'pipe']});
    child.stdout.on('data', chunk => log(chunk));
    child.stderr.on('data', chunk => log(chunk));
    const exited = new Promise(resolve => child.once('exit', (code, signal) => resolve({code, signal})));
    const base = `http://127.0.0.1:${port}`;
    const deadline = Date.now() + 30000;
    for (;;) {
        const early = await Promise.race([exited, new Promise(resolve => setTimeout(() => resolve(null), 100))]);
        if (early) {
            throw new Error(`${binary} exited before it was ready (${early.code ?? early.signal})`);
        }
        try {
            const status = await request(base, 'GET', '/status');
            if (status?.ready !== false) {
                break;
            }
        } catch (error) {
            if (Date.now() > deadline) {
                child.kill('SIGKILL');
                throw new Error(`${binary} did not answer on ${base}: ${error.message}`);
            }
        }
    }
    return {
        base,
        async stop() {
            if (child.exitCode === null && child.signalCode === null) {
                child.kill('SIGTERM');
                const stopped = await Promise.race([exited, new Promise(resolve => setTimeout(() => resolve(null), 5000))]);
                if (!stopped) {
                    child.kill('SIGKILL');
                    await exited;
                }
            }
        },
    };
}

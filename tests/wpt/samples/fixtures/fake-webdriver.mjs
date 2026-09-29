import {appendFileSync} from 'node:fs';
import {createServer} from 'node:http';

const port = Number(process.argv.find(argument => argument.startsWith('--port='))?.slice(7));
const log = process.env.FAKE_WEBDRIVER_LOG;
const elements = new Map([
    ['*[id="startButton"]', 'start'],
    ['*[id="dataChannelSend"]', 'input'],
    ['#servers>option', 'option'],
]);
const sessions = new Map();
let nextSession = 1;

function record(entry) {
    appendFileSync(log, JSON.stringify(entry) + '\n');
}

function reply(response, status, value) {
    response.writeHead(status, {'content-type': 'application/json; charset=utf-8'});
    response.end(JSON.stringify({value}));
}

function fail(response, status, error, message) {
    reply(response, status, {error, message, stacktrace: ''});
}

function execute(session, script, args) {
    if (script.includes('readyCounter')) {
        session.counter += 1;
        return session.counter > 2;
    }
    if (script.includes('willvalidate') && args[1] === 'value') {
        return session.typed;
    }
    if (script.includes('throwInPage')) {
        return {error: 'javascript error', message: 'ReferenceError: pc1 is not defined'};
    }
    return null;
}

const server = createServer((request, response) => {
    let body = '';
    request.on('data', chunk => {
        body += chunk;
    });
    request.on('end', () => {
        const payload = body ? JSON.parse(body) : null;
        const parts = request.url.split('/').filter(Boolean);
        record({method: request.method, path: request.url, body: payload});
        if (request.url === '/status') {
            reply(response, 200, {ready: true, message: 'ready'});
            return;
        }
        if (request.method === 'POST' && request.url === '/session') {
            const id = `session-${nextSession++}`;
            sessions.set(id, {counter: 0, typed: '', handles: ['window-1'], current: 'window-1'});
            reply(response, 200, {sessionId: id, capabilities: {browserName: payload.capabilities.alwaysMatch.browserName}});
            return;
        }
        const session = sessions.get(parts[1]);
        if (!session) {
            fail(response, 404, 'invalid session id', 'No such session');
            return;
        }
        const tail = '/' + parts.slice(2).join('/');
        if (request.method === 'DELETE' && tail === '/') {
            sessions.delete(parts[1]);
            reply(response, 200, null);
        } else if (request.method === 'POST' && tail === '/url') {
            reply(response, 200, null);
        } else if (request.method === 'POST' && tail === '/element') {
            const id = elements.get(payload.value);
            if (id) {
                reply(response, 200, {'element-6066-11e4-a52e-4f735466cecf': id});
            } else {
                fail(response, 404, 'no such element', `Unable to locate ${payload.value}`);
            }
        } else if (request.method === 'POST' && /^\/element\/[^/]+\/click$/.test(tail)) {
            reply(response, 200, null);
        } else if (request.method === 'GET' && /^\/element\/[^/]+\/enabled$/.test(tail)) {
            reply(response, 200, true);
        } else if (request.method === 'POST' && /^\/element\/[^/]+\/value$/.test(tail)) {
            session.typed = payload.text.replace(/\n$/, '');
            reply(response, 200, null);
        } else if (request.method === 'POST' && tail === '/execute/sync') {
            const value = execute(session, payload.script, payload.args);
            if (value && value.error) {
                fail(response, 500, value.error, value.message);
            } else {
                reply(response, 200, value);
            }
        } else if (request.method === 'GET' && tail === '/window') {
            reply(response, 200, session.current);
        } else if (request.method === 'GET' && tail === '/window/handles') {
            reply(response, 200, session.handles);
        } else if (request.method === 'POST' && tail === '/window/new') {
            const handle = `window-${session.handles.length + 1}`;
            session.handles.push(handle);
            reply(response, 200, {handle, type: payload.type});
        } else if (request.method === 'POST' && tail === '/window') {
            if (!session.handles.includes(payload.handle)) {
                fail(response, 404, 'no such window', payload.handle);
                return;
            }
            session.current = payload.handle;
            reply(response, 200, null);
        } else if (request.method === 'POST' && tail === '/actions') {
            reply(response, 200, null);
        } else {
            fail(response, 404, 'unknown command', `${request.method} ${tail}`);
        }
    });
});

server.listen(port, '127.0.0.1');
process.on('SIGTERM', () => server.close(() => process.exit(0)));

import {writeFile} from 'node:fs/promises';

const [address, output] = process.argv.slice(2);
if (!address || !output) {
    throw new Error('Usage: node remote-score.mjs http://127.0.0.1:port output.json');
}
const pause = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
const deadline = Date.now() + 1800000;

async function target() {
    const html = await fetch(address, {signal: AbortSignal.timeout(5000)}).then(response => response.text());
    for (const row of html.split('<tbody>')) {
        if (!row.includes('Speedometer3.1')) {
            continue;
        }
        const match = row.match(/\/socket\/\d+\/\d+\/[^'"<]+/);
        if (match) {
            return {path: match[0], complete: row.includes('Speedometer 3.1 Results Summary') && row.includes('#summary')};
        }
    }
    return null;
}

async function connect(path) {
    const socket = new WebSocket(address.replace('http:', 'ws:') + path);
    let nextId = 0;
    let targetId = null;
    const pending = new Map();
    function receive(message) {
        if (message.method === 'Target.targetCreated') {
            targetId = message.params.targetInfo.targetId;
        }
        if (message.method === 'Target.dispatchMessageFromTarget') {
            receive(JSON.parse(message.params.message));
        }
        const callback = pending.get(message.id);
        if (callback) {
            pending.delete(message.id);
            callback(message);
        }
    }
    socket.addEventListener('message', event => receive(JSON.parse(event.data)));
    await new Promise((resolve, reject) => {
        socket.addEventListener('open', resolve, {once: true});
        socket.addEventListener('error', reject, {once: true});
    });
    await pause(200);
    return {
        socket,
        async evaluate(expression) {
            const id = ++nextId;
            const message = {id, method: 'Runtime.evaluate', params: {expression, returnByValue: true}};
            const response = new Promise((resolve, reject) => {
                const timeout = setTimeout(() => {pending.delete(id); reject(new Error('Inspector evaluation timed out'));}, 15000);
                pending.set(id, value => {clearTimeout(timeout); resolve(value);});
            });
            socket.send(JSON.stringify(targetId ? {id: ++nextId, method: 'Target.sendMessageToTarget', params: {targetId, message: JSON.stringify(message)}} : message));
            const value = await response;
            if (value.error || value.result?.wasThrown) {
                throw new Error(JSON.stringify(value));
            }
            return value.result?.result?.value;
        }
    };
}

let selected;
const startupDeadline = Date.now() + 60000;
while (Date.now() < startupDeadline) {
    try { selected = await target(); } catch {}
    if (selected) {
        break;
    }
    await pause(200);
}
if (!selected) {
    throw new Error('No benchmark target');
}
let client = await connect(selected.path);
let ready = false;
for (let attempt = 0; attempt < 120; ++attempt) {
    ready = await client.evaluate("document.readyState==='complete' && typeof benchmarkClient==='object' && !!document.querySelector('.start-tests-button')");
    if (ready) {
        break;
    }
    await pause(500);
}
if (!ready) {
    throw new Error('Benchmark did not load');
}
const before = await client.evaluate('({viewport:[innerWidth,innerHeight,devicePixelRatio],visibility:document.visibilityState,userAgent:navigator.userAgent})');
console.log(JSON.stringify({phase: 'ready', ...before}));
const startedAt = Date.now();
await client.evaluate(`(()=>{
    setTimeout(()=>document.querySelector('.start-tests-button').click(),2000);
    return true;
})()`);
client.socket.close();
console.log(JSON.stringify({phase: 'running-detached'}));
while (Date.now() < deadline) {
    await pause(5000);
    selected = await target();
    if (selected?.complete) {
        break;
    }
}
if (!selected?.complete) {
    throw new Error('Benchmark timed out');
}
client = await connect(selected.path);
const result = await client.evaluate(`({
    score:benchmarkClient._computeResults(benchmarkClient._measuredValuesList,'score'),
    measuredValues:benchmarkClient._measuredValuesList,
    fullJSON:benchmarkClient._formattedJSONResult({modern:true}),
    viewport:[innerWidth,innerHeight,devicePixelRatio],
    visibility:document.visibilityState,
    userAgent:navigator.userAgent,
    timeOrigin:performance.timeOrigin
})`);
client.socket.close();
result.before = before;
result.startedAt = startedAt;
result.completedAt = Date.now();
await writeFile(output, JSON.stringify(result, null, 2));
if (!result.score.isValid || result.measuredValues.length !== 10 || result.measuredValues.some(value => Object.keys(value.tests).length !== 20) || result.visibility !== 'visible' || JSON.stringify(before.viewport) !== JSON.stringify(result.viewport)) {
    throw new Error('Invalid or incomplete benchmark');
}
console.log(JSON.stringify({phase:'complete',score:result.score.mean,viewport:result.viewport}));

import { spawn } from "node:child_process";
import { mkdir, writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { setTimeout as delay } from "node:timers/promises";

let activeBrowser;

class DevToolsClient {
    constructor(url) {
        this.socket = new WebSocket(url);
        this.nextId = 1;
        this.pending = new Map();
        this.ready = new Promise((resolveReady, rejectReady) => {
            this.socket.onopen = resolveReady;
            this.socket.onerror = rejectReady;
        });
        this.socket.onmessage = event => {
            const message = JSON.parse(event.data);
            if (!message.id) {
                return;
            }
            const pending = this.pending.get(message.id);
            if (!pending) {
                return;
            }
            this.pending.delete(message.id);
            if (message.error) {
                pending.reject(new Error(message.error.message));
            } else {
                pending.resolve(message.result);
            }
        };
    }

    async send(method, params = {}) {
        await this.ready;
        const id = this.nextId++;
        const response = new Promise((resolveResponse, rejectResponse) => {
            this.pending.set(id, { resolve: resolveResponse, reject: rejectResponse });
        });
        this.socket.send(JSON.stringify({ id, method, params }));
        return response;
    }

    async evaluate(expression, awaitPromise = false) {
        const response = await this.send("Runtime.evaluate", {
            expression,
            awaitPromise,
            returnByValue: true,
            userGesture: true,
        });
        if (response.exceptionDetails) {
            throw new Error(response.exceptionDetails.exception?.description ?? response.exceptionDetails.text);
        }
        return response.result.value;
    }

    close() {
        this.socket.close();
    }
}

function parseArguments(argumentsList) {
    const options = {
        executable: resolve("build-cef-direct/eden-cef-direct"),
        outputDirectory: resolve("build-cef-direct/speedometer-results"),
        name: "run",
        port: 9333,
        connectOnly: false,
        inspectOnly: false,
        timeoutMinutes: 30,
        browserArguments: [],
    };
    let browserArguments = false;
    for (const argument of argumentsList) {
        if (argument === "--") {
            browserArguments = true;
        } else if (browserArguments) {
            options.browserArguments.push(argument);
        } else if (argument === "--connect-only") {
            options.connectOnly = true;
        } else if (argument === "--inspect-only") {
            options.inspectOnly = true;
        } else if (argument.startsWith("--name=")) {
            options.name = argument.slice("--name=".length);
        } else if (argument.startsWith("--port=")) {
            options.port = Number(argument.slice("--port=".length));
        } else if (argument.startsWith("--timeout-minutes=")) {
            options.timeoutMinutes = Number(argument.slice("--timeout-minutes=".length));
        } else if (argument.startsWith("--executable=")) {
            options.executable = resolve(argument.slice("--executable=".length));
        } else if (argument.startsWith("--output-directory=")) {
            options.outputDirectory = resolve(argument.slice("--output-directory=".length));
        } else {
            throw new Error(`Unknown controller argument: ${argument}`);
        }
    }
    return options;
}

async function findPage(port, timeoutMilliseconds = 30000) {
    const deadline = Date.now() + timeoutMilliseconds;
    while (Date.now() < deadline) {
        try {
            const response = await fetch(`http://127.0.0.1:${port}/json/list`);
            const targets = await response.json();
            const page = targets.find(target => target.type === "page" && target.url.includes("Speedometer3.1"));
            if (page) {
                return page;
            }
        } catch {
        }
        await delay(250);
    }
    throw new Error(`No Speedometer page appeared on port ${port}`);
}

async function inspectPage(client) {
    return client.evaluate(`(() => ({
        title: document.title,
        readyState: document.readyState,
        buttons: Array.from(document.querySelectorAll("button")).map(button => ({
            id: button.id,
            className: button.className,
            text: button.innerText,
        })),
        links: Array.from(document.querySelectorAll("a")).map(link => ({
            id: link.id,
            className: link.className,
            text: link.innerText,
        })),
        interestingGlobals: Object.keys(window).filter(key => /speed|bench|test|result|suite/i.test(key)).sort(),
        benchmarkClient: window.benchmarkClient ? {
            keys: Object.keys(window.benchmarkClient),
            prototypeKeys: Object.getOwnPropertyNames(Object.getPrototypeOf(window.benchmarkClient)),
            formattedJSONSource: window.benchmarkClient._formattedJSONResult.toString(),
            computeResultsSource: window.benchmarkClient._computeResults.toString(),
            metrics: window.benchmarkClient._metrics,
            measuredValues: window.benchmarkClient._measuredValuesList,
        } : null,
        suites: window.Suites ? {
            type: typeof window.Suites,
            count: window.Suites.length,
            first: window.Suites[0],
        } : null,
    }))()`);
}

async function waitForDocument(client) {
    const deadline = Date.now() + 30000;
    while (Date.now() < deadline) {
        const state = await client.evaluate(`({
            readyState: document.readyState,
            hasStartButton: Boolean(document.querySelector(".start-tests-button")),
            hasResults: Boolean(window.benchmarkClient?._hasResults),
        })`);
        if (state.readyState === "complete" && (state.hasStartButton || state.hasResults)) {
            return;
        }
        await delay(250);
    }
    throw new Error("Speedometer did not finish loading");
}

async function runBenchmark(client, timeoutMinutes) {
    await client.evaluate(`(() => {
        const button = document.querySelector(".start-tests-button");
        if (!button) {
            throw new Error("Start Test button not found");
        }
        button.click();
        return true;
    })()`);

    const deadline = Date.now() + timeoutMinutes * 60000;
    let lastCompleted = -1;
    while (Date.now() < deadline) {
        const state = await client.evaluate(`(() => ({
            running: benchmarkClient._isRunning,
            hasResults: benchmarkClient._hasResults,
            finished: benchmarkClient._finishedTestCount,
            title: document.title,
        }))()`);
        if (state.finished !== lastCompleted) {
            lastCompleted = state.finished;
            process.stdout.write(`${state.finished}/580 ${state.title}\n`);
        }
        if (state.hasResults) {
            return client.evaluate(`(() => ({
                title: document.title,
                score: benchmarkClient._computeResults(benchmarkClient._measuredValuesList, "score"),
                fullJSON: benchmarkClient._formattedJSONResult({ modern: true }),
                classicJSON: benchmarkClient._formattedJSONResult({ modern: false }),
                metrics: benchmarkClient._metrics,
                measuredValues: benchmarkClient._measuredValuesList,
                scoreElements: Array.from(document.querySelectorAll("[class*=score], [id*=score]"))
                    .filter(element => element.checkVisibility())
                    .map(element => ({
                        id: element.id,
                        className: element.className,
                        text: element.innerText,
                    })),
            }))()`);
        }
        await delay(1000);
    }
    throw new Error(`Speedometer exceeded ${timeoutMinutes} minutes`);
}

async function stopBrowser(browser, client) {
    try {
        await client.send("Browser.close");
        await Promise.race([
            new Promise(resolveExit => browser.once("exit", resolveExit)),
            delay(5000),
        ]);
    } catch {
    }
    if (browser.exitCode === null && browser.signalCode === null) {
        browser.kill("SIGTERM");
    }
}

async function main() {
    const options = parseArguments(process.argv.slice(2));
    let browser;
    if (!options.connectOnly) {
        const cachePath = `/tmp/eden-cef-direct-${options.name}`;
        const browserArguments = [
            "--url=https://browserbench.org/Speedometer3.1/",
            `--cache-path=${cachePath}`,
            "--window-size=1280x900",
            `--remote-debugging-port=${options.port}`,
            ...options.browserArguments,
        ];
        browser = spawn(options.executable, browserArguments, { stdio: ["ignore", "pipe", "pipe"] });
        activeBrowser = browser;
        browser.stdout.pipe(process.stdout);
        browser.stderr.pipe(process.stderr);
    }

    const page = await findPage(options.port);
    const client = new DevToolsClient(page.webSocketDebuggerUrl);
    await client.send("Runtime.enable");
    await waitForDocument(client);
    const inspection = await inspectPage(client);
    await mkdir(options.outputDirectory, { recursive: true });
    if (options.inspectOnly) {
        await writeFile(
            resolve(options.outputDirectory, `${options.name}-inspection.json`),
            `${JSON.stringify(inspection, null, 2)}\n`
        );
        process.stdout.write(`${JSON.stringify(inspection, null, 2)}\n`);
    } else {
        const result = await runBenchmark(client, options.timeoutMinutes);
        await writeFile(
            resolve(options.outputDirectory, `${options.name}-result.json`),
            `${JSON.stringify(result, null, 2)}\n`
        );
        if (typeof result.fullJSON === "string") {
            await writeFile(resolve(options.outputDirectory, `${options.name}-full.json`), `${result.fullJSON}\n`);
        }
        if (typeof result.classicJSON === "string") {
            await writeFile(
                resolve(options.outputDirectory, `${options.name}-classic.json`),
                `${result.classicJSON}\n`
            );
        }
        process.stdout.write(
            `${JSON.stringify({ title: result.title, score: result.score, scoreElements: result.scoreElements }, null, 2)}\n`
        );
    }

    if (browser) {
        await stopBrowser(browser, client);
        activeBrowser = undefined;
    }
    client.close();
}

main().catch(error => {
    if (activeBrowser && activeBrowser.exitCode === null && activeBrowser.signalCode === null) {
        activeBrowser.kill("SIGTERM");
    }
    process.stderr.write(`${error.stack ?? error.message}\n`);
    process.exitCode = 1;
});

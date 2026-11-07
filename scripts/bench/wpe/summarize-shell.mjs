import {readFile} from 'node:fs/promises';
import {join} from 'node:path';

const mean = values => values.reduce((sum, value) => sum + value, 0) / values.length;
const median = values => {
    const sorted = [...values].sort((a, b) => a - b);
    const middle = Math.floor(sorted.length / 2);
    return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
};
const rows = [];
for (const directory of process.argv.slice(2)) {
    const result = JSON.parse(await readFile(join(directory, 'result.json'), 'utf8'));
    const provenance = JSON.parse(await readFile(join(directory, 'provenance.json'), 'utf8'));
    const load = (await readFile(join(directory, 'load.jsonl'), 'utf8')).trim().split('\n').map(line => JSON.parse(line));
    const suites = Object.keys(result.measuredValues[0].tests);
    const times = {};
    let stalls = 0;
    let sync = 0;
    let async = 0;
    for (const suite of suites) {
        times[suite] = mean(result.measuredValues.map(value => value.tests[suite].total));
        for (const step of Object.keys(result.measuredValues[0].tests[suite].tests)) {
            const values = result.measuredValues.map(value => value.tests[suite].tests[step].tests);
            const asynchronous = values.map(value => value.Async);
            stalls += asynchronous.filter(value => value > median(asynchronous) + 6).length;
            sync += mean(values.map(value => value.Sync));
            async += mean(asynchronous);
        }
    }
    rows.push({directory, mode: provenance.mode, ack: provenance.ack, profiled: provenance.profiled,
        score: result.score.mean, viewport: result.viewport, iterations: result.measuredValues.length,
        suites: suites.length, asyncStalls: stalls, meanSyncSum: sync, meanAsyncSum: async,
        backgroundMeanPercent: mean(load.map(value => value.estimatedBackgroundPercent)),
        backgroundMaxPercent: Math.max(...load.map(value => value.estimatedBackgroundPercent)),
        suiteTimes: times});
}
console.log(JSON.stringify(rows, null, 2));

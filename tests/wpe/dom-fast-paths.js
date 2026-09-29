async function edenDOMProbe() {
    const failures = [];
    const measurements = {failures, collection: null, svg: null, timings: null};
    try {
        const collection = await window.edenCollectionIndexTest();
        measurements.collection = collection.records;
        failures.push(...collection.failures);
    } catch (error) {
        failures.push("collection: " + String(error && error.message || error));
    }
    document.body.replaceChildren();
    try {
        measurements.svg = (await window.edenSvgLengthParseRegression()).map(record => ({name: record.name, passed: record.passed}));
    } catch (error) {
        failures.push("svg: " + String(error && error.message || error));
    }
    document.body.replaceChildren();

    const host = document.createElement("div");
    for (let index = 0; index < 100; index++) {
        host.appendChild(document.createElement("span"));
    }
    document.body.appendChild(host);
    const children = host.children;
    const namespace = "http://www.w3.org/2000/svg";
    const svg = document.createElementNS(namespace, "svg");
    const rect = document.createElementNS(namespace, "rect");
    rect.setAttribute("x", "12");
    svg.appendChild(rect);
    document.body.appendChild(svg);

    const time = (iterations, body) => {
        body(Math.min(iterations, 1000));
        const start = performance.now();
        body(iterations);
        return performance.now() - start;
    };
    let sink = 0;
    const repetitions = 5;
    const samples = {collectionMissMilliseconds: [], collectionTraversalMilliseconds: [], svgEqualWriteMilliseconds: [], svgChangingWriteMilliseconds: []};
    for (let repetition = 0; repetition < repetitions; repetition++) {
        samples.collectionMissMilliseconds.push(time(1000000, count => {
            for (let index = 0; index < count; index++) {
                if (children[100] === undefined) {
                    sink++;
                }
            }
        }));
        samples.collectionTraversalMilliseconds.push(time(20000, count => {
            for (let round = 0; round < count; round++) {
                let index = 0;
                while (children[index]) {
                    index++;
                }
                sink += index;
            }
        }));
        samples.svgEqualWriteMilliseconds.push(time(200000, count => {
            for (let index = 0; index < count; index++) {
                rect.setAttribute("x", "12");
            }
        }));
        samples.svgChangingWriteMilliseconds.push(time(200000, count => {
            for (let index = 0; index < count; index++) {
                rect.setAttribute("x", index & 1 ? "12" : "13");
            }
        }));
    }
    if (rect.x.baseVal.valueInSpecifiedUnits !== 12 && rect.x.baseVal.valueInSpecifiedUnits !== 13) {
        failures.push("timing loop changed the reflected length");
    }
    if (sink <= 0) {
        failures.push("timing loop did not observe collection misses");
    }
    measurements.timings = samples;
    document.body.replaceChildren();
    return {
        probe: "dom-fast-paths",
        measurements,
        viewport: [innerWidth, innerHeight, devicePixelRatio],
        visibility: document.visibilityState
    };
}

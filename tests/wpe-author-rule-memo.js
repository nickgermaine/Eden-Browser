async function edenDOMProbe() {
    const parsed = new DOMParser().parseFromString(globalThis.edenAuthorMemoMarkup, "text/html");
    for (const script of parsed.querySelectorAll("script")) {
        script.remove();
    }
    document.head.replaceChildren(...Array.from(parsed.head.childNodes, node => document.importNode(node, true)));
    document.body.replaceChildren(...Array.from(parsed.body.childNodes, node => document.importNode(node, true)));
    await new Promise(resolve => requestAnimationFrame(resolve));
    const result = await window.edenAuthorRuleMemoTest();
    const frame = document.createElement("iframe");
    frame.style.cssText = "width: 800px; height: 150px; border: 0;";
    const loaded = new Promise(resolve => frame.onload = resolve);
    frame.srcdoc = "<!doctype html><style>p.media-probe { --base-one: yes; } .media-probe[data-kind] { --base-two: yes; } p[data-kind] { --base-three: yes; } @media (min-width: 500px) { p.media-probe { --media-width: wide; } .media-probe[data-kind] { --media-state: active; } p[data-kind] { --media-third: enabled; } }</style><p class='media-probe' data-kind='one'>one</p><p class='media-probe' data-kind='one'>two</p>";
    document.body.appendChild(frame);
    await loaded;
    const rows = frame.contentDocument.querySelectorAll("p");
    function record(name, actual, expected) {
        const passed = actual === expected;
        ++result.assertions;
        result.records.push({name, actual, expected, passed});
        if (!passed) {
            result.failures.push(name + ": expected " + expected + ", received " + actual);
        }
    }
    for (const [stage, width, wide] of [["initial", 800, true], ["narrow", 320, false], ["restored", 800, true]]) {
        frame.style.width = width + "px";
        frame.getBoundingClientRect();
        await new Promise(resolve => requestAnimationFrame(resolve));
        record("native media viewport " + stage, frame.contentWindow.innerWidth, width);
        for (let index = 0; index < rows.length; ++index) {
            const style = frame.contentWindow.getComputedStyle(rows[index]);
            for (const [property, expected] of [["--base-one", "yes"], ["--base-two", "yes"], ["--base-three", "yes"], ["--media-width", wide ? "wide" : ""], ["--media-state", wide ? "active" : ""], ["--media-third", wide ? "enabled" : ""]]) {
                record("native media " + stage + " row " + index + " " + property, style.getPropertyValue(property).trim(), expected);
            }
        }
    }
    frame.remove();
    return {
        probe: "author-rule-memo",
        visibility: document.visibilityState,
        viewport: [innerWidth, innerHeight, devicePixelRatio],
        measurements: result
    };
}

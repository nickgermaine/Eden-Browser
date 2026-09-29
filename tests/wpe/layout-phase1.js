window.layoutPhase1RegressionResult = {done: false, assertions: 0, error: ""};
let assertions = 0;

(async () => {
    const assert = (condition, label) => {
        if (!condition) {
            throw new Error(label);
        }
        ++assertions;
    };
    const closeTo = (actual, expected, tolerance, label) => {
        assert(Number.isFinite(actual) && Math.abs(actual - expected) <= tolerance, label);
    };
    const frame = () => new Promise(resolve => requestAnimationFrame(resolve));
    const style = document.createElement("style");
    style.textContent = `
        .phase1-root { position: absolute; left: -12000px; top: 0; }
        .phase1-text { display: block; white-space: pre; width: max-content; font-size: 28px; line-height: normal; font-kerning: normal; font-feature-settings: "liga" 1, "kern" 1; }
        .phase1-emphasis { display: inline-block; writing-mode: vertical-rl; font-size: 28px; line-height: normal; text-emphasis-style: filled circle; }
        .phase1-input { color: rgb(12, 34, 56); }
        .phase1-check:checked { color: rgb(201, 22, 19); }
        .phase1-check:disabled { color: rgb(0, 100, 200); }
        .phase1-check:checked:disabled { color: rgb(20, 140, 60); }
        .phase1-row { color: rgb(30, 40, 50); }
    `;
    const root = document.createElement("div");
    root.className = "phase1-root";
    document.body.replaceChildren(style, root);

    const textCases = [
        {text: "office affinity AVATAR ffi", direction: "ltr"},
        {text: "café déjà vu Ελληνικά Кириллица 日本語 😀", direction: "ltr"},
        {text: "שלום 123 עולם", direction: "rtl"},
        {text: "שלום Ελληνικά אבג", direction: "rtl"},
        {text: "co\u00adoperate soft\u200bwrap", direction: "ltr"}
    ];
    const measuredText = [];
    for (const item of textCases) {
        const probe = document.createElement("span");
        probe.className = "phase1-text";
        probe.dir = item.direction;
        probe.style.fontFamily = '"Eden Missing Font", "DejaVu Sans", sans-serif';
        probe.style.fontVariationSettings = '"wght" 550';
        probe.textContent = item.text;
        root.append(probe);
        const range = document.createRange();
        range.selectNodeContents(probe);
        const textRect = range.getBoundingClientRect();
        const elementRect = probe.getBoundingClientRect();
        assert(Number.isFinite(textRect.width) && textRect.width > 0, `text width ${item.direction}`);
        assert(Number.isFinite(textRect.height) && textRect.height > 0, `text height ${item.direction}`);
        closeTo(textRect.width, elementRect.width, 1.5, `text range width ${item.direction}`);
        measuredText.push({probe, range, width: textRect.width, direction: item.direction});
    }
    await frame();
    for (const item of measuredText) {
        const repeatedWidth = item.range.getBoundingClientRect().width;
        closeTo(repeatedWidth, item.width, 0.1, `stable shaped text ${item.direction}`);
    }

    const emphasis = document.createElement("span");
    emphasis.className = "phase1-emphasis";
    emphasis.textContent = "office letters";
    root.append(emphasis);
    const emphasisRange = document.createRange();
    emphasisRange.selectNodeContents(emphasis);
    const emphasisRect = emphasisRange.getBoundingClientRect();
    assert(emphasisRect.width > 0 && emphasisRect.height > 0, "text emphasis shaping geometry");

    const flex = document.createElement("div");
    flex.style.cssText = "display:flex;flex-direction:row;align-items:stretch;width:150px";
    const flexItem = document.createElement("div");
    flexItem.style.cssText = "flex:0 1 auto;min-width:auto";
    const flexChild = document.createElement("div");
    flexChild.style.cssText = "width:82px;font-size:16px;line-height:normal";
    flexChild.textContent = "alpha beta";
    flexItem.append(flexChild);
    flex.append(flexItem);
    root.append(flex);
    const initialFlexHeight = flexItem.getBoundingClientRect().height;
    flexChild.textContent = "alpha beta ".repeat(18);
    const expandedFlexHeight = flexItem.getBoundingClientRect().height;
    assert(expandedFlexHeight > initialFlexHeight + 4, "flex descendant content grows cross size");
    flexChild.style.fontSize = "22px";
    const restyledFlexHeight = flexItem.getBoundingClientRect().height;
    assert(restyledFlexHeight >= expandedFlexHeight, "flex descendant restyle updates cross size");
    flexChild.style.fontSize = "16px";
    flexChild.textContent = "alpha beta";
    const restoredFlexHeight = flexItem.getBoundingClientRect().height;
    closeTo(restoredFlexHeight, initialFlexHeight, 1.5, "flex content restores cross size");

    const teardown = document.createElement("div");
    teardown.style.cssText = "display:block;width:180px";
    const originalChildren = [];
    for (let index = 0; index < 120; ++index) {
        const child = document.createElement("div");
        child.style.cssText = "height:12px;overflow:visible";
        child.textContent = `old row ${index}`;
        originalChildren.push(child);
        teardown.append(child);
    }
    root.append(teardown);
    assert(teardown.scrollHeight >= 1440, "bulk teardown initial layout");
    const replacement = document.createDocumentFragment();
    for (let index = 0; index < 24; ++index) {
        const child = document.createElement("div");
        child.style.cssText = "height:15px;overflow:visible";
        child.textContent = `new row ${index}`;
        replacement.append(child);
    }
    teardown.replaceChildren(replacement);
    await frame();
    assert(originalChildren.every(child => !child.isConnected), "bulk teardown detaches old roots");
    assert(teardown.children.length === 24, "bulk teardown replacement count");
    assert(teardown.scrollHeight >= 360, "bulk teardown replacement layout");
    assert(teardown.lastElementChild.getBoundingClientRect().height === 15, "bulk teardown last child geometry");
    teardown.innerHTML = "";
    assert(teardown.children.length === 0, "bulk teardown clears children");

    const form = document.createElement("form");
    const checkbox = document.createElement("input");
    checkbox.type = "checkbox";
    checkbox.className = "phase1-input phase1-check";
    const textInput = document.createElement("input");
    textInput.type = "text";
    textInput.placeholder = "Search terms";
    textInput.style.cssText = "box-sizing:border-box;width:220px;height:40px;font:16px/normal sans-serif";
    const searchInput = document.createElement("input");
    searchInput.type = "search";
    searchInput.placeholder = "Find";
    searchInput.style.cssText = "box-sizing:border-box;width:180px;height:36px";
    const numberInput = document.createElement("input");
    numberInput.type = "number";
    numberInput.value = "123.45";
    numberInput.style.cssText = "box-sizing:border-box;width:180px;height:36px";
    form.append(checkbox, textInput, searchInput, numberInput);
    root.append(form);
    const checkboxColor = () => getComputedStyle(checkbox).color;
    assert(checkboxColor() === "rgb(12, 34, 56)", "form enabled style");
    checkbox.checked = true;
    assert(checkboxColor() === "rgb(201, 22, 19)", "form checked style");
    checkbox.disabled = true;
    assert(checkboxColor() === "rgb(20, 140, 60)", "form checked disabled style");
    checkbox.checked = false;
    assert(checkboxColor() === "rgb(0, 100, 200)", "form disabled style");
    checkbox.disabled = false;
    assert(checkboxColor() === "rgb(12, 34, 56)", "form restored style");
    assert(textInput.matches(":placeholder-shown"), "placeholder shown state");
    textInput.value = "value";
    assert(!textInput.matches(":placeholder-shown"), "placeholder hidden state");
    textInput.value = "";
    assert(textInput.matches(":placeholder-shown"), "placeholder restored state");
    textInput.value = "0123456789 ".repeat(40);
    textInput.focus();
    textInput.setSelectionRange(4, 12);
    assert(textInput.selectionStart === 4 && textInput.selectionEnd === 12, "text control selection");
    assert(textInput.getBoundingClientRect().height === 40, "text control fixed height");
    assert(searchInput.getBoundingClientRect().height === 36, "search decoration geometry");
    assert(numberInput.value === "123.45" && numberInput.getBoundingClientRect().height === 36, "number control geometry");
    for (const input of [textInput, searchInput, numberInput]) {
        input.style.boxSizing = "content-box";
        const computed = getComputedStyle(input);
        const outerHeight = parseFloat(input.style.height) + parseFloat(computed.paddingTop) +
            parseFloat(computed.paddingBottom) + parseFloat(computed.borderTopWidth) +
            parseFloat(computed.borderBottomWidth);
        closeTo(input.getBoundingClientRect().height, outerHeight, 0.01, `${input.type} content-box height`);
        input.style.boxSizing = "border-box";
        closeTo(input.getBoundingClientRect().height, parseFloat(input.style.height), 0.01,
            `${input.type} restored border-box height`);
    }

    const rows = document.createElement("div");
    rows.className = "phase1-row";
    for (let index = 0; index < 80; ++index) {
        const row = document.createElement("div");
        row.textContent = `repeated declaration row ${index}`;
        rows.append(row);
    }
    root.append(rows);
    assert(Array.from(rows.children).every(row => getComputedStyle(row).color === "rgb(30, 40, 50)"), "repeated style cache baseline");
    rows.style.color = "rgb(80, 90, 100)";
    assert(Array.from(rows.children).every(row => getComputedStyle(row).color === "rgb(80, 90, 100)"), "repeated style inherited update");
    rows.style.color = "rgb(30, 40, 50)";
    assert(Array.from(rows.children).every(row => getComputedStyle(row).color === "rgb(30, 40, 50)"), "repeated style inherited restore");

    window.layoutPhase1RegressionResult = {done: true, assertions, error: ""};
})().catch(error => {
    window.layoutPhase1RegressionResult = {done: true, assertions, error: String(error)};
});

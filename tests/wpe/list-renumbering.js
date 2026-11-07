window.edenListRenumberingRegression = (() => {
    const rules = `
        html, body { margin: 0; padding: 0; background: #f5f7fa; }
        body { width: 800px; height: 600px; overflow: hidden; }
        ol { margin: 0; padding: 0; list-style-position: inside; }
        ol ol { padding-left: 24px; }
        li {
            min-height: 28px;
            margin: 0;
            padding: 0;
            color: rgb(16, 32, 48);
            font-family: monospace;
            font-size: 20px;
            font-weight: 400;
            line-height: 28px;
            white-space: pre;
        }
        li::marker {
            color: rgb(16, 32, 48);
            font-family: monospace;
            font-size: 20px;
            font-variant-numeric: tabular-nums;
            font-weight: 400;
            white-space: pre;
        }
        #list-renumbering-actual {
            position: absolute;
            z-index: 1;
            top: 40px;
            left: 16px;
            width: 240px;
        }
        #list-renumbering-references {
            position: absolute;
            z-index: 10;
            inset: 0;
            pointer-events: none;
        }
        .list-renumbering-reference {
            position: absolute;
            width: 96px;
            margin: 0;
            padding: 0;
            list-style-position: inside;
        }
        .list-renumbering-reference > li {
            list-style-type: decimal;
        }
        .list-renumbering-reference > li::marker {
            content: attr(data-marker);
        }
        #list-renumbering-phase {
            position: absolute;
            z-index: 20;
            top: 8px;
            left: 760px;
            width: 24px;
            height: 24px;
        }
        #list-renumbering-source-frame {
            position: absolute;
            z-index: 2;
            top: 500px;
            left: 16px;
            width: 220px;
            height: 28px;
            margin: 0;
            padding: 0;
            border: 0;
        }
    `;
    let sourceDocument;
    let removedReversedTail;

    function appendItem(document, list, identifier, attributes = {}) {
        const item = document.createElement("li");
        item.id = identifier;
        for (const [name, value] of Object.entries(attributes)) {
            item.setAttribute(name, value);
        }
        list.append(item);
        return item;
    }

    function makeList(document, identifier, attributes = {}) {
        const list = document.createElement("ol");
        list.id = identifier;
        for (const [name, value] of Object.entries(attributes)) {
            list.setAttribute(name, value);
        }
        return list;
    }

    function globalPosition(element) {
        const rect = element.getBoundingClientRect();
        let x = rect.left;
        let y = rect.top;
        let owner = element.ownerDocument;
        while (owner !== document) {
            const frame = owner.defaultView.frameElement;
            if (!frame) {
                throw new Error("A marker document has no embedding frame");
            }
            const frameRect = frame.getBoundingClientRect();
            x += frameRect.left;
            y += frameRect.top;
            owner = frame.ownerDocument;
        }
        if (!Number.isInteger(x) || !Number.isInteger(y)) {
            throw new Error("Marker geometry is not aligned to integer pixels");
        }
        return {x, y};
    }

    function initialize() {
        document.documentElement.style.cssText = "margin:0;padding:0;height:100%";
        document.body.style.cssText = "margin:0;padding:0;width:800px;height:600px;overflow:hidden;background:#f5f7fa";

        const style = document.createElement("style");
        style.textContent = rules;
        const actual = document.createElement("div");
        actual.id = "list-renumbering-actual";
        const references = document.createElement("div");
        references.id = "list-renumbering-references";
        const phase = document.createElement("div");
        phase.id = "list-renumbering-phase";
        phase.style.backgroundColor = "#f5f7fa";

        const descending = makeList(document, "descending", {reversed: ""});
        appendItem(document, descending, "reverse-one");
        appendItem(document, descending, "reverse-two");
        appendItem(document, descending, "reverse-three");

        const explicit = makeList(document, "explicit", {start: "7"});
        appendItem(document, explicit, "explicit-one");
        appendItem(document, explicit, "explicit-value", {value: "12"});
        appendItem(document, explicit, "explicit-three");

        const outer = makeList(document, "outer", {start: "4"});
        const outerOne = appendItem(document, outer, "outer-one");
        const inner = makeList(document, "inner");
        appendItem(document, inner, "inner-one");
        appendItem(document, inner, "inner-two");
        outerOne.append(inner);
        appendItem(document, outer, "outer-two");

        const css = makeList(document, "css");
        css.style.counterReset = "list-item 20";
        appendItem(document, css, "css-one");
        const cssSet = appendItem(document, css, "css-set");
        cssSet.style.counterSet = "list-item 7";
        appendItem(document, css, "css-three");

        const styleList = makeList(document, "style-list", {start: "10"});
        appendItem(document, styleList, "style-item", {value: "23"});

        const destination = makeList(document, "destination");
        actual.append(descending, explicit, outer, css, styleList, destination);

        const sourceFrame = document.createElement("iframe");
        sourceFrame.id = "list-renumbering-source-frame";
        document.body.replaceChildren(style, actual, references, phase, sourceFrame);
        const frameDocument = sourceFrame.contentDocument;
        sourceDocument = frameDocument;
        frameDocument.documentElement.style.cssText = "margin:0;padding:0";
        frameDocument.body.style.cssText = "margin:0;padding:0;width:220px;height:28px;overflow:hidden";
        const frameStyle = frameDocument.createElement("style");
        frameStyle.textContent = rules;
        frameDocument.head.append(frameStyle);
        const source = makeList(frameDocument, "source");
        appendItem(frameDocument, source, "adopted-item");
        frameDocument.body.append(source);

        return true;
    }

    function snapshot(expected, color) {
        document.getElementById("list-renumbering-phase").style.backgroundColor = color;
        const referenceLayer = document.getElementById("list-renumbering-references");
        referenceLayer.replaceChildren();
        const geometry = [];
        for (const [identifier, marker] of expected) {
            const element = document.getElementById(identifier) || sourceDocument.getElementById(identifier);
            if (!element) {
                throw new Error(`Missing list item ${identifier}`);
            }
            const position = globalPosition(element);
            const reference = document.createElement("ol");
            reference.className = "list-renumbering-reference";
            reference.style.left = `${position.x + 320}px`;
            reference.style.top = `${position.y}px`;
            const referenceItem = document.createElement("li");
            referenceItem.setAttribute("data-marker", marker);
            reference.append(referenceItem);
            referenceLayer.append(reference);
            const referenceRect = reference.getBoundingClientRect();
            if (!Number.isInteger(referenceRect.left) || !Number.isInteger(referenceRect.top)) {
                throw new Error("Reference marker geometry is not aligned to integer pixels");
            }
            geometry.push({
                id: identifier,
                actualX: position.x,
                actualY: position.y,
                referenceX: Math.round(referenceRect.left),
                referenceY: Math.round(referenceRect.top)
            });
        }
        return JSON.stringify(geometry);
    }

    function removeReversedTail() {
        removedReversedTail = document.getElementById("reverse-three");
        removedReversedTail.remove();
        return true;
    }

    function reinsertReversedTail() {
        document.getElementById("descending").append(removedReversedTail);
        removedReversedTail = null;
        return true;
    }

    function changeExplicitStart() {
        document.getElementById("explicit").setAttribute("start", "3");
        return true;
    }

    function hideStyleItem() {
        const item = document.getElementById("style-item");
        const position = globalPosition(item);
        item.style.display = "none";
        return JSON.stringify(position);
    }

    function showStyleItem() {
        document.getElementById("style-item").style.display = "";
        return true;
    }

    function adoptSourceItem() {
        const source = sourceDocument.getElementById("adopted-item");
        const position = globalPosition(source);
        const adopted = document.adoptNode(source);
        document.getElementById("destination").append(adopted);
        return JSON.stringify(position);
    }

    return {
        initialize,
        snapshot,
        removeReversedTail,
        reinsertReversedTail,
        changeExplicitStart,
        hideStyleItem,
        showStyleItem,
        adoptSourceItem
    };
})();

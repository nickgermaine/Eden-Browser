(function () {
    function createResult() {
        return { assertions: 0, failures: [], records: [] };
    }

    function check(result, name, actual, expected) {
        result.assertions++;
        var passed = actual === expected;
        result.records.push({ name: name, actual: actual, expected: expected, passed: passed });
        if (!passed) {
            result.failures.push(name + ": expected " + expected + ", received " + actual);
        }
    }

    function customProperty(element, name) {
        return getComputedStyle(element).getPropertyValue(name).trim();
    }

    function makeRow(document, id, className, kind, href) {
        var row = document.createElement("li");
        row.id = id;
        row.className = className;
        row.setAttribute("data-kind", kind);
        row.setAttributeNS("http://www.w3.org/1999/xlink", "xlink:href", href);
        return row;
    }

    async function capFixture(result) {
        var frame = document.createElement("iframe");
        frame.style.cssText = "position: fixed; width: 320px; height: 200px; left: -10000px;";
        document.body.appendChild(frame);
        var loaded = new Promise(function (resolve) {
            frame.onload = resolve;
        });
        frame.srcdoc = "<!doctype html><html><head></head><body></body></html>";
        await loaded;
        var frameDocument = frame.contentDocument;
        var style = frameDocument.createElement("style");
        style.textContent = "p.cap-row { --cap-tag: yes; } .cap-row[data-shape] { --cap-class: yes; } p[data-shape] { --cap-attribute: yes; }";
        frameDocument.head.appendChild(style);
        var rows = [];
        for (var index = 0; index < 34; index++) {
            var row = frameDocument.createElement("p");
            row.className = "cap-row";
            row.setAttribute("data-shape", String(index));
            frameDocument.body.appendChild(row);
            rows.push(row);
        }
        var repeat = frameDocument.createElement("p");
        repeat.className = "cap-row";
        repeat.setAttribute("data-shape", "0");
        frameDocument.body.appendChild(repeat);
        check(result, "cap fixture tag rule", customProperty(rows[0], "--cap-tag"), "yes");
        check(result, "cap fixture class rule", customProperty(rows[0], "--cap-class"), "yes");
        check(result, "cap fixture attribute rule", customProperty(rows[0], "--cap-attribute"), "yes");
        check(result, "cap fixture repeated shape", customProperty(repeat, "--cap-attribute"), "yes");
        return frame;
    }

    async function quirksFixture(result) {
        var frame = document.createElement("iframe");
        frame.style.cssText = "position: fixed; width: 320px; height: 200px; left: -10000px;";
        document.body.appendChild(frame);
        var loaded = new Promise(function (resolve) {
            frame.onload = resolve;
        });
        var markup = "<style>#CASE[data-kind] { --quirks-id: matched; } div[data-kind] { --quirks-tag: matched; } div#CASE { --quirks-attribute: matched; }</style><div id='case' data-kind='one'></div><div id='case' data-kind='one'></div>";
        var url = URL.createObjectURL(new Blob([markup], {type: "text/html"}));
        frame.src = url;
        await loaded;
        URL.revokeObjectURL(url);
        var rows = frame.contentDocument.querySelectorAll("div");
        check(result, "quirks mode document", frame.contentDocument.compatMode, "BackCompat");
        check(result, "quirks id selector", customProperty(rows[0], "--quirks-id"), "matched");
        check(result, "quirks tag selector", customProperty(rows[1], "--quirks-tag"), "matched");
        check(result, "quirks attribute selector", customProperty(rows[1], "--quirks-attribute"), "matched");
        return frame;
    }

    window.edenAuthorRuleMemoTest = async function () {
        var result = createResult();
        for (var element of document.querySelectorAll("[data-kind]")) {
            if (element.hasAttribute("xlink:href")) {
                var href = element.getAttribute("xlink:href");
                element.removeAttribute("xlink:href");
                element.setAttributeNS("http://www.w3.org/1999/xlink", "xlink:href", href);
            }
        }
        var rows = document.querySelectorAll("#rows > li");
        check(result, "repeated row tag-class", customProperty(rows[0], "--memo-tag-class"), "matched");
        check(result, "repeated row class-attribute", customProperty(rows[1], "--memo-class-attribute"), "matched");
        check(result, "repeated row id-namespaced-attribute", customProperty(rows[2], "--memo-id-namespaced-attribute"), "matched");
        check(result, "repeated row custom property", customProperty(rows[2], "--memo-universal"), "matched");
        check(result, "ancestor combinator", customProperty(rows[0], "--memo-ancestor"), "matched");
        check(result, "positional pseudo class", customProperty(rows[1], "--memo-position"), "second");

        var hasRow = document.getElementById("has-row");
        check(result, "has selector initially matches", customProperty(hasRow, "--memo-has"), "matched");
        hasRow.querySelector(".marker").remove();
        check(result, "has selector updates after descendant removal", customProperty(hasRow, "--memo-has"), "");
        var marker = document.createElement("span");
        marker.className = "marker";
        hasRow.appendChild(marker);
        check(result, "has selector updates after descendant insertion", customProperty(hasRow, "--memo-has"), "matched");

        var focusRow = document.getElementById("focus-row");
        focusRow.focus();
        check(result, "focus element selection", document.activeElement === focusRow, true);
        if (document.hasFocus()) {
            check(result, "focus state selector", customProperty(focusRow, "--memo-focus"), "matched");
        } else {
            result.records.push({name: "focus state selector", skipped: true, reason: "The native test window lacks focus"});
        }
        var checkedRow = document.getElementById("checked-row");
        check(result, "checked state selector", customProperty(checkedRow, "--memo-checked"), "matched");
        checkedRow.checked = false;
        check(result, "checked state selector updates", customProperty(checkedRow, "--memo-checked"), "");

        var inlineRows = document.querySelectorAll(".inline-row");
        var inlineRow = inlineRows[0];
        check(result, "style attribute selector", customProperty(inlineRow, "--memo-style-attribute"), "red");
        check(result, "same-shape style attribute selector", customProperty(inlineRows[1], "--memo-style-attribute"), "red");
        check(result, "inline class static selector", customProperty(inlineRow, "--memo-inline-class"), "matched");
        check(result, "inline tag static selector", customProperty(inlineRow, "--memo-inline-tag"), "matched");
        inlineRow.style.color = "blue";
        check(result, "style attribute selector updates", customProperty(inlineRow, "--memo-style-attribute"), "");

        check(result, "inherited custom property", customProperty(document.getElementById("inherited-child"), "--memo-inherited"), "parent");

        var cssomRow = document.getElementById("cssom-row");
        var styleSheet = document.getElementById("author-rule-memo-rules").sheet;
        var cssomRule = Array.from(styleSheet.cssRules).find(function (rule) {
            return rule.selectorText && rule.selectorText.indexOf("#cssom-row") === 0;
        });
        check(result, "CSSOM target rule found", !!cssomRule, true);
        check(result, "CSSOM declaration initially matches", customProperty(cssomRow, "--memo-cssom-empty"), "original");
        if (cssomRule) {
            cssomRule.style.cssText = "";
            check(result, "CSSOM empty declaration is skipped on cached rule", customProperty(cssomRow, "--memo-cssom-empty"), "");
            cssomRule.style.setProperty("--memo-cssom-empty", "restored");
            check(result, "CSSOM nonempty declaration is reused", customProperty(cssomRow, "--memo-cssom-empty"), "restored");
            cssomRule.style.removeProperty("--memo-cssom-empty");
            check(result, "CSSOM removed declaration is skipped", customProperty(cssomRow, "--memo-cssom-empty"), "");
        }

        var mutated = makeRow(document, "memo-row", "memo-row shared-row", "task", "memo");
        document.getElementById("rows").appendChild(mutated);
        check(result, "new repeated shape matches before mutation", customProperty(mutated, "--memo-class-attribute"), "matched");
        mutated.className = "other-row";
        check(result, "class mutation changes key", customProperty(mutated, "--memo-class-attribute"), "");
        mutated.className = "memo-row shared-row";
        mutated.id = "other-id";
        check(result, "id mutation changes key", customProperty(mutated, "--memo-id-namespaced-attribute"), "");
        mutated.id = "memo-row";
        mutated.setAttribute("data-kind", "other");
        check(result, "attribute value mutation changes key", customProperty(mutated, "--memo-class-attribute"), "");
        mutated.setAttribute("data-kind", "task");
        mutated.setAttributeNS("http://www.w3.org/1999/xlink", "xlink:href", "other");
        check(result, "namespaced attribute mutation changes key", customProperty(mutated, "--memo-id-namespaced-attribute"), "");

        var idOnly = document.getElementById("id-only");
        check(result, "id-only element style", customProperty(idOnly, "--memo-id-only"), "matched");
        var attrFree = document.getElementById("attribute-free");
        check(result, "attribute-free element selector collection", customProperty(attrFree, "--memo-universal"), "matched");

        var rules = styleSheet;
        var insertedIndex = rules.insertRule("#cssom-row { --memo-inserted: matched; }", rules.cssRules.length);
        check(result, "CSSOM inserted rule invalidates rule cache", customProperty(cssomRow, "--memo-inserted"), "matched");
        rules.deleteRule(insertedIndex);
        check(result, "CSSOM deleted rule invalidates rule cache", customProperty(cssomRow, "--memo-inserted"), "");

        var mediaRow = document.getElementById("media-row");
        var mediaRules = document.getElementById("author-rule-memo-media-rules").sheet;
        var mediaRule = Array.from(mediaRules.cssRules).find(function (rule) {
            return rule.media;
        });
        check(result, "media rule initially active", customProperty(mediaRow, "--memo-media"), window.innerWidth >= 500 ? "wide" : "");
        mediaRules.disabled = true;
        check(result, "disabled media stylesheet", customProperty(mediaRow, "--memo-media"), "");
        mediaRules.disabled = false;
        check(result, "enabled media stylesheet", customProperty(mediaRow, "--memo-media"), window.innerWidth >= 500 ? "wide" : "");
        if (window.testRunner && typeof testRunner.setViewSize === "function" && mediaRule) {
            var oldWidth = window.innerWidth;
            var oldHeight = window.innerHeight;
            testRunner.setViewSize(320, 600);
            check(result, "dynamic media query disabled", customProperty(mediaRow, "--memo-media"), "");
            testRunner.setViewSize(Math.max(oldWidth, 800), 600);
            check(result, "dynamic media query reenabled", customProperty(mediaRow, "--memo-media"), "wide");
            testRunner.setViewSize(oldWidth, oldHeight);
        } else {
            result.records.push({ name: "dynamic media viewport toggle", skipped: true });
        }

        var shadowHost = document.createElement("div");
        shadowHost.id = "shadow-host";
        document.body.appendChild(shadowHost);
        check(result, "shadow host selector", customProperty(shadowHost, "--memo-shadow-host"), "matched");
        var shadowRoot = shadowHost.attachShadow({ mode: "open" });
        var shadowStyle = document.createElement("style");
        shadowStyle.textContent = ".shadow-row { --memo-shadow: matched; } .shadow-row[data-kind] { --memo-shadow-attribute: matched; } p { --memo-shadow-tag: matched; }";
        shadowRoot.appendChild(shadowStyle);
        for (var shadowIndex = 0; shadowIndex < 3; shadowIndex++) {
            var shadowRow = document.createElement("p");
            shadowRow.className = "shadow-row";
            shadowRow.setAttribute("data-kind", "task");
            shadowRoot.appendChild(shadowRow);
            check(result, "shadow tree selector " + shadowIndex, customProperty(shadowRow, "--memo-shadow"), "matched");
        }

        var scopedFrame = document.createElement("iframe");
        scopedFrame.style.cssText = "position: fixed; width: 320px; height: 200px; left: -10000px;";
        document.body.appendChild(scopedFrame);
        var scopedLoaded = new Promise(function (resolve) {
            scopedFrame.onload = resolve;
        });
        scopedFrame.srcdoc = "<!doctype html><style>@scope (.scope-root) { .scope-row { --memo-scope: matched; } } .container-root { container-type: inline-size; width: 200px; } @container (min-width: 100px) { .container-row { --memo-container: matched; } }</style><div class='scope-root'><div class='scope-row'></div><div class='scope-row'></div></div><div class='container-root'><div class='container-row'></div></div>";
        await scopedLoaded;
        check(result, "scope rule matching", customProperty(scopedFrame.contentDocument.querySelector(".scope-row"), "--memo-scope"), "matched");
        check(result, "container query matching", customProperty(scopedFrame.contentDocument.querySelector(".container-row"), "--memo-container"), "matched");

        await capFixture(result);
        await quirksFixture(result);
        return result;
    };

    if (window.testRunner) {
        testRunner.dumpAsText();
        testRunner.waitUntilDone();
        window.edenAuthorRuleMemoTest().then(function (result) {
            document.body.textContent = result.failures.length ? result.failures.join("\n") : "PASS";
            testRunner.notifyDone();
        }, function (error) {
            document.body.textContent = "FAIL: " + error;
            testRunner.notifyDone();
        });
    }
})();

(function () {
    function createResults() {
        return { failures: [], records: [], check: function (name, condition) {
            this.records.push({ name: name, passed: Boolean(condition) });
            if (!condition) {
                this.failures.push("FAIL: " + name);
            }
        } };
    }

    window.edenCollectionIndexTest = async function () {
        const results = createResults();
        const host = document.createElement("div");
        const item = document.createElement("span");
        item.id = "4294967295";
        host.appendChild(item);
        document.body.appendChild(host);

        const collection = host.children;
        results.check("indexed wrapper identity", collection[0] === item && collection.item(0) === item);
        results.check("maximum array index remains a named property", collection[4294967295] === item);

        collection[7] = "blocked";
        results.check("ordinary indexed assignment creates no expando", collection[7] === undefined && !Object.prototype.hasOwnProperty.call(collection, "7"));
        const stringKeyWriteSucceeded = Reflect.set(collection, "6", "blocked");
        results.check("ordinary string-key indexed assignment creates no expando", !stringKeyWriteSucceeded && !Object.prototype.hasOwnProperty.call(collection, "6"));

        let strictAssignmentThrew = false;
        try {
            (function (target) {
                "use strict";
                target[8] = "blocked";
            })(collection);
        } catch (error) {
            strictAssignmentThrew = error instanceof TypeError;
        }
        results.check("strict ordinary indexed assignment still throws", strictAssignmentThrew);

        let definePropertyThrew = false;
        try {
            Object.defineProperty(collection, "9", { value: "blocked" });
        } catch (error) {
            definePropertyThrew = error instanceof TypeError;
        }
        results.check("indexed defineOwnProperty still throws", definePropertyThrew && !Object.prototype.hasOwnProperty.call(collection, "9"));

        let inheritedGetterCalls = 0;
        const prototype = Object.getPrototypeOf(collection);
        Object.defineProperty(prototype, "17", {
            configurable: true,
            get: function () {
                inheritedGetterCalls += 1;
                return "inherited-value";
            }
        });
        results.check("prototype indexed getter remains visible", collection[17] === "inherited-value" && inheritedGetterCalls === 1);
        delete prototype[17];

        let maximumIndexGetterCalls = 0;
        Object.defineProperty(prototype, "4294967294", {
            configurable: true,
            get: function () {
                maximumIndexGetterCalls += 1;
                return "maximum-index-value";
            }
        });
        results.check("maximum array index remains inherited", collection[4294967294] === "maximum-index-value" && maximumIndexGetterCalls === 1);
        delete prototype[4294967294];

        const form = document.createElement("form");
        const control = document.createElement("input");
        control.name = "collection-index-control";
        form.appendChild(control);
        document.body.appendChild(form);
        results.check("HTMLFormControlsCollection indexed wrapper identity", form.elements[0] === control);
        results.check("HTMLFormControlsCollection named lookup", form.elements.namedItem("collection-index-control") === control);

        const select = document.createElement("select");
        const option = document.createElement("option");
        select.appendChild(option);
        document.body.appendChild(select);
        results.check("HTMLOptionsCollection indexed wrapper identity", select.options[0] === option);

        customElements.define("extends-li", class extends HTMLLIElement {}, { extends: "li" });
        collection[7] = "quirk-expando";
        const descriptor = Object.getOwnPropertyDescriptor(collection, "7");
        results.check("site quirk indexed expando remains readable", collection[7] === "quirk-expando" && descriptor && descriptor.value === "quirk-expando" && descriptor.configurable);
        const stringKeyQuirkWriteSucceeded = Reflect.set(collection, "6", "quirk-string-expando");
        results.check("site quirk string-key indexed expando remains readable", stringKeyQuirkWriteSucceeded && collection[6] === "quirk-string-expando");

        window.edenCollectionIndexProbe = collection;
        const frame = document.createElement("iframe");
        const loaded = new Promise(function (resolve) {
            frame.onload = resolve;
        });
        frame.srcdoc = "<!doctype html><script>parent.edenCollectionIndexFrameValue = parent.edenCollectionIndexProbe[7]<\/script>";
        document.body.appendChild(frame);
        await loaded;
        results.check("quirk expando survives reads from a non-quirks realm", window.edenCollectionIndexFrameValue === "quirk-expando");

        let quirkDefinePropertyThrew = false;
        try {
            Object.defineProperty(collection, "10", { value: "blocked" });
        } catch (error) {
            quirkDefinePropertyThrew = error instanceof TypeError;
        }
        results.check("site quirk does not alter indexed defineOwnProperty", quirkDefinePropertyThrew);
        results.check("site quirk indexed expando remains deletable", delete collection[7] && collection[7] === undefined);
        results.check("site quirk string-key indexed expando remains deletable", delete collection["6"] && collection[6] === undefined);

        return results;
    };

    if (window.testRunner) {
        testRunner.dumpAsText();
        testRunner.waitUntilDone();
        window.edenCollectionIndexTest().then(function (results) {
            document.body.textContent = results.failures.length ? results.failures.join("\n") : "PASS";
            testRunner.notifyDone();
        }, function (error) {
            document.body.textContent = "FAIL: " + error;
            testRunner.notifyDone();
        });
    }
})();

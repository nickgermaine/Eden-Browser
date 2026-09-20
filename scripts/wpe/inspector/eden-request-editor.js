(function() {
    if (!window.WI || !WI.NetworkResourceDetailView || WI.edenRequests)
        return;

    const RawMarker = "__eden_raw";
    const HookSourceURL = "eden-request-hook.js";
    const PageHook = __EDEN_PAGE_HOOK__;
    const BodyMethods = new Set(["POST", "PUT", "PATCH", "DELETE"]);
    const MutatingMethods = new Set(["POST", "PUT", "PATCH", "DELETE"]);

    function encodeBody(text) {
        let bytes = new TextEncoder().encode(text);
        let binary = "";
        for (let byte of bytes)
            binary += String.fromCharCode(byte);
        return btoa(binary);
    }

    function isBinary(text) {
        return /[\u0000-\u0008\u000E-\u001F\uFFFD]/.test(text || "");
    }

    function parseURL(url) {
        try {
            return new URL(url);
        } catch {
            return null;
        }
    }

    function splitRequest(method, url, headers, body) {
        let parsed = parseURL(url);
        let params = [];
        if (parsed) {
            for (let [name, value] of parsed.searchParams) {
                if (name !== RawMarker)
                    params.push({name, value});
            }
        }
        let headerList = [];
        for (let name of Object.keys(headers || {}))
            headerList.push({name, value: String(headers[name])});
        return {
            method: (method || "GET").toUpperCase(),
            base: parsed ? parsed.origin + parsed.pathname : url,
            params,
            headers: headerList,
            body: body || "",
        };
    }

    function baseForResource(resource) {
        return splitRequest(resource.requestMethod, resource.url, resource.requestHeaders, resource.requestData);
    }

    function flattenJSON(value, path, output) {
        if (value !== null && typeof value === "object") {
            let keys = Array.isArray(value) ? value.map((item, index) => index) : Object.keys(value);
            if (!keys.length)
                output.set(path, JSON.stringify(value));
            for (let key of keys)
                flattenJSON(value[key], path ? path + "." + key : String(key), output);
            return;
        }
        output.set(path, JSON.stringify(value));
    }

    function bodyChanges(before, after) {
        if (before === after)
            return [];
        let left = null;
        let right = null;
        try {
            left = JSON.parse(before);
            right = JSON.parse(after);
        } catch {
            return [{path: "body", before, after}];
        }
        let leftValues = new Map;
        let rightValues = new Map;
        flattenJSON(left, "", leftValues);
        flattenJSON(right, "", rightValues);
        let changes = [];
        for (let [path, value] of rightValues) {
            if (leftValues.get(path) !== value)
                changes.push({path: path || "body", before: leftValues.get(path), after: value});
        }
        for (let [path, value] of leftValues) {
            if (!rightValues.has(path))
                changes.push({path: path || "body", before: value, after: undefined});
        }
        return changes;
    }

    class Draft {
        constructor(key, method, base) {
            this.key = key;
            this.matchMethod = method;
            this.matchBase = base;
            this.method = null;
            this.base = null;
            this.params = new Map;
            this.headers = new Map;
            this.body = null;
            this.armed = null;
            this.lastRun = null;
        }

        get hasEdits() {
            return this.method !== null || this.base !== null || this.params.size > 0 || this.headers.size > 0 || this.body !== null;
        }

        apply(request) {
            let params = [];
            let seenParams = new Set;
            for (let param of request.params) {
                let patch = this.params.get(param.name);
                seenParams.add(param.name);
                params.push({name: param.name, value: patch ? patch.value : param.value, enabled: patch ? patch.enabled : true, original: param.value});
            }
            for (let [name, patch] of this.params) {
                if (!seenParams.has(name))
                    params.push({name, value: patch.value, enabled: patch.enabled, original: undefined});
            }
            let headers = [];
            let seenHeaders = new Set;
            for (let header of request.headers) {
                let id = header.name.toLowerCase();
                let patch = this.headers.get(id);
                seenHeaders.add(id);
                headers.push({name: header.name, value: patch ? patch.value : header.value, enabled: patch ? patch.enabled : true, original: header.value});
            }
            for (let [id, patch] of this.headers) {
                if (!seenHeaders.has(id))
                    headers.push({name: patch.name, value: patch.value, enabled: patch.enabled, original: undefined});
            }
            return {
                method: this.method ?? request.method,
                originalMethod: request.method,
                base: this.base ?? request.base,
                originalBase: request.base,
                params,
                headers,
                body: this.body ?? request.body,
                originalBody: request.body,
            };
        }

        static finalURL(applied) {
            let query = new URLSearchParams;
            for (let param of applied.params) {
                if (param.enabled && param.name)
                    query.append(param.name, param.value);
            }
            let text = query.toString();
            return applied.base + (text ? "?" + text : "");
        }

        static changeCount(applied) {
            let count = 0;
            if (applied.method !== applied.originalMethod)
                ++count;
            if (applied.base !== applied.originalBase)
                ++count;
            for (let item of [...applied.params, ...applied.headers]) {
                if (item.original === undefined || !item.enabled || item.value !== item.original)
                    ++count;
            }
            count += bodyChanges(applied.originalBody, applied.body).length;
            return count;
        }
    }

    class EdenRequests extends WI.Object {
        constructor() {
            super();
            this._drafts = new Map;
            this._modified = new Map;
            this._rawPending = new Map;
            this._rawCounter = 0;
            this._disabled = false;
            this._interceptions = new Map;
        }

        get site() {
            let frame = WI.networkManager.mainFrame;
            let parsed = frame ? parseURL(frame.url) : null;
            return parsed ? parsed.origin : "";
        }

        get modifiedCount() { return this._modified.size; }

        get disabled() { return this._disabled; }

        set disabled(value) {
            if (this._disabled === value)
                return;
            this._disabled = value;
            this._syncInterceptions();
            this.dispatchEventToListeners(EdenRequests.Event.Changed);
        }

        keyFor(method, base) {
            return this.site + " " + method + " " + base;
        }

        draftForRequest(request, create) {
            let key = this.keyFor(request.method, request.base);
            let draft = this._drafts.get(key);
            if (!draft && create) {
                draft = new Draft(key, request.method, request.base);
                this._drafts.set(key, draft);
            }
            return draft || null;
        }

        draftForKey(key) {
            return this._drafts.get(key) || null;
        }

        get siteDrafts() {
            let prefix = this.site + " ";
            return Array.from(this._drafts.values()).filter((draft) => draft.key.startsWith(prefix) && (draft.hasEdits || draft.armed));
        }

        modifiedInfo(resource) {
            return this._modified.get(resource.requestIdentifier) || null;
        }

        draftChanged(draft) {
            if (!draft.hasEdits && draft.armed !== "all")
                draft.armed = draft.armed === "once" ? "once" : null;
            this._syncInterceptions();
            this.dispatchEventToListeners(EdenRequests.Event.Changed);
        }

        arm(draft, mode) {
            draft.armed = mode;
            this._syncInterceptions();
            this.dispatchEventToListeners(EdenRequests.Event.Changed);
        }

        clearSite() {
            let prefix = this.site + " ";
            for (let key of Array.from(this._drafts.keys())) {
                if (key.startsWith(prefix))
                    this._drafts.delete(key);
            }
            this._syncInterceptions();
            this.dispatchEventToListeners(EdenRequests.Event.Changed);
        }

        async sendRaw(draft, applied) {
            let id = String(++this._rawCounter);
            let finalURL = Draft.finalURL(applied);
            let marker = applied.originalBase + "?" + RawMarker + "=" + id;
            this._rawPending.set(id, {draft, applied, finalURL});
            this._syncInterceptions();
            let usesBody = BodyMethods.has(applied.method) && applied.body;
            let options = {method: usesBody ? "POST" : "GET", mode: "no-cors", credentials: "include", cache: "no-store"};
            if (usesBody)
                options.body = "eden";
            let expression = "fetch(" + JSON.stringify(marker) + "," + JSON.stringify(options) + ").catch(function(){});0";
            try {
                await WI.mainTarget.RuntimeAgent.evaluate.invoke({expression, objectGroup: "eden", doNotPauseOnExceptionsAndMuteConsole: true, returnByValue: true});
            } catch {
                this._rawPending.delete(id);
                this._syncInterceptions();
            }
        }

        handleIntercepted(target, requestId, request) {
            let parsed = parseURL(request.url);
            let rawId = parsed ? parsed.searchParams.get(RawMarker) : null;
            if (rawId && this._rawPending.has(rawId)) {
                let pending = this._rawPending.get(rawId);
                this._rawPending.delete(rawId);
                this._send(target, requestId, pending.applied, pending.finalURL, true);
                this._modified.set(requestId, {kind: "raw", draftKey: pending.draft.key, sent: pending.applied, finalURL: pending.finalURL});
                this._syncInterceptions();
                this.dispatchEventToListeners(EdenRequests.Event.Changed);
                return true;
            }
            if (this._disabled)
                return false;
            let base = splitRequest(request.method, request.url, request.headers, request.postData || "");
            let draft = this.draftForRequest(base, false);
            if (!draft || !draft.armed || !draft.hasEdits)
                return false;
            let applied = draft.apply(base);
            let finalURL = Draft.finalURL(applied);
            this._send(target, requestId, applied, finalURL);
            this._modified.set(requestId, {kind: "modified", draftKey: draft.key, base, sent: applied, finalURL});
            if (draft.armed === "once")
                draft.armed = null;
            this._syncInterceptions();
            this.dispatchEventToListeners(EdenRequests.Event.Changed);
            return true;
        }

        installHook(target) {
            if (!target.hasCommand || !target.hasCommand("Page.setBootstrapScript"))
                return;
            let manager = WI.networkManager;
            let user = manager && manager._bootstrapScript && manager._bootstrapScriptEnabledSetting?.value ? manager._bootstrapScript.content : "";
            target.PageAgent.setBootstrapScript(PageHook + "\n" + (user || ""));
            if (target.hasCommand("Runtime.evaluate"))
                target.RuntimeAgent.evaluate.invoke({expression: PageHook + "\n//# sourceURL=" + HookSourceURL, objectGroup: "eden", doNotPauseOnExceptionsAndMuteConsole: true, returnByValue: true}).catch(() => {});
        }

        async evaluate(expression, awaitResult) {
            let target = WI.mainTarget;
            let result = await target.RuntimeAgent.evaluate.invoke({expression, objectGroup: "eden", doNotPauseOnExceptionsAndMuteConsole: true, returnByValue: !awaitResult});
            if (!awaitResult || result.wasThrown)
                return result.wasThrown ? null : result.result.value;
            let settled = await target.RuntimeAgent.awaitPromise.invoke({promiseObjectId: result.result.objectId, returnByValue: true});
            return settled.wasThrown ? null : settled.result.value;
        }

        originFor(resource) {
            let time = Math.round((resource.requestSentWalltime || 0) * 1000);
            let expression = "window.__edenHook ? window.__edenHook.originFor(" + JSON.stringify((resource.requestMethod || "GET").toUpperCase()) + "," + JSON.stringify(resource.url) + "," + time + ") : null";
            return this.evaluate(expression, false).catch(() => null);
        }

        reloadWith(draft) {
            if (draft.hasEdits && !draft.armed)
                this.arm(draft, "once");
            draft.lastRun = null;
            for (let target of WI.targets) {
                if (target.hasCommand && target.hasCommand("Page.reload")) {
                    target.PageAgent.reload.invoke({});
                    break;
                }
            }
        }

        async rerun(draft, origin) {
            if (origin.kind === "load") {
                this.reloadWith(draft);
                return;
            }
            if (draft.hasEdits && !draft.armed)
                this.arm(draft, "once");
            draft.lastRun = {pending: true, label: origin.label, effects: []};
            this.dispatchEventToListeners(EdenRequests.Event.Changed);
            let outcome = null;
            try {
                outcome = await this.evaluate("window.__edenHook.rerun(" + Number(origin.id) + ")", true);
            } catch {
                outcome = null;
            }
            draft.lastRun = {pending: false, label: origin.label, effects: outcome?.effects || [], error: outcome ? outcome.error : "The page did not answer"};
            if (draft.armed === "once")
                this.arm(draft, null);
            else
                this.dispatchEventToListeners(EdenRequests.Event.Changed);
        }

        _send(target, requestId, applied, finalURL, forceBody) {
            let headers = {};
            for (let header of applied.headers) {
                if (header.enabled && header.name)
                    headers[header.name] = header.value;
            }
            let parameters = {requestId, url: finalURL, method: applied.method, headers};
            if (BodyMethods.has(applied.method) && applied.body && (forceBody || applied.body !== applied.originalBody))
                parameters.postData = encodeBody(applied.body);
            target.NetworkAgent.interceptWithRequest.invoke(parameters);
        }

        _wantedInterceptions() {
            let wanted = new Map;
            let escape = (text) => text.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
            for (let pending of this._rawPending.values())
                wanted.set("^" + escape(pending.applied.originalBase) + "\\?" + RawMarker + "=", true);
            if (!this._disabled) {
                for (let draft of this._drafts.values()) {
                    if (draft.armed && draft.hasEdits)
                        wanted.set("^" + escape(draft.matchBase) + "(\\?.*)?$", true);
                }
            }
            return wanted;
        }

        _syncInterceptions(specificTarget) {
            let wanted = this._wantedInterceptions();
            let targets = specificTarget ? [specificTarget] : WI.targets;
            for (let target of targets) {
                if (!target.hasCommand("Network.addInterception"))
                    continue;
                let current = specificTarget ? new Set : (this._interceptions.get(target) || new Set);
                for (let pattern of wanted.keys()) {
                    if (!current.has(pattern)) {
                        target.NetworkAgent.addInterception.invoke({url: pattern, stage: "request", caseSensitive: true, isRegex: true});
                        current.add(pattern);
                    }
                }
                for (let pattern of Array.from(current)) {
                    if (!wanted.has(pattern)) {
                        target.NetworkAgent.removeInterception.invoke({url: pattern, stage: "request", caseSensitive: true, isRegex: true});
                        current.delete(pattern);
                    }
                }
                this._interceptions.set(target, current);
            }
        }
    }

    EdenRequests.Event = {Changed: "eden-requests-changed"};

    WI.edenRequests = new EdenRequests;

    let originalRequestIntercepted = WI.NetworkManager.prototype.requestIntercepted;
    WI.NetworkManager.prototype.requestIntercepted = function(target, requestId, request) {
        if (WI.edenRequests.handleIntercepted(target, requestId, request))
            return Promise.resolve();
        return originalRequestIntercepted.call(this, target, requestId, request);
    };

    let originalInitiatorLocation = WI.NetworkManager.prototype._initiatorSourceCodeLocationFromPayload;
    WI.NetworkManager.prototype._initiatorSourceCodeLocationFromPayload = function(payload) {
        let frames = payload ? (Array.isArray(payload.stackTrace) ? payload.stackTrace : payload.stackTrace?.callFrames) : null;
        if (frames?.length) {
            let hidden = (frame) => frame.url === WI.NetworkManager.bootstrapScriptURL || (frame.url || "").endsWith(HookSourceURL);
            let kept = frames.filter((frame) => !hidden(frame));
            if (kept.length && kept.length !== frames.length)
                payload = {...payload, stackTrace: Array.isArray(payload.stackTrace) ? kept : {...payload.stackTrace, callFrames: kept}};
        }
        return originalInitiatorLocation.call(this, payload);
    };

    let originalInitializeTarget = WI.NetworkManager.prototype.initializeTarget;
    WI.NetworkManager.prototype.initializeTarget = function(target) {
        originalInitializeTarget.call(this, target);
        WI.edenRequests._syncInterceptions(target);
        WI.edenRequests.installHook(target);
        if (!this._edenBootstrapWatched) {
            this._edenBootstrapWatched = true;
            let resend = () => {
                for (let item of WI.targets)
                    WI.edenRequests.installHook(item);
            };
            this.addEventListener(WI.NetworkManager.Event.BootstrapScriptCreated, resend, this);
            this.addEventListener(WI.NetworkManager.Event.BootstrapScriptEnabledChanged, resend, this);
            this.addEventListener(WI.NetworkManager.Event.BootstrapScriptDestroyed, resend, this);
        }
    };

    function element(tag, className, text) {
        let node = document.createElement(tag);
        if (className)
            node.className = className;
        if (text !== undefined)
            node.textContent = text;
        return node;
    }

    WI.EdenRequestEditorContentView = class EdenRequestEditorContentView extends WI.ContentView {
        constructor(resource) {
            super(null);
            this._resource = resource;
            this.element.classList.add("eden-request-editor");
            this._tooltip = this.element.appendChild(element("div", "eden-tooltip"));
            this._content = this.element.appendChild(element("div", "eden-editor-content"));
            WI.edenRequests.addEventListener(EdenRequests.Event.Changed, this._render, this);
            this._resource.addEventListener(WI.Resource.Event.ResponseReceived, this._render, this);
            this._resource.addEventListener(WI.Resource.Event.RequestHeadersDidChange, this._render, this);
        }

        attached() {
            super.attached();
            this._render();
            if (this._origin === undefined) {
                this._origin = null;
                WI.edenRequests.originFor(this._resource).then((origin) => {
                    this._origin = origin || null;
                    this._originChecked = true;
                    this._render();
                });
            }
        }

        get _base() {
            let info = WI.edenRequests.modifiedInfo(this._resource);
            if (info && info.base)
                return info.base;
            if (info && info.kind === "raw") {
                let sent = info.sent;
                return {method: sent.originalMethod, base: sent.originalBase, params: sent.params.filter((item) => item.original !== undefined).map((item) => ({name: item.name, value: item.original})), headers: sent.headers.filter((item) => item.original !== undefined).map((item) => ({name: item.name, value: item.original})), body: sent.originalBody};
            }
            return baseForResource(this._resource);
        }

        _draft(create) {
            let info = WI.edenRequests.modifiedInfo(this._resource);
            if (info) {
                let existing = WI.edenRequests.draftForKey(info.draftKey);
                if (existing)
                    return existing;
            }
            return WI.edenRequests.draftForRequest(this._base, create);
        }

        _render() {
            if (!this.isAttached)
                return;
            let active = document.activeElement;
            let focusKey = active && this.element.contains(active) ? active.dataset.edenKey : null;
            let selection = focusKey && active.selectionStart !== undefined ? [active.selectionStart, active.selectionEnd] : null;
            let scrollTop = this.element.scrollTop;
            let base = this._base;
            let draft = this._draft(false);
            let applied = (draft || new Draft("", base.method, base.base)).apply(base);
            let content = this._content;
            content.removeChildren();
            this._hideTooltip();

            let info = WI.edenRequests.modifiedInfo(this._resource);
            let banner = content.appendChild(element("div", "eden-banner"));
            banner.append("Shared draft for ");
            banner.appendChild(element("code", null, base.method + " " + (parseURL(base.base)?.pathname ?? base.base)));
            banner.append(info ? (info.kind === "raw" ? ". This row was sent raw by Eden." : ". Eden modified this request.") : ". Edits here apply to every matching request.");

            let urlRow = content.appendChild(element("div", "eden-url-row"));
            let method = urlRow.appendChild(element("select", "eden-method"));
            for (let name of ["GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"]) {
                let option = method.appendChild(element("option", null, name));
                option.selected = name === applied.method;
            }
            if (applied.method !== applied.originalMethod)
                this._markChanged(method, applied.originalMethod, applied.method, () => this._edit((target) => { target.method = null; }));
            method.addEventListener("change", () => this._edit((target) => { target.method = method.value === base.method ? null : method.value; }));
            let url = urlRow.appendChild(element("input", "eden-url"));
            url.value = applied.base;
            url.dataset.edenKey = "url";
            url.spellcheck = false;
            if (applied.base !== applied.originalBase)
                this._markChanged(url, applied.originalBase, applied.base, () => this._edit((target) => { target.base = null; }));
            url.addEventListener("change", () => this._edit((target) => { target.base = url.value === base.base ? null : url.value; }));

            if (this._originChecked) {
                let strip = content.appendChild(element("div", "eden-origin"));
                if (this._origin) {
                    strip.append("Started by ");
                    strip.appendChild(element("code", null, this._origin.label));
                    strip.appendChild(element("span", this._origin.available ? "eden-chip armed" : "eden-chip", this._origin.available ? "Re-runnable" : "Origin is gone"));
                } else
                    strip.append("Eden did not see what started this request, so it cannot be re-run in the app. Arm it and trigger it yourself.");
            }

            let count = Draft.changeCount(applied);
            let summary = content.appendChild(element("div", "eden-summary"));
            let chip = summary.appendChild(element("span", count ? "eden-chip changed" : "eden-chip", count === 1 ? "1 change" : count + " changes"));
            if (draft && draft.armed)
                summary.appendChild(element("span", "eden-chip armed", draft.armed === "all" ? "Armed for all matches" : "Armed for next match"));
            summary.appendChild(element("span", "eden-spacer"));
            let reset = summary.appendChild(element("button", "eden-button", "Reset all"));
            reset.disabled = !count;
            reset.addEventListener("click", () => this._edit((target) => {
                target.method = null;
                target.base = null;
                target.params.clear();
                target.headers.clear();
                target.body = null;
            }));

            this._renderList(content, "Query params", applied.params, "params", (name) => name);
            this._renderList(content, "Headers", applied.headers, "headers", (name) => name.toLowerCase());
            this._renderBody(content, applied, base);
            this._renderRun(content, applied, draft, info);

            this.element.scrollTop = scrollTop;
            if (focusKey) {
                let target = this.element.querySelector("[data-eden-key=\"" + CSS.escape(focusKey) + "\"]");
                if (target) {
                    target.focus();
                    if (selection && target.setSelectionRange)
                        target.setSelectionRange(selection[0], selection[1]);
                }
            }
        }

        _edit(callback) {
            let draft = this._draft(true);
            callback(draft);
            WI.edenRequests.draftChanged(draft);
        }

        _markChanged(node, before, after, revert) {
            node.classList.add("eden-changed");
            node.addEventListener("mouseenter", () => this._showTooltip(node, before, after));
            node.addEventListener("mouseleave", () => this._hideTooltip());
            let holder = node.parentNode;
            let button = holder.appendChild(element("button", "eden-revert"));
            button.title = "Revert";
            button.addEventListener("click", revert);
        }

        _showTooltip(node, before, after) {
            let tooltip = this._tooltip;
            tooltip.removeChildren();
            let rows = before === undefined ? [["Added", "Not in the original request"], ["New", String(after)]] : after === undefined ? [["Old", String(before)], ["Removed", "Not sent"]] : [["Old", String(before)], ["New", String(after)]];
            for (let [label, value] of rows) {
                let row = tooltip.appendChild(element("div", "eden-tooltip-row"));
                row.appendChild(element("span", "eden-tooltip-label", label));
                row.appendChild(element("span", "eden-tooltip-value", value));
            }
            let bounds = node.getBoundingClientRect();
            let host = this.element.getBoundingClientRect();
            tooltip.style.left = Math.max(8, Math.min(bounds.left - host.left, host.width - 280)) + "px";
            tooltip.style.top = (bounds.bottom - host.top + this.element.scrollTop + 4) + "px";
            tooltip.classList.add("visible");
        }

        _hideTooltip() {
            this._tooltip.classList.remove("visible");
        }

        _renderList(content, title, items, field, identify) {
            let header = content.appendChild(element("div", "eden-section"));
            header.appendChild(element("span", "eden-section-title", title));
            header.appendChild(element("span", "eden-section-count", String(items.length)));
            header.appendChild(element("span", "eden-spacer"));
            let add = header.appendChild(element("button", "eden-button", "Add"));
            add.addEventListener("click", () => this._edit((draft) => {
                let index = 1;
                let name = field === "params" ? "param" : "x-header";
                while (draft[field].has(identify(name + index)) || items.some((item) => identify(item.name) === identify(name + index)))
                    ++index;
                draft[field].set(identify(name + index), {name: name + index, value: "", enabled: true});
            }));
            if (!items.length)
                return;
            let list = content.appendChild(element("div", "eden-list"));
            items.forEach((item, index) => {
                let row = list.appendChild(element("div", item.enabled ? "eden-row" : "eden-row disabled"));
                let toggle = row.appendChild(element("input"));
                toggle.type = "checkbox";
                toggle.checked = item.enabled;
                toggle.addEventListener("change", () => this._patchItem(field, identify, item, {enabled: toggle.checked}));
                let nameCell = row.appendChild(element("span", "eden-cell name"));
                let added = item.original === undefined;
                if (added) {
                    let nameInput = nameCell.appendChild(element("input", "eden-input"));
                    nameInput.value = item.name;
                    nameInput.spellcheck = false;
                    nameInput.dataset.edenKey = field + ":name:" + index;
                    nameInput.addEventListener("change", () => this._edit((draft) => {
                        let patch = draft[field].get(identify(item.name)) || {name: item.name, value: item.value, enabled: item.enabled};
                        draft[field].delete(identify(item.name));
                        if (nameInput.value) {
                            patch.name = nameInput.value;
                            draft[field].set(identify(nameInput.value), patch);
                        }
                    }));
                } else
                    nameCell.textContent = item.name;
                let valueCell = row.appendChild(element("span", "eden-cell value"));
                let valueInput = valueCell.appendChild(element("input", "eden-input"));
                valueInput.value = item.value;
                valueInput.spellcheck = false;
                valueInput.dataset.edenKey = field + ":value:" + index;
                valueInput.addEventListener("change", () => this._patchItem(field, identify, item, {value: valueInput.value}));
                let changed = added || !item.enabled || item.value !== item.original;
                if (changed) {
                    this._markChanged(valueInput, item.original, item.enabled ? item.value : undefined, () => this._edit((draft) => {
                        draft[field].delete(identify(item.name));
                    }));
                }
            });
        }

        _patchItem(field, identify, item, change) {
            this._edit((draft) => {
                let id = identify(item.name);
                let patch = draft[field].get(id) || {name: item.name, value: item.value, enabled: item.enabled};
                Object.assign(patch, change);
                if (item.original !== undefined && patch.enabled && patch.value === item.original)
                    draft[field].delete(id);
                else
                    draft[field].set(id, patch);
            });
        }

        _renderBody(content, applied, base) {
            let header = content.appendChild(element("div", "eden-section"));
            header.appendChild(element("span", "eden-section-title", "Body"));
            let valid = null;
            if (applied.body.trim()) {
                try {
                    JSON.parse(applied.body);
                    valid = true;
                } catch {
                    valid = /^\s*[\[{]/.test(applied.body) ? false : null;
                }
            }
            if (valid !== null)
                header.appendChild(element("span", valid ? "eden-valid" : "eden-invalid", valid ? "Valid JSON" : "Invalid JSON"));
            header.appendChild(element("span", "eden-spacer"));
            let format = header.appendChild(element("button", "eden-button", "Format"));
            format.disabled = valid !== true;
            format.addEventListener("click", () => this._edit((draft) => {
                let text = JSON.stringify(JSON.parse(applied.body), null, 2);
                draft.body = text === base.body ? null : text;
            }));
            if (isBinary(applied.originalBody) && applied.body === applied.originalBody) {
                content.appendChild(element("div", "eden-banner", "This body is binary, so Eden leaves it untouched. Header, URL and query edits still apply."));
                return;
            }
            let changes = bodyChanges(applied.originalBody, applied.body);
            if (changes.length) {
                let chips = content.appendChild(element("div", "eden-body-changes"));
                for (let change of changes) {
                    let holder = chips.appendChild(element("span", "eden-body-change"));
                    let chip = holder.appendChild(element("span", "eden-chip", change.path));
                    this._markChanged(chip, change.before, change.after, () => this._edit((draft) => { draft.body = null; }));
                }
            }
            let body = content.appendChild(element("textarea", "eden-body"));
            body.value = applied.body;
            body.spellcheck = false;
            body.dataset.edenKey = "body";
            body.rows = Math.max(4, Math.min(16, applied.body.split("\n").length + 1));
            if (changes.length)
                body.classList.add("eden-changed");
            body.addEventListener("change", () => this._edit((draft) => { draft.body = body.value === base.body ? null : body.value; }));
        }

        _renderRun(content, applied, draft, info) {
            if (MutatingMethods.has(applied.method))
                content.appendChild(element("div", "eden-warning", applied.method + " can change data on the server. Sending it again repeats that change."));
            let menu = content.appendChild(element("div", "eden-run"));
            let hasEdits = !!(draft && draft.hasEdits);
            let add = (title, detail, enabled, active, action) => {
                let row = menu.appendChild(element("button", active ? "eden-run-item active" : "eden-run-item"));
                row.disabled = !enabled;
                row.appendChild(element("span", "eden-run-title", active ? title + " \u00b7 armed, click to disarm" : title));
                row.appendChild(element("span", "eden-run-detail", detail));
                row.addEventListener("click", action);
            };
            let origin = this._origin;
            let canRerun = !!(origin && origin.available) && !(draft && draft.lastRun && draft.lastRun.pending);
            add("Re-run in app", origin ? (origin.available ? (origin.kind === "load" ? "This request is made while the page loads, so Eden arms your edits and reloads the page." : (origin.kind === "event" ? "Repeats the " : "Calls ") + origin.label + " again against current app state. Your edits replace the outgoing request.") : "The element that started it is no longer on the page.") : "Not available for this request.", canRerun, false, () => WI.edenRequests.rerun(this._draft(true), origin));
            add("Arm for next match", hasEdits ? "Applies these edits once, then disarms." : "Edit something first.", hasEdits, draft?.armed === "once", () => WI.edenRequests.arm(draft, draft.armed === "once" ? null : "once"));
            add("Arm for all matches", hasEdits ? "Applies to every match until you disarm. Survives reloads." : "Edit something first.", hasEdits, draft?.armed === "all", () => WI.edenRequests.arm(draft, draft.armed === "all" ? null : "all"));
            let binaryBody = isBinary(applied.body);
            add("Arm and reload", hasEdits ? "Arms these edits, then reloads the page. For requests made during page load." : "Edit something first.", hasEdits, false, () => WI.edenRequests.reloadWith(draft));
            add("Send raw", binaryBody ? "Not available, because the binary body cannot be reproduced exactly." : "Eden sends it from this page with your edits. Your app never sees the response.", !binaryBody, false, () => WI.edenRequests.sendRaw(this._draft(true), applied));
            if (WI.edenRequests.disabled)
                content.appendChild(element("div", "eden-warning", "Custom requests are disabled in the Network toolbar, so armed edits will not apply."));
            if (draft && draft.lastRun) {
                let run = content.appendChild(element("div", "eden-sent"));
                run.appendChild(element("div", "eden-sent-title", draft.lastRun.pending ? "Re-running " + draft.lastRun.label : "What your app did"));
                if (draft.lastRun.error)
                    run.appendChild(element("div", "eden-sent-row bad", draft.lastRun.error));
                for (let effect of draft.lastRun.effects)
                    run.appendChild(element("div", effect.kind === "rejected" || effect.kind === "error" ? "eden-sent-row bad" : effect.kind === "resolved" ? "eden-sent-row good" : "eden-sent-row", effect.text));
                if (!draft.lastRun.pending && !draft.lastRun.error && !draft.lastRun.effects.length)
                    run.appendChild(element("div", "eden-sent-row", "Nothing observable happened."));
            }
            if (info && info.sent) {
                let sent = content.appendChild(element("div", "eden-sent"));
                sent.appendChild(element("div", "eden-sent-title", info.kind === "raw" ? "Sent raw by Eden" : "What went out"));
                let status = this._resource.statusCode;
                sent.appendChild(element("div", "eden-sent-row", (info.sent.method + " " + info.finalURL)));
                if (status)
                    sent.appendChild(element("div", status >= 400 ? "eden-sent-row bad" : "eden-sent-row good", "Response " + status + " " + (this._resource.statusText || "")));
                for (let header of info.sent.headers) {
                    if (header.original === undefined || !header.enabled || header.value !== header.original)
                        sent.appendChild(element("div", "eden-sent-row", header.name + ": " + (header.enabled ? header.value : "(removed)")));
                }
            }
        }
    };

    let originalContentLoaded = WI.contentLoaded;
    WI.contentLoaded = function() {
        originalContentLoaded.apply(this, arguments);
        let host = WI.tabBar?.element;
        if (!host || host.querySelector(".eden-window-controls"))
            return;
        let controls = host.appendChild(element("div", "eden-window-controls"));
        for (let [command, label] of [["dock", "Switch between side and bottom"], ["window", "Move to a separate window or back"], ["close", "Close developer tools"]]) {
            let button = controls.appendChild(element("button", "eden-window-control " + command));
            button.title = label;
            button.setAttribute("aria-label", label);
            button.appendChild(element("span", "eden-window-glyph"));
            if (command === "window")
                button.appendChild(element("span", "eden-window-glyph windowed"));
            button.addEventListener("click", () => InspectorFrontendHost.inspectedURLChanged("eden-command:" + command));
        }
    };

    let originalInitialLayout = WI.NetworkResourceDetailView.prototype.initialLayout;
    WI.NetworkResourceDetailView.prototype.initialLayout = function() {
        let create = this.createDetailNavigationItem;
        this.createDetailNavigationItem = function(identifier, toolTip) {
            create.call(this, identifier, toolTip);
            if (identifier === "security")
                create.call(this, "editor", "Editor");
        };
        try {
            originalInitialLayout.call(this);
        } finally {
            delete this.createDetailNavigationItem;
        }
    };

    let originalShowContentView = WI.NetworkResourceDetailView.prototype.showContentViewForIdentifier;
    WI.NetworkResourceDetailView.prototype.showContentViewForIdentifier = function(identifier) {
        if (identifier !== "editor" || (this._contentViewCookie && "lineNumber" in this._contentViewCookie)) {
            originalShowContentView.call(this, identifier);
            return;
        }
        if (!this._edenEditorContentView)
            this._edenEditorContentView = new WI.EdenRequestEditorContentView(this.representedObject);
        this._contentBrowser.showContentView(this._edenEditorContentView, this._contentViewCookie);
        this._contentViewCookie = null;
    };

    let originalPopulateNameCell = WI.NetworkTableContentView.prototype._populateNameCell;
    WI.NetworkTableContentView.prototype._populateNameCell = function(cell, entry) {
        originalPopulateNameCell.call(this, cell, entry);
        let resource = entry.resource;
        let info = resource ? WI.edenRequests.modifiedInfo(resource) : null;
        if (!info)
            return;
        let name = cell.querySelector("span:not(.eden-table-chip)");
        let chip = element("span", "eden-table-chip " + info.kind, info.kind === "raw" ? "Raw" : "Modified");
        cell.insertBefore(chip, name || null);
        if (name && info.kind === "raw") {
            let parsed = parseURL(info.finalURL);
            if (parsed)
                name.textContent = parsed.pathname.split("/").filter(Boolean).pop() || parsed.host;
            cell.title = info.finalURL;
        }
    };

    let navigationDescriptor = Object.getOwnPropertyDescriptor(WI.NetworkTableContentView.prototype, "navigationItems");
    Object.defineProperty(WI.NetworkTableContentView.prototype, "navigationItems", {
        configurable: true,
        get() {
            let items = navigationDescriptor.get.call(this);
            if (!this._edenItems) {
                let count = new WI.TextNavigationItem("eden-custom-count", "");
                let disable = new WI.CheckboxNavigationItem("eden-custom-disable", "Disable custom requests", WI.edenRequests.disabled);
                disable.addEventListener(WI.CheckboxNavigationItem.Event.CheckedDidChange, () => { WI.edenRequests.disabled = disable.checked; });
                let clear = new WI.ButtonNavigationItem("eden-custom-clear", "Clear custom requests");
                clear.buttonStyle = WI.ButtonNavigationItem.Style.Text;
                clear.addEventListener(WI.ButtonNavigationItem.Event.Clicked, () => WI.edenRequests.clearSite());
                let update = () => {
                    let total = WI.edenRequests.siteDrafts.length;
                    count.text = total === 1 ? "1 modified request" : total + " modified requests";
                    for (let item of [count, disable, clear])
                        item.hidden = !total;
                    let modified = WI.edenRequests.modifiedCount;
                    if (this._table && this._edenModifiedCount !== modified) {
                        this._edenModifiedCount = modified;
                        this._table.reloadData();
                    }
                };
                WI.edenRequests.addEventListener(EdenRequests.Event.Changed, update, this);
                WI.networkManager.addEventListener(WI.NetworkManager.Event.MainFrameDidChange, update, this);
                this._edenItems = [count, disable, clear];
                update();
            }
            return [...this._edenItems, new WI.DividerNavigationItem, ...items];
        },
    });
})();

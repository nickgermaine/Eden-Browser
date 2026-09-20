(function() {
    if (window.__edenHook)
        return;
    var origins = [];
    var requests = [];
    var activeActions = [];
    var lastEvent = null;
    var run = null;
    var nextId = 1;

    function describeElement(node) {
        if (!node || !node.tagName)
            return "document";
        var text = node.tagName.toLowerCase();
        if (node.id)
            return text + "#" + node.id;
        if (node.classList && node.classList.length)
            text += "." + Array.prototype.slice.call(node.classList, 0, 2).join(".");
        return text;
    }

    function preview(value) {
        try {
            var text = JSON.stringify(value);
            if (text === undefined)
                text = String(value);
            return text.length > 60 ? text.slice(0, 57) + "..." : text;
        } catch (error) {
            return String(value);
        }
    }

    function remember(origin) {
        origin.id = nextId++;
        origins.push(origin);
        if (origins.length > 200)
            origins.shift();
        return origin;
    }

    var NativeMutationObserver = window.MutationObserver;
    var currentRoot = null;
    var lastRoot = null;

    function runAsRoot(label, callback, self, args) {
        var previous = currentRoot;
        currentRoot = {kind: "call", label: label, fn: callback, self: self, args: Array.prototype.slice.call(args)};
        lastRoot = {time: Date.now(), origin: currentRoot};
        try {
            return callback.apply(self, args);
        } finally {
            currentRoot = previous;
        }
    }

    function wrapCallback(callback, label) {
        return function() {
            return runAsRoot(label + " " + (callback.name || "callback") + "()", callback, this, arguments);
        };
    }

    function wrapScheduler(name, label) {
        var original = window[name];
        if (typeof original !== "function")
            return;
        window[name] = function(callback) {
            if (typeof callback !== "function")
                return original.apply(this, arguments);
            var rest = Array.prototype.slice.call(arguments, 1);
            return original.apply(this, [wrapCallback(callback, label)].concat(rest));
        };
    }

    wrapScheduler("setTimeout", "timer");
    wrapScheduler("setInterval", "interval");
    wrapScheduler("requestAnimationFrame", "animation frame");
    wrapScheduler("requestIdleCallback", "idle callback");

    ["IntersectionObserver", "MutationObserver", "ResizeObserver"].forEach(function(name) {
        var Original = window[name];
        if (typeof Original !== "function")
            return;
        var label = name.replace("Observer", " observer").toLowerCase();
        var Wrapped = function(callback, options) {
            return new Original(typeof callback === "function" ? wrapCallback(callback, label) : callback, options);
        };
        Wrapped.prototype = Original.prototype;
        Object.getOwnPropertyNames(Original).forEach(function(key) {
            if (!(key in Wrapped)) {
                try {
                    Wrapped[key] = Original[key];
                } catch (error) {}
            }
        });
        window[name] = Wrapped;
    });

    var messageTargets = ["WebSocket", "EventSource", "MessagePort", "Worker", "SharedWorker", "BroadcastChannel"].filter(function(name) { return typeof window[name] === "function"; });
    var windowTypes = {message: true, storage: true, online: true, offline: true, visibilitychange: true, hashchange: true, popstate: true, focus: true};
    var listenerMap = typeof WeakMap !== "undefined" ? new WeakMap : null;

    function targetLabel(target, type) {
        for (var index = 0; index < messageTargets.length; ++index) {
            if (target instanceof window[messageTargets[index]])
                return messageTargets[index].toLowerCase() + " " + type + " listener";
        }
        if ((target === window || target === document) && windowTypes[type])
            return "window " + type + " listener";
        return null;
    }

    if (listenerMap && window.EventTarget) {
        var addListener = EventTarget.prototype.addEventListener;
        var removeListener = EventTarget.prototype.removeEventListener;
        EventTarget.prototype.addEventListener = function(type, listener, options) {
            var label = typeof listener === "function" ? targetLabel(this, type) : null;
            if (!label)
                return addListener.apply(this, arguments);
            var wrapped = listenerMap.get(listener);
            if (!wrapped) {
                wrapped = wrapCallback(listener, label);
                listenerMap.set(listener, wrapped);
            }
            return addListener.call(this, type, wrapped, options);
        };
        EventTarget.prototype.removeEventListener = function(type, listener, options) {
            var wrapped = typeof listener === "function" ? listenerMap.get(listener) : null;
            return removeListener.call(this, type, wrapped || listener, options);
        };
    }

    messageTargets.forEach(function(name) {
        ["onmessage", "onopen", "onerror", "onclose"].forEach(function(property) {
            var descriptor = Object.getOwnPropertyDescriptor(window[name].prototype, property);
            if (!descriptor || !descriptor.set || !descriptor.configurable)
                return;
            Object.defineProperty(window[name].prototype, property, {
                configurable: true,
                enumerable: descriptor.enumerable,
                get: function() {
                    var current = descriptor.get.call(this);
                    return current && current.__edenOriginal ? current.__edenOriginal : current;
                },
                set: function(handler) {
                    if (typeof handler !== "function")
                        return descriptor.set.call(this, handler);
                    var wrapped = wrapCallback(handler, name.toLowerCase() + " " + property.slice(2) + " handler");
                    wrapped.__edenOriginal = handler;
                    return descriptor.set.call(this, wrapped);
                },
            });
        });
    });

    var loadOrigin = null;

    function currentOrigin() {
        if (activeActions.length)
            return activeActions[activeActions.length - 1].origin;
        if (currentRoot) {
            if (!currentRoot.id)
                remember(currentRoot);
            return currentRoot;
        }
        if (lastEvent && Date.now() - lastEvent.time < 1500)
            return lastEvent.origin;
        if (lastRoot && Date.now() - lastRoot.time < 1500) {
            if (!lastRoot.origin.id)
                remember(lastRoot.origin);
            return lastRoot.origin;
        }
        if (document.readyState !== "complete") {
            if (!loadOrigin)
                loadOrigin = remember({kind: "load", label: "page load"});
            return loadOrigin;
        }
        return null;
    }


    function note(method, url) {
        var absolute = url;
        try {
            absolute = new URL(url, location.href).href;
        } catch (error) {}
        var origin = currentOrigin();
        requests.push({method: String(method || "GET").toUpperCase(), url: absolute, time: Date.now(), origin: origin ? origin.id : 0});
        if (requests.length > 300)
            requests.shift();
    }

    var originalFetch = window.fetch;
    if (originalFetch) {
        window.fetch = function(input, init) {
            try {
                var isRequest = typeof Request !== "undefined" && input instanceof Request;
                note((init && init.method) || (isRequest ? input.method : "GET"), isRequest ? input.url : String(input));
            } catch (error) {}
            return originalFetch.apply(this, arguments);
        };
    }

    var originalOpen = XMLHttpRequest.prototype.open;
    XMLHttpRequest.prototype.open = function(method, url) {
        try {
            note(method, String(url));
        } catch (error) {}
        return originalOpen.apply(this, arguments);
    };

    ["click", "submit", "change", "keydown", "input", "pointerup"].forEach(function(type) {
        window.addEventListener(type, function(event) {
            if (!event.isTrusted && !(run && run.dispatching))
                return;
            if (type === "keydown" && event.key !== "Enter")
                return;
            var target = event.target;
            lastEvent = {time: Date.now(), origin: remember({kind: "event", type: type, label: type + " on " + describeElement(target), target: typeof WeakRef !== "undefined" ? new WeakRef(target) : {deref: function() { return target; }}})};
        }, true);
    });

    function watchStore(store) {
        if (!store || store.__edenWatched || !store.$onAction)
            return;
        Object.defineProperty(store, "__edenWatched", {value: true});
        store.$onAction(function(context) {
            var origin = remember({kind: "action", store: context.store.$id, name: context.name, args: context.args, label: context.store.$id + "." + context.name + "(" + Array.prototype.map.call(context.args, preview).join(", ") + ")"});
            var entry = {origin: origin};
            activeActions.push(entry);
            var finish = function(outcome, detail) {
                var index = activeActions.indexOf(entry);
                if (index >= 0)
                    activeActions.splice(index, 1);
                if (run)
                    run.effects.push({kind: outcome, text: context.name + (outcome === "resolved" ? " resolved" : " rejected: " + detail)});
            };
            context.after(function() { finish("resolved"); });
            context.onError(function(error) { finish("rejected", error && error.message ? error.message : String(error)); });
        });
    }

    function describeValue(value) {
        if (Array.isArray(value))
            return value.length + (value.length === 1 ? " item" : " items");
        if (value && typeof value === "object")
            return Object.keys(value).length + " keys";
        return preview(value);
    }

    function snapshotStores() {
        var snapshot = {};
        findPinia().forEach(function(pinia) {
            if (!pinia._s)
                return;
            pinia._s.forEach(function(store, id) {
                var state = store.$state || {};
                var entry = {};
                Object.keys(state).forEach(function(key) {
                    entry[key] = {text: preview(state[key]), label: describeValue(state[key])};
                });
                snapshot[id] = entry;
            });
        });
        return snapshot;
    }

    function storeChanges(before, after) {
        var changes = [];
        Object.keys(after).forEach(function(id) {
            Object.keys(after[id]).forEach(function(key) {
                var previous = before[id] && before[id][key];
                if (!previous || previous.text !== after[id][key].text)
                    changes.push({kind: "store", text: id + "." + key + ": " + (previous ? previous.label : "unset") + " \u2192 " + after[id][key].label});
            });
        });
        return changes;
    }

    function findPinia() {
        var found = [];
        var nodes = document.querySelectorAll("body, body *");
        for (var index = 0; index < nodes.length && index < 3000; ++index) {
            var app = nodes[index].__vue_app__;
            var pinia = app && app.config && app.config.globalProperties && app.config.globalProperties.$pinia;
            if (pinia && found.indexOf(pinia) < 0)
                found.push(pinia);
        }
        return found;
    }

    function scan() {
        findPinia().forEach(function(pinia) {
            if (!pinia._s)
                return;
            pinia._s.forEach(watchStore);
            if (!pinia.__edenWatched) {
                Object.defineProperty(pinia, "__edenWatched", {value: true});
                var set = pinia._s.set;
                pinia._s.set = function(key, store) {
                    var result = set.call(this, key, store);
                    watchStore(store);
                    return result;
                };
            }
        });
    }

    var scans = 0;
    var timer = setInterval(function() {
        scan();
        if (++scans > 40)
            clearInterval(timer);
    }, 250);
    document.addEventListener("DOMContentLoaded", scan);

    ["log", "info", "warn", "error"].forEach(function(level) {
        var original = console[level];
        console[level] = function() {
            if (run)
                run.effects.push({kind: level === "error" || level === "warn" ? level : "log", text: "console." + level + ": " + Array.prototype.map.call(arguments, function(item) { return typeof item === "string" ? item : preview(item); }).join(" ")});
            return original.apply(this, arguments);
        };
    });

    window.__edenHook = {
        originFor: function(method, url, time) {
            scan();
            var best = null;
            for (var index = requests.length - 1; index >= 0; --index) {
                var item = requests[index];
                if (item.method !== method || item.url.split("#")[0] !== url.split("#")[0])
                    continue;
                if (!best || Math.abs(item.time - time) < Math.abs(best.time - time))
                    best = item;
            }
            if (!best || !best.origin)
                return null;
            var origin = origins.filter(function(item) { return item.id === best.origin; })[0];
            if (!origin)
                return null;
            var available = true;
            if (origin.kind === "event") {
                var node = origin.target.deref();
                available = !!node && node.isConnected;
            }
            if (origin.kind === "call" && origin.self && origin.self.readyState !== undefined && typeof WebSocket !== "undefined" && origin.self instanceof WebSocket)
                available = origin.self.readyState === 1;
            return {id: origin.id, kind: origin.kind, label: origin.label, available: available};
        },
        rerun: function(id) {
            var origin = origins.filter(function(item) { return item.id === id; })[0];
            if (!origin)
                return Promise.resolve({error: "The origin is no longer available"});
            var href = location.href;
            var before = snapshotStores();
            var domChanges = 0;
            var domObserver = null;
            if (NativeMutationObserver && document.documentElement) {
                domObserver = new NativeMutationObserver(function(records) { domChanges += records.length; });
                domObserver.observe(document.documentElement, {subtree: true, childList: true, characterData: true, attributes: true});
            }
            run = {originId: id, effects: [], dispatching: false};
            var current = run;
            var finished;
            try {
                if (origin.kind === "action") {
                    var store = null;
                    findPinia().forEach(function(pinia) {
                        if (pinia._s && pinia._s.has(origin.store))
                            store = pinia._s.get(origin.store);
                    });
                    if (!store)
                        throw new Error("Store " + origin.store + " is gone");
                    finished = Promise.resolve(store[origin.name].apply(store, origin.args)).catch(function() {});
                } else if (origin.kind === "call") {
                    finished = Promise.resolve(origin.fn.apply(origin.self || window, origin.args)).catch(function() {}).then(function() {
                        return new Promise(function(resolve) { setTimeout(resolve, 800); });
                    });
                } else {
                    var node = origin.target.deref();
                    if (!node || !node.isConnected)
                        throw new Error("The element is no longer on the page");
                    current.dispatching = true;
                    if (origin.type === "click" && node.click)
                        node.click();
                    else if (origin.type === "submit" && node.requestSubmit)
                        node.requestSubmit();
                    else
                        node.dispatchEvent(new Event(origin.type, {bubbles: true, cancelable: true}));
                    current.dispatching = false;
                    finished = new Promise(function(resolve) { setTimeout(resolve, 1200); });
                }
            } catch (error) {
                run = null;
                if (domObserver)
                    domObserver.disconnect();
                return Promise.resolve({error: error.message});
            }
            return finished.then(function() {
                return new Promise(function(resolve) { setTimeout(resolve, 250); });
            }).then(function() {
                if (domObserver) {
                    domChanges += domObserver.takeRecords().length;
                    domObserver.disconnect();
                }
                current.effects = current.effects.concat(storeChanges(before, snapshotStores()));
                if (domChanges)
                    current.effects.push({kind: "dom", text: "Page updated: " + domChanges + (domChanges === 1 ? " DOM change" : " DOM changes")});
                if (location.href !== href)
                    current.effects.push({kind: "route", text: "Route changed to " + location.pathname + location.search});
                if (run === current)
                    run = null;
                return {effects: current.effects};
            });
        },
    };
    scan();
})();

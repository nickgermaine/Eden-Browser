(() => {
    function assert(condition) {
        if (!condition) {
            throw new Error('Object copy invariant failed');
        }
    }
    const assign = (target, a, b) => Object.assign(target, a, b);
    const rest = source => { const {...copy} = source; return copy; };
    for (let i = 0; i < 2000; ++i) {
        const events = [];
        const source = {a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8};
        Object.defineProperty(source, 'later', {enumerable: true, get() {
            events.push('get');
            return i;
        }});
        const target = {set a(value) { events.push('set:' + value); }};
        const result = assign(target, source, {later: -i});
        assert(result === target && result.later === -i && events.join(',') === 'set:1,get');
        const copied = rest(source);
        assert(copied.later === i && Object.getOwnPropertyDescriptor(copied, 'later').writable);
        const protoSource = Object.defineProperty({a: 1, b: 2, c: 3, d: 4}, '__proto__', {enumerable: true, value: {tag: i}});
        const assigned = assign({}, protoSource, {});
        const spread = rest(protoSource);
        assert(Object.getPrototypeOf(assigned).tag === i);
        assert(Object.getPrototypeOf(spread) === Object.prototype && Object.hasOwn(spread, '__proto__'));
        const proxyEvents = [];
        const proxy = new Proxy({a: 1, b: 2}, {
            ownKeys(object) { proxyEvents.push('keys'); return Reflect.ownKeys(object); },
            getOwnPropertyDescriptor(object, key) { proxyEvents.push('desc:' + key); return Reflect.getOwnPropertyDescriptor(object, key); },
            get(object, key) { proxyEvents.push('get:' + key); return Reflect.get(object, key); },
        });
        assert(assign({}, proxy, {c: 3}).c === 3);
        assert(proxyEvents.join(',') === 'keys,desc:a,get:a,desc:b,get:b');
        const crossRealmTarget = new otherTestRealm.Object();
        assert(assign(crossRealmTarget, {a: 1, b: 2, c: 3, d: 4}, {e: 5}) === crossRealmTarget);
        assert(Object.getPrototypeOf(crossRealmTarget) === otherTestRealm.Object.prototype);
        const frozen = Object.freeze({a: 0});
        let threw = false;
        try { assign(frozen, {a: 1}, {}); } catch (error) { threw = error instanceof TypeError; }
        assert(threw && frozen.a === 0);
        let tailRead = false;
        const failing = {a: 1, get b() { throw new Error('expected'); }};
        const tail = {get c() { tailRead = true; return 3; }};
        const partial = {};
        try { assign(partial, failing, tail); } catch (error) { assert(error.message === 'expected'); }
        assert(partial.a === 1 && !tailRead);
        const hidden = Object.defineProperty({a: 1, b: 2, c: 3, d: 4}, 'b', {enumerable: false});
        const resultHidden = assign({}, hidden, {z: 1});
        assert(!Object.hasOwn(resultHidden, 'b') && resultHidden.z === 1);
        const values = [undefined, null, 1, true, 'ab', ['a', 'b']];
        for (const value of values) {
            assert(assign({}, value, {end: i}).end === i);
        }
    }
    return 'copy boundaries passed';
})();

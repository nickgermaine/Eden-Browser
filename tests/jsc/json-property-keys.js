(() => {
    function assert(condition, message) {
        if (!condition) {
            throw new Error(message);
        }
    }

    function assertSyntaxError(source) {
        let threw = false;
        try {
            JSON.parse(source);
        } catch (error) {
            threw = error instanceof SyntaxError;
        }
        assert(threw, "Invalid JSON was accepted: " + JSON.stringify(source));
    }

    function verify(source, key, expected) {
        const value = JSON.parse(source);
        assert(Object.hasOwn(value, key), "Missing key: " + JSON.stringify(key));
        assert(Object.is(value[key], expected), "Changed value: " + JSON.stringify(key));
        const descriptor = Object.getOwnPropertyDescriptor(value, key);
        assert(descriptor.writable && descriptor.enumerable && descriptor.configurable, "Changed descriptor");
        assert(Object.getPrototypeOf(value) === Object.prototype, "Changed prototype");
    }

    const prefixes = ['{"first":1,', '{"first":"\u0100",'];
    const keys = ["", "a", "title", "completed", "__proto__", "constructor", "0", "4294967295",
        "\u00ff", "\u0100", "\u0122", "\u015c", "\u200c", "\u2028", "\ud800", "\udfff", "\ud83d\ude00",
        "quote\"key", "slash\\key", "tab\tkey", "line\nkey", "null\u0000key"];
    for (const length of [7, 8, 9, 15, 16, 17, 31, 32, 33, 255, 4096]) {
        keys.push("a".repeat(length));
        keys.push("a".repeat(length) + "\u0100");
        keys.push("a".repeat(length) + "\"tail");
    }

    for (let iteration = 0; iteration < 20; ++iteration) {
        for (const prefix of prefixes) {
            for (const key of keys) {
                const source = prefix + JSON.stringify(key) + ":-0}";
                verify(source, key, -0);
                verify(prefix + " \r\n\t" + JSON.stringify(key) + ":42}", key, 42);
            }
            verify(prefix + '"dupe":1,"dupe":2}', "dupe", 2);
            verify(prefix + '"escaped":1,"\\u0065scaped":2}', "escaped", 2);
            verify(prefix + '"__proto__":null}', "__proto__", null);
        }
        const nested = JSON.parse('{"wide":"\u0100","child":{"first":0,"second":[{"a":1,"b":2}]},"tail":3}');
        assert(nested.child.second[0].b === 2 && nested.tail === 3, "Nested parser state changed");
        const ordered = JSON.parse('{"wide":"\u0100","b":1,"2":2,"1":1,"a":3}');
        assert(Object.keys(ordered).join(",") === "1,2,wide,b,a", "Property order changed");
    }

    for (const prefix of prefixes) {
        for (const suffix of ['}', '"', '"unterminated', '"key"', '"key":', '"key":1,}',
            '"key":1 "tail":2}', "'key':1}", 'key:1}', '"\\x41":1}', '"\\u0":1}',
            '"\\u00gg":1}', '"bad\nkey":1}', '"bad\u0000key":1}', '\u00a0"key":1}']) {
            assertSyntaxError(prefix + suffix);
        }
        for (let control = 0; control < 32; ++control) {
            assertSyntaxError(prefix + '"a' + String.fromCharCode(control) + 'b":1}');
        }
    }

    const visits = [];
    const revived = JSON.parse('{"wide":"\u0100","number":123,"drop":0}', function (key, value, context) {
        visits.push(key);
        if (key === "number" && context) {
            assert(context.source === "123", "Reviver source changed");
        }
        return key === "drop" ? undefined : value;
    });
    assert(revived.number === 123 && !Object.hasOwn(revived, "drop"), "Reviver result changed");
    assert(visits.join(",") === "wide,number,drop,", "Reviver order changed");

    const depth = 5000;
    const deep = JSON.parse('{"wide":"\u0100","next":'.repeat(depth) + '0' + '}'.repeat(depth));
    let cursor = deep;
    for (let index = 0; index < depth; ++index) {
        assert(cursor.wide === "\u0100", "Stack fallback changed a key");
        cursor = cursor.next;
    }
    assert(cursor === 0, "Stack fallback changed the leaf");

    if (typeof print === "function") {
        print("JSON property key boundaries passed");
    }
    return "JSON property key boundaries passed";
})();

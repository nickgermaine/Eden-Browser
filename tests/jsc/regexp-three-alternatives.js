(() => {
    let hash = 2166136261;
    let count = 0;
    function record(value) {
        const text = JSON.stringify(value);
        for (let i = 0; i < text.length; ++i) {
            hash = Math.imul(hash ^ text.charCodeAt(i), 16777619);
        }
        ++count;
    }
    function check(pattern, flags, input, lastIndex) {
        const expression = new RegExp(pattern, flags);
        expression.lastIndex = lastIndex;
        for (let i = 0; i < 3; ++i) {
            const match = expression.exec(input);
            record([match && Array.from(match), match && match.index, expression.lastIndex]);
        }
    }
    const groups = [
        ['<script', '<style', '<link'],
        ['abcd', 'abce', 'abcf'],
        ['aaaabbbb', 'aaab', 'zzzzzzzzzz'],
        ['aaaa', 'aaaabbbb', 'cccc'],
        ['[ac]bcd', '[ac]bce', 'xyzw'],
        ['[a-z]bcd', 'abce', 'abcf'],
        ['ab[^x]d', 'abce', 'abcf'],
        ['abcd+', 'abce', 'abcf'],
        ['ab(cd)', 'ab(ce)', 'ab(cf)'],
        ['abcd$', 'abce$', 'abcf$'],
        ['^abcd', '^abce', '^abcf'],
        ['abcd(?=x)', 'abce(?=y)', 'abcf(?=z)'],
        ['abcé', 'abcè', 'abcê'],
        ['éabc', 'èabc', 'êabc'],
        ['abcd', 'xyzq', '0123'],
        ['a', 'abce', 'abcf'],
    ];
    const flagsList = ['', 'i', 'g', 'gi', 'y', 'iy', 'u', 'iu', 'd', 's', 'm'];
    const tokens = ['abcd', 'abce', 'abcf', 'abcg', 'aaaabbbb', 'aaab', 'cccc', 'xyzw', '<script', '<SCRIPT', '<style', '<STYLE', '<link', '<LINK', '<sxxx', 'abcdx', 'abcey', 'abcfz', 'abcé', 'éabc', 'zzzzzzzzzz'];
    for (const group of groups) {
        for (const order of [[0, 1, 2], [0, 2, 1], [1, 0, 2], [1, 2, 0], [2, 0, 1], [2, 1, 0]]) {
            const pattern = order.map(index => group[index]).join('|');
            for (const flags of flagsList) {
                for (let offset = 0; offset < 65; ++offset) {
                    const token = tokens[offset % tokens.length];
                    const padding = 'x'.repeat(offset);
                    check(pattern, flags, padding + token, 0);
                    check(pattern, flags, padding + token + 'x'.repeat(31) + tokens[(offset + 1) % tokens.length], offset % 5);
                    check(pattern, flags, padding + token + '\u0100', offset);
                    check(pattern, flags, padding + token.slice(0, offset % (token.length + 1)), 0);
                }
            }
        }
    }
    let seed = 17092026;
    function random(limit) {
        seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
        return seed % limit;
    }
    const alphabet = 'abcdef<>xyz1234ABCDEFGHIJKLMNOPQRSTUVWXYZ';
    for (let i = 0; i < 16000; ++i) {
        const prefixes = [];
        for (let j = 0; j < 3; ++j) {
            let prefix = i % 2 ? 'ab' : '';
            const length = 4 + random(8);
            while (prefix.length < length) {
                prefix += alphabet[random(alphabet.length)];
            }
            prefixes.push(prefix);
        }
        let input = '';
        const length = random(128);
        for (let j = 0; j < length; ++j) {
            input += alphabet[random(alphabet.length)];
        }
        const offset = random(input.length + 1);
        if (i % 4) {
            const match = prefixes[random(3)];
            input = input.slice(0, offset) + (i % 2 ? match.toLowerCase() : match) + input.slice(offset);
        }
        check(prefixes.join('|'), flagsList[random(flagsList.length)], input, i % 3 ? 0 : offset);
    }
    return JSON.stringify({checks: count, hash: hash >>> 0});
})();

(() => {
    const times = {};
    let checksum = 0;
    function measure(name, operation, count) {
        operation(Math.floor(count / 4));
        const values = [];
        for (let sample = 0; sample < 7; ++sample) {
            const start = Date.now();
            checksum += operation(count);
            values.push(Date.now() - start);
        }
        times[name] = values;
    }
    measure('assign', count => {
        let state = {a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8};
        for (let i = 0; i < count; ++i) {
            state = Object.assign({}, state, {b: i, h: i + 1});
        }
        return state.h;
    }, 1000000);
    measure('rest', count => {
        let state = {a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8};
        for (let i = 0; i < count; ++i) {
            const {...next} = state;
            next.h = i;
            state = next;
        }
        return state.h;
    }, 1000000);
    const expressions = [/<script|<style|<link/i, /alpha|alpine|bravo/, /abcd|abce|abcf/];
    const tokens = ['<script', 'alpine', 'abcf'];
    for (let group = 0; group < expressions.length; ++group) {
        const expression = expressions[group];
        for (const length of [32, 256, 4096]) {
            const inputs = Array.from({length: 32}, (_, i) => '0123 <>:xyz. '.repeat(Math.ceil(length / 12)).slice(0, length - i % 12) + (i % 4 ? '' : tokens[group]));
            measure('regexp-' + group + '-' + length, count => {
                let matches = 0;
                for (let i = 0; i < count; ++i) {
                    matches += expression.test(inputs[i & 31]);
                }
                return matches;
            }, 250000);
        }
    }
    return JSON.stringify({times, checksum});
})();

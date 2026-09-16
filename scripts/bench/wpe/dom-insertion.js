async function edenDOMProbe() {
    const nextFrame = () => new Promise(resolve => requestAnimationFrame(resolve));
    document.head.innerHTML =
        '<style>body{margin:0;font:16px sans-serif}.row{display:flex;height:24px}.row label{width:200px}.row input{margin:2px}.row button{width:24px}</style>';
    document.body.innerHTML = '<main></main>';
    const mount = document.querySelector('main');
    const measurements = {};
    let checksum = 0;
    function markup(count) {
        return Array
            .from(
                {length : count},
                (_, i) => '<div class="row"><input type="checkbox"><label>Item ' + i +
                          '</label><button>x</button><input value="' + i + '"></div>'
            )
            .join('');
    }
    function row(i) {
        const container = document.createElement('div');
        container.className = 'row';
        const checkbox = document.createElement('input');
        checkbox.type = 'checkbox';
        const label = document.createElement('label');
        label.textContent = 'Item ' + i;
        const button = document.createElement('button');
        button.textContent = 'x';
        const input = document.createElement('input');
        input.value = i;
        container.append(checkbox, label, button, input);
        return container;
    }
    async function measure(name, prepare, mutate, count) {
        const sync = [];
        const layout = [];
        for (let iteration = 0; iteration < 11; ++iteration) {
            mount.replaceChildren();
            const context = prepare();
            await nextFrame();
            const start = performance.now();
            const target = mutate(context, iteration);
            const end = performance.now();
            checksum += document.body.offsetHeight;
            const layoutEnd = performance.now();
            if (target.children.length !== count) {
                throw new Error(name + ' produced ' + target.children.length + ' children');
            }
            checksum += target.querySelectorAll('input').length;
            if (iteration) {
                sync.push(end - start);
                layout.push(layoutEnd - end);
            }
            await nextFrame();
        }
        measurements[name] = {sync, layout};
    }
    for (const count of [100, 1000]) {
        const source = markup(count);
        const template = document.createElement('div');
        template.innerHTML = source;
        await measure('parse-detached-' + count, () => document.createElement('div'), target => {
            target.innerHTML = source;
            return target;
        }, count);
        await measure('create-detached-' + count, () => document.createElement('div'), target => {
            for (let i = 0; i < count; ++i) {
                target.appendChild(row(i));
            }
            return target;
        }, count);
        await measure('clone-detached-' + count, () => template, source => source.cloneNode(true), count);
        await measure('connect-subtree-' + count, () => template.cloneNode(true), subtree => {
            mount.appendChild(subtree);
            return subtree;
        }, count);
        await measure('connect-individual-' + count, () => Array.from({length: count}, (_, i) => row(i)), rows => {
            for (const child of rows) {
                mount.appendChild(child);
            }
            return mount;
        }, count);
        await measure('connect-fragment-' + count, () => {
            const fragment = document.createDocumentFragment();
            for (let i = 0; i < count; ++i) {
                fragment.appendChild(row(i));
            }
            return fragment;
        }, fragment => {
            mount.appendChild(fragment);
            return mount;
        }, count);
        await measure('replace-connected-' + count, () => {
            mount.innerHTML = source;
            return mount;
        }, target => {
            target.innerHTML = source;
            return target;
        }, count);
        await measure('replace-and-change-' + count, () => {
            mount.innerHTML = source;
            return mount;
        }, (target, iteration) => {
            target.innerHTML = source.replace('Item 0', 'Changed ' + iteration);
            return target;
        }, count);
    }
    const growingSources = Array.from({length : 100}, (_, i) => markup(i + 1));
    await measure('replace-growing-100', () => mount, target => {
        for (const source of growingSources) {
            target.innerHTML = source;
        }
        return target;
    }, 100);
    mount.replaceChildren();
    return {
        probe : 'dom-insertion',
        measurements,
        checksum,
        userAgent : navigator.userAgent,
        crossOriginIsolated,
        location : location.href,
        viewport : [ innerWidth, innerHeight, devicePixelRatio ],
        visibility : document.visibilityState
    };
}

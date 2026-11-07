async function edenDOMProbe() {
    const stylesheet = document.createElement('style');
    stylesheet.textContent = `
        html, body { margin: 0; padding: 0; }
        #mount { position: absolute; left: 0; top: 0; width: 1200px; }
        .scenario { position: relative; margin: 0; padding: 0; border: 0; font-synthesis: none; }
        .target { box-sizing: border-box; margin: 0; padding: 0; border: 0; line-height: 1.25; }
        .peer { box-sizing: border-box; flex: 0 0 73px; width: 73px; min-width: 73px; height: 19px; }
        .mark { background: rgb(217, 239, 255); }
        .first-line::first-line { font-size: 19px; font-weight: 700; letter-spacing: 0.35px; }
    `;
    document.head.replaceChildren(stylesheet);
    document.body.replaceChildren();
    const mount = document.createElement('main');
    mount.id = 'mount';
    mount.lang = 'en';
    document.body.appendChild(mount);

    const loadedFonts = [];
    for (const entry of globalThis.edenFixtureFonts || []) {
        const font = new FontFace(entry.family, 'url(data:font/ttf;base64,' + entry.data + ')', entry.descriptors);
        await font.load();
        document.fonts.add(font);
        loadedFonts.push(entry.family);
    }
    await document.fonts.ready;
    for (const family of loadedFonts) {
        if (!document.fonts.check('17px ' + family)) {
            throw new Error('Fixture font did not become available: ' + family);
        }
    }

    const geometry = [];
    const layoutNames = ['direct', 'flex', 'grid'];
    const whitespaceModes = ['normal', 'pre-line', 'pre-wrap', 'break-spaces', 'pre', 'nowrap'];
    const contents = [
        {name: 'edge-spaces', text: '  alpha   beta   gamma  '},
        {name: 'newlines-tabs', text: 'alpha beta\n\t gamma\n delta'},
        {name: 'unicode-breakers', text: 'co\u00adoperate a\u200bb a\u00a0b A\u0301b 😀x אבג'}
    ];

    function appendText(target, spec) {
        const wrapper = document.createElement('span');
        const text = spec.text;
        if (spec.fullMark) {
            const mark = document.createElement('mark');
            mark.className = 'mark';
            mark.textContent = text;
            wrapper.appendChild(mark);
        } else if (spec.breakPosition) {
            if (spec.breakPosition === 'start') {
                wrapper.appendChild(document.createElement('br'));
                wrapper.appendChild(document.createTextNode(text));
            } else if (spec.breakPosition === 'end') {
                wrapper.appendChild(document.createTextNode(text));
                wrapper.appendChild(document.createElement('br'));
            } else {
                const middle = Math.floor(text.length / 2);
                wrapper.appendChild(document.createTextNode(text.slice(0, middle)));
                wrapper.appendChild(document.createElement('br'));
                wrapper.appendChild(document.createTextNode(text.slice(middle)));
            }
        } else if (spec.mark || spec.multipleNodes) {
            const firstEnd = Math.max(1, Math.floor(text.length / 3));
            const secondEnd = Math.max(firstEnd + 1, Math.floor(text.length * 2 / 3));
            wrapper.appendChild(document.createTextNode(text.slice(0, firstEnd)));
            if (spec.mark) {
                const mark = document.createElement('mark');
                mark.className = 'mark';
                mark.style.fontWeight = '700';
                mark.textContent = text.slice(firstEnd, secondEnd);
                wrapper.appendChild(mark);
            } else {
                wrapper.appendChild(document.createTextNode(text.slice(firstEnd, secondEnd)));
            }
            wrapper.appendChild(document.createTextNode(text.slice(secondEnd)));
        } else {
            wrapper.textContent = text;
        }
        target.appendChild(wrapper);
    }

    function makeScenario(spec) {
        const host = document.createElement('section');
        host.className = 'scenario';
        host.dataset.case = spec.name;
        host.style.width = (spec.width || 216) + 'px';
        host.style.fontFamily = spec.family || 'FixtureVariable, sans-serif';
        host.style.fontSize = (spec.fontSize || 17.25) + 'px';
        host.style.lineHeight = '1.25';

        const target = document.createElement('div');
        target.className = spec.firstLine ? 'target first-line' : 'target';
        target.dataset.target = spec.name;
        target.style.whiteSpace = spec.whiteSpace || 'normal';
        target.style.cssText += spec.style || '';
        target.style.fontFamily = spec.family || 'FixtureVariable, sans-serif';
        target.style.fontSize = (spec.fontSize || 17.25) + 'px';
        target.style.lineHeight = '1.25';
        if (spec.firstLine) {
            target.classList.add('first-line');
        }
        appendText(target, spec);

        const peer = document.createElement('div');
        peer.className = 'peer';

        if (spec.layout === 'flex') {
            host.style.display = 'flex';
            target.style.flex = '1 1 0px';
            target.style.minWidth = 'min-content';
            target.style.maxWidth = (spec.maxWidth || 181) + 'px';
            host.append(target, peer);
        } else if (spec.layout === 'grid') {
            host.style.display = 'grid';
            host.style.gridTemplateColumns = 'minmax(min-content, max-content) minmax(0, 1fr)';
            host.style.columnGap = '7px';
            target.style.minWidth = 'min-content';
            target.style.maxWidth = (spec.maxWidth || 181) + 'px';
            host.append(target, peer);
        } else {
            target.style.width = 'min-content';
            target.style.maxWidth = (spec.maxWidth || 181) + 'px';
            host.appendChild(target);
        }
        return {host, target, peer};
    }

    function rect(value) {
        const box = value.getBoundingClientRect();
        return [box.x, box.y, box.width, box.height];
    }

    function textRanges(target) {
        const walker = document.createTreeWalker(target, NodeFilter.SHOW_TEXT);
        const range = document.createRange();
        const values = [];
        while (walker.nextNode()) {
            const node = walker.currentNode;
            range.selectNodeContents(node);
            values.push({
                text: node.data,
                rects: Array.from(range.getClientRects(), box => [box.x, box.y, box.width, box.height])
            });
        }
        return values;
    }

    function capture(spec, nodes) {
        const targetStyle = getComputedStyle(nodes.target);
        return {
            name: spec.name,
            layout: spec.layout,
            whiteSpace: targetStyle.whiteSpace,
            font: targetStyle.font,
            host: rect(nodes.host),
            target: rect(nodes.target),
            peer: nodes.peer.isConnected ? rect(nodes.peer) : null,
            scroll: [nodes.target.scrollWidth, nodes.target.scrollHeight],
            hardBreaks: Array.from(nodes.target.querySelectorAll('br'), element => rect(element)),
            textRanges: textRanges(nodes.target)
        };
    }

    function record(spec) {
        const nodes = makeScenario(spec);
        mount.appendChild(nodes.host);
        geometry.push(capture(spec, nodes));
        nodes.host.remove();
    }

    record({
        name: 'simple-min-content-hit',
        text: 'alpha beta',
        whiteSpace: 'normal',
        layout: 'direct'
    });

    for (const whiteSpace of whitespaceModes) {
        for (const content of contents) {
            for (const layout of layoutNames) {
                record({
                    name: `${whiteSpace}-${content.name}-${layout}`,
                    text: content.text,
                    whiteSpace,
                    layout,
                    mark: content.name === 'unicode-breakers',
                    multipleNodes: content.name === 'newlines-tabs'
                });
            }
        }
    }

    const edgeCases = [
        {name: 'spacing-positive-letter', text: 'AVATAR office affinity beta', style: 'letter-spacing:1.3px;'},
        {name: 'spacing-negative-letter', text: 'AVATAR office affinity beta', style: 'letter-spacing:-0.35px;'},
        {name: 'spacing-positive-word', text: 'one two three four five', style: 'word-spacing:2.25px;'},
        {name: 'spacing-negative-word', text: 'one two three four five', style: 'word-spacing:-0.5px;'},
        {
            name: 'indent-right-align',
            text: '  indent alpha beta gamma delta',
            style: 'text-indent:17px;text-align:right;'
        },
        {
            name: 'indent-center-align',
            text: 'alpha beta gamma delta epsilon',
            style: 'text-indent:11px;text-align:center;'
        },
        {
            name: 'overflow-wrap-anywhere',
            text: 'supercalifragilisticexpialidocious repeatedword',
            style: 'overflow-wrap:anywhere;'
        },
        {
            name: 'word-break-all',
            text: 'supercalifragilisticexpialidocious repeatedword',
            style: 'word-break:break-all;'
        },
        {name: 'word-break-keep-all', text: 'alpha 中文 日本語 beta gamma', style: 'word-break:keep-all;'},
        {name: 'hyphens-auto', text: 'internationalization characterization representation', style: 'hyphens:auto;'},
        {name: 'nested-mark', text: 'alpha marked text across several words', mark: true},
        {
            name: 'multiple-text-nodes',
            text: 'one   two\nthree\tfour five',
            multipleNodes: true,
            whiteSpace: 'pre-wrap'
        },
        {name: 'first-line-style', text: 'first line changes the shaping and width of these words', firstLine: true},
        {
            name: 'first-line-preserved-spaces',
            text: '  first  line\nsecond   line',
            firstLine: true,
            whiteSpace: 'pre-wrap'
        },
        {name: 'soft-hyphen-at-end', text: 'alpha beta con\u00ad', whiteSpace: 'pre-wrap'},
        {name: 'zwsp-breakpoint', text: 'alpha\u200bbeta gamma delta', whiteSpace: 'pre-wrap'},
        {name: 'nbsp-run', text: 'alpha\u00a0beta gamma delta', whiteSpace: 'normal'},
        {name: 'mixed-script-bidi', text: 'Latin אבג (عربية) tail', whiteSpace: 'pre-wrap'}
    ];
    for (let index = 0; index < edgeCases.length; ++index) {
        const spec = edgeCases[index];
        record({
            ...spec,
            name: spec.name + '-' + layoutNames[index % layoutNames.length],
            layout: layoutNames[index % layoutNames.length]
        });
    }

    const extendedCases = [
        {
            name: 'pre-line-leading-consecutive-breaks',
            text: '\nalpha\n\nbeta\n',
            whiteSpace: 'pre-line',
            layout: 'direct'
        },
        {
            name: 'pre-wrap-leading-consecutive-breaks',
            text: '\nalpha\n\nbeta\n',
            whiteSpace: 'pre-wrap',
            layout: 'direct'
        },
        {name: 'br-start', text: 'alpha beta', breakPosition: 'start', layout: 'flex'},
        {name: 'br-end', text: 'alpha beta', breakPosition: 'end', layout: 'grid'},
        {name: 'br-middle', text: 'alpha beta', breakPosition: 'middle', layout: 'direct'},
        {name: 'single-character-min-content', text: 'x', layout: 'direct'},
        {name: 'only-whitespace-collapse', text: '   ', layout: 'direct'},
        {
            name: 'only-whitespace-preserved',
            text: '\t  ',
            whiteSpace: 'pre-wrap',
            layout: 'flex'
        },
        {name: 'single-full-mark', text: 'single nested marked content', fullMark: true, layout: 'grid'},
        {
            name: 'overflow-wrap-break-word',
            text: 'ordinary words stay together when they fit',
            style: 'overflow-wrap:break-word;',
            layout: 'direct'
        }
    ];
    for (const spec of extendedCases) {
        record({...spec, name: spec.name + '-' + spec.layout});
    }

    const mutation = makeScenario({
        name: 'mutation-initial',
        text: 'alpha   beta gamma delta epsilon',
        whiteSpace: 'pre-wrap',
        layout: 'flex'
    });
    mount.appendChild(mutation.host);
    geometry.push(capture({name: 'mutation-initial', layout: 'flex'}, mutation));
    mutation.target.replaceChildren(document.createTextNode('changed  content\nwith a hard break and more words'));
    geometry.push(capture({name: 'mutation-text', layout: 'flex'}, mutation));
    mutation.host.style.width = '287px';
    mutation.target.style.fontFamily = 'FixtureRegularOnly, serif';
    mutation.target.style.fontSize = '19.5px';
    geometry.push(capture({name: 'mutation-width-font', layout: 'flex'}, mutation));
    mutation.host.style.width = '193px';
    mutation.target.style.fontFamily = 'FixtureVariable, sans-serif';
    mutation.target.style.fontSize = '16px';
    geometry.push(capture({name: 'mutation-second-width-font', layout: 'flex'}, mutation));
    mutation.host.remove();

    let randomState = 0x5a17c9e3;
    function random() {
        randomState ^= randomState << 13;
        randomState ^= randomState >>> 17;
        randomState ^= randomState << 5;
        return (randomState >>> 0) / 4294967296;
    }
    const tokens = [
        'alpha', '  ', 'beta', '\n', '\t', 'A\u0301', 'co\u00adoperate', 'x\u200by',
        'a\u00a0b', '😀', 'אבג'
    ];
    const randomStyles = [
        '',
        'letter-spacing:0.4px;',
        'word-spacing:1.2px;',
        'overflow-wrap:break-word;',
        'text-indent:9px;text-align:right;'
    ];
    for (let index = 0; index < 12; ++index) {
        let text = '';
        const count = 6 + Math.floor(random() * 8);
        for (let tokenIndex = 0; tokenIndex < count; ++tokenIndex) {
            text += tokens[Math.floor(random() * tokens.length)];
        }
        const layout = layoutNames[index % layoutNames.length];
        const spec = {
            name: 'random-' + String(index).padStart(2, '0'),
            text,
            whiteSpace: whitespaceModes[Math.floor(random() * whitespaceModes.length)],
            layout,
            style: randomStyles[Math.floor(random() * randomStyles.length)],
            width: 181 + Math.floor(random() * 98),
            maxWidth: 139 + Math.floor(random() * 66),
            family: index % 2 ? 'FixtureVariable, sans-serif' : 'FixtureRegularOnly, serif',
            mark: index % 4 === 0,
            multipleNodes: index % 3 === 0
        };
        record(spec);
    }

    if (!geometry.length) {
        throw new Error('Intrinsic width fixture produced no geometry');
    }
    return {
        probe: 'intrinsic-width',
        measurements: {geometry},
        visibility: document.visibilityState,
        viewport: [innerWidth, innerHeight, devicePixelRatio]
    };
}

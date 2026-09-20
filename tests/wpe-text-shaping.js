async function edenDOMProbe() {
    document.head.innerHTML = '<style>body{margin:0}#mount{width:900px} .case{margin:0;padding:0;border:0;line-height:1.2}</style>';
    document.body.innerHTML = '<main id="mount"></main>';
    const mount = document.querySelector('#mount');
    const families = ['serif', 'sans-serif', 'monospace', 'MissingTestFont,serif'];
    for (const entry of globalThis.edenFixtureFonts || []) {
        const font = new FontFace(entry.family, 'url(data:font/ttf;base64,' + entry.data + ')', entry.descriptors);
        await font.load();
        document.fonts.add(font);
        families.push(entry.family);
    }
    const texts = [
        'AVATAR To WAVE ffi office affinity',
        'À côté de chez Swann, déjà étonné, français œuvre.',
        'longtemps je me suis couché de bonne heure',
        'office affine efficient difficult official',
        'a\u00a0b a\u00adb\t c\n d',
        'A\u0301 e\u0308 naïve élève',
        'אבג (abc) العربية',
        '中文 日本語 한글',
        'a😀b👨‍👩‍👧‍👦c',
        'AV To\u200bffi\u200d fi',
        'façade garçon Noël où être vingt-cinq',
        '((((AVATAR))))',
        'aµb 1234 ©©© (((( ))))',
    ];
    const styles = [
        '',
        'text-rendering:optimizeLegibility',
        'font-kerning:none',
        'font-variant-ligatures:none',
        'font-feature-settings:"kern" 0,"liga" 0',
        'font-feature-settings:"liga" 1,"dlig" 1',
        'font-weight:700;font-style:italic',
        'letter-spacing:1.3px;word-spacing:2.1px',
        'font-variant:small-caps',
        'writing-mode:vertical-rl;height:160px',
        'font-variation-settings:"wght" 650',
    ];
    const geometry = [];
    for (const family of families) {
        for (const style of styles) {
            for (const direction of ['ltr', 'rtl']) {
                for (const text of texts) {
                    const element = document.createElement('div');
                    element.className = 'case';
                    element.style.cssText = `font:17.25px ${family};direction:${direction};${style};width:173px`;
                    element.textContent = text.repeat(3);
                    mount.replaceChildren(element);
                    const range = document.createRange();
                    range.selectNodeContents(element);
                    const boxes = Array.from(range.getClientRects(), r => [r.x, r.y, r.width, r.height]);
                    const whole = element.getBoundingClientRect();
                    range.setStart(element.firstChild, 1);
                    range.setEnd(element.firstChild, text.length - 1);
                    const selection = Array.from(range.getClientRects(), r => [r.x, r.y, r.width, r.height]);
                    geometry.push([family, style, direction, text, whole.width, whole.height, boxes, selection]);
                }
            }
        }
    }
    mount.replaceChildren();
    return {probe:'text-shaping', measurements:{geometry}, visibility:document.visibilityState, viewport:[innerWidth,innerHeight,devicePixelRatio]};
}

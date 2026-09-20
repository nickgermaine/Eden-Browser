async function edenDOMProbe() {
    document.body.style.margin = '0';
    const canvases = [];
    for (const kind of ['geometry', 'text', 'state']) {
        const canvas = document.createElement('canvas');
        canvas.width = 512;
        canvas.height = 256;
        document.body.appendChild(canvas);
        const context = canvas.getContext('2d');
        context.fillStyle = '#14283c';
        context.fillRect(0, 0, 512, 256);
        context.save();
        context.beginPath();
        context.rect(8, 8, 496, 240);
        context.clip();
        if (kind === 'geometry') {
            for (let index = 0; index < 768; ++index) {
                context.fillStyle = index % 2 ? '#e65a28' : '#32aadc';
                context.globalAlpha = 0.6;
                context.beginPath();
                context.arc(index % 32 * 16 + 8.25, Math.floor(index / 32) % 16 * 16 + 8.5, 7, 0, Math.PI * 2);
                context.fill();
            }
        } else if (kind === 'text') {
            context.fillStyle = 'white';
            context.font = '18px serif';
            for (let index = 0; index < 512; ++index) {
                context.fillText('office ffi AV', index % 4 * 128, Math.floor(index / 4) % 12 * 20 + 18);
            }
        } else {
            for (let index = 0; index < 768; ++index) {
                context.save();
                context.translate(index % 32 * 16 + 8, Math.floor(index / 32) % 16 * 16 + 8);
                context.rotate(index % 4 * Math.PI / 8);
                context.fillStyle = '#e65a28';
                context.fillRect(-5, -5, 10, 10);
                context.restore();
            }
            const image = context.getImageData(0, 0, 8, 8);
            context.putImageData(image, 200, 200);
            context.fillStyle = '#32aadc';
            context.fillRect(300, 200, 20, 20);
        }
        context.restore();
        const background = Array.from(context.getImageData(0, 0, 1, 1).data);
        canvases.push({kind, background, png: canvas.toDataURL('image/png')});
    }
    return {probe: 'canvas-raster', measurements: {canvases}, visibility: document.visibilityState, viewport: [innerWidth, innerHeight, devicePixelRatio]};
}

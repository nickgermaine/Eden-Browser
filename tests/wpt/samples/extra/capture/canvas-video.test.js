'use strict';
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {pixels, read, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/capture/canvas-video/index.html`;

describe('capture canvas to video', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('plays an animated WebGL canvas in a video element', async () => {
    await waitFor(driver, 'typeof gl !== "undefined" && gl !== null && typeof controller !== "undefined" && controller !== null', 'WebGL never started in the canvas');
    await driver.executeScript('window.__spin = setInterval(() => { controller.yRot = (controller.yRot + 5) % 360; draw(); }, 33);');
    expect(await read(driver, 'video.srcObject === stream && stream.getVideoTracks()[0].readyState === "live"')).toBe(true);
    await waitFor(driver, 'video.readyState >= 2 && video.videoWidth === canvas.width && video.videoHeight === canvas.height',
        'the video never showed the canvas stream', {detail: '({video: [video.readyState, video.videoWidth, video.videoHeight], canvas: [canvas.width, canvas.height]})'});
    const center = await waitFor(driver, `(() => { const stats = ${pixels('video', null)}; return stats && stats.lit > 0.02 ? stats : null; })()`,
        'the video stayed black instead of showing the teapot', {detail: pixels('video', null)});
    expect(center.lit).toBeGreaterThan(0.02);
  });
});

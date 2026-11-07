'use strict';
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {media, pixels, read, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/capture/canvas-filter/index.html`;

describe('capture canvas filter', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('streams the filtered camera canvas into the result video', async () => {
    await waitFor(driver, 'inputStream !== null && source.videoWidth > 0', 'the camera never reached the source video', {detail: media('#source')});
    await waitFor(driver, 'imageData !== null', 'the canvas loop never copied a frame');
    await waitFor(driver, 'result.readyState >= 2 && result.videoWidth > 0 && result.videoWidth === source.videoWidth',
        'the result video never showed the canvas stream', {detail: media('#result')});
    const square = await waitFor(driver, `(() => { const stats = ${pixels('#result', {x: 45, y: 45, width: 10, height: 10})}; return stats && stats.mean[0] > 200 && stats.mean[1] < 60 && stats.mean[2] < 60 ? stats : null; })()`,
        'the red square drawn into the canvas never reached the result video', {detail: pixels('#result', {x: 45, y: 45, width: 10, height: 10})});
    expect(square.mean[0]).toBeGreaterThan(200);
    expect(await read(driver, 'stream.getVideoTracks()[0].readyState')).toBe('live');
  });
});

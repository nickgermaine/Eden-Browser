'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {media, pixels, read, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/getusermedia/canvas/index.html`;

describe('getusermedia canvas', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('takes a snapshot of the camera into a canvas', async () => {
    await waitFor(driver, 'video.readyState >= 2 && video.videoWidth > 0', 'the camera never played', {detail: media('video')});
    await driver.findElement(webdriver.By.css('button')).click();
    await waitFor(driver, 'canvas.width === video.videoWidth && canvas.height === video.videoHeight', 'the snapshot canvas never took the camera size',
        {detail: '({canvas: [canvas.width, canvas.height], video: [video.videoWidth, video.videoHeight]})'});
    const stats = await read(driver, pixels('canvas', null));
    expect(stats.lit).toBeGreaterThan(0.02);
  });
});

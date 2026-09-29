'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {choose, media, pixels, read, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/getusermedia/filter/index.html`;

describe('getusermedia filter', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('filters the camera and takes a snapshot', async () => {
    await waitFor(driver, 'video.readyState >= 2 && video.videoWidth > 0', 'the camera never played', {detail: media('video')});
    await choose(driver, webdriver, '#filter option[value="grayscale"]');
    await waitFor(driver, 'video.className === "grayscale"', 'choosing a filter never styled the video', {detail: 'video.className'});
    expect(await read(driver, 'getComputedStyle(video).filter')).toBe('grayscale(1)');
    await driver.findElement(webdriver.By.id('snapshot')).click();
    await waitFor(driver, 'canvas.className === "grayscale"', 'the snapshot never took the filter', {detail: 'canvas.className'});
    const stats = await read(driver, pixels('canvas', null));
    expect(stats.lit).toBeGreaterThan(0.02);
  });
});

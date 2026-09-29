'use strict';
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {media, read, requireFeature, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/capture/video-video/index.html`;

describe('capture video to video', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('plays a captured video in a second video element', async () => {
    await requireFeature(driver, 'typeof HTMLMediaElement.prototype.captureStream === "function"', 'HTMLMediaElement.captureStream()');
    await driver.executeScript('return leftVideo.play().then(() => true, error => error.name + ": " + error.message);');
    await waitFor(driver, 'rightVideo.srcObject instanceof MediaStream && rightVideo.readyState >= 2 && rightVideo.videoWidth === 480 && !rightVideo.paused',
        'the captured video never played', {detail: media('#rightVideo')});
    const before = await read(driver, 'rightVideo.currentTime');
    await driver.sleep(1000);
    expect(await read(driver, 'rightVideo.currentTime')).toBeGreaterThan(before);
  });
});

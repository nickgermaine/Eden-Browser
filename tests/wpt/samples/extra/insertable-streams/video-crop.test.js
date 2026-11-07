'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {media, requireFeature, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/insertable-streams/video-crop/index.html`;

describe('insertable streams video crop', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('crops the camera in a worker', async () => {
    await waitFor(driver, 'document.readyState === "complete"', 'the page never finished loading');
    await requireFeature(driver, 'typeof MediaStreamTrackProcessor !== "undefined" && typeof MediaStreamTrackGenerator !== "undefined"',
        'MediaStreamTrackProcessor and MediaStreamTrackGenerator on the main thread');
    await driver.findElement(webdriver.By.id('startButton')).click();
    await waitFor(driver, 'localVideo.videoWidth === 1280', 'the camera never opened at 1280x720', {detail: media('#localVideo')});
    await waitFor(driver, 'croppedVideo.readyState >= 2 && croppedVideo.videoWidth === 640 && croppedVideo.videoHeight === 360', 'the cropped video never played at 640x360',
        {detail: media('#croppedVideo')});
  });
});

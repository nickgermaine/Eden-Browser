'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {click, media, read, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/getusermedia/getdisplaymedia/index.html`;

describe('getusermedia getDisplayMedia', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('shares the screen into a video element', async () => {
    await waitFor(driver, 'typeof adapter !== "undefined"', 'adapter.js never loaded');
    await click(driver, webdriver, 'startButton');
    await waitFor(driver, `(() => {
      const video = document.getElementById('video');
      const track = video.srcObject && video.srcObject.getVideoTracks()[0];
      return !!track && track.readyState === 'live' && video.readyState >= 2 && video.videoWidth > 0;
    })()`, 'the shared screen never played', {detail: `({media: ${media('#video')}, error: document.getElementById('errorMsg').textContent})`});
    expect(await read(driver, 'document.getElementById("startButton").disabled')).toBe(true);
    expect(await read(driver, 'document.getElementById("errorMsg").textContent.trim()')).toBe('');
  });
});

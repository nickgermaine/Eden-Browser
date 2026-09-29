'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {click, media, read, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/getusermedia/exposure/index.html`;

describe('getusermedia exposure', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('offers exposure, brightness and white balance controls for the camera', async () => {
    await click(driver, webdriver, 'showVideo');
    await waitFor(driver, 'window.track && window.track.readyState === "live" && document.getElementById("gum-local").videoWidth > 0 && document.getElementById("showVideo").disabled && !document.getElementById("refreshControls").disabled',
        'the camera never opened', {detail: media('#gum-local')});
    const missing = await read(driver, '[...document.querySelectorAll("#errorMsg p")].map(paragraph => paragraph.textContent.trim())');
    if (missing.length) {
      throw new Error(`Missing feature: ${missing.join(' ')}`);
    }
  });
});

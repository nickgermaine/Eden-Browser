'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {click, media, read, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/getusermedia/pan-tilt-zoom/index.html`;

describe('getusermedia pan tilt zoom', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('opens the camera with pan, tilt and zoom and applies a zoom change', async () => {
    await click(driver, webdriver, 'showVideo');
    await waitFor(driver, 'window.track && window.track.readyState === "live" && document.getElementById("gum-local").videoWidth > 0 && document.getElementById("showVideo").disabled',
        'the camera never opened', {detail: media('#gum-local')});
    const missing = await read(driver, '[...document.querySelectorAll("#errorMsg p")].map(paragraph => paragraph.textContent.trim())');
    if (missing.length) {
      throw new Error(`Missing feature: ${missing.join(' ')}`);
    }
    const target = await read(driver, `(() => {
      const input = document.querySelector('input[name=zoom]');
      const value = Math.min(Number(input.max), Number(input.min) + Number(input.step || 1));
      input.value = value;
      input.dispatchEvent(new Event('input'));
      return value;
    })()`);
    await waitFor(driver, `window.track.getSettings().zoom == ${target}`, 'the zoom change was never applied', {detail: 'window.track.getSettings().zoom'});
  });
});

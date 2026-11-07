'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {click, read, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/getusermedia/volume/index.html`;

describe('getusermedia volume', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('meters the microphone with an audio worklet', async () => {
    await click(driver, webdriver, 'startButton');
    await waitFor(driver, 'window.audioContext && window.audioContext.state === "running"', 'the audio context never ran',
        {detail: 'window.audioContext && window.audioContext.state'});
    await waitFor(driver, 'window.soundMeter && window.soundMeter.node instanceof AudioWorkletNode', 'the audio worklet meter never started');
    await waitFor(driver, 'window.soundMeter.slow > 0.001', 'the meter never heard the microphone', {timeout: 15000, detail: '({instant: window.soundMeter.instant, slow: window.soundMeter.slow})'});
    expect(await read(driver, 'document.querySelector("#instant .value").innerText !== ""')).toBe(true);
  });
});

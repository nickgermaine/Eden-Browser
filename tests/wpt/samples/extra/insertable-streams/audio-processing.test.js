'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {click, read, requireFeature, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/insertable-streams/audio-processing/index.html`;

describe('insertable streams audio processing', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('filters the microphone in a worker and plays the result', async () => {
    await waitFor(driver, 'document.readyState === "complete"', 'the page never finished loading');
    await requireFeature(driver, 'typeof MediaStreamTrackProcessor !== "undefined" && typeof MediaStreamTrackGenerator !== "undefined"',
        'MediaStreamTrackProcessor and MediaStreamTrackGenerator on the main thread');
    await click(driver, webdriver, 'startButton');
    await waitFor(driver, 'generator && generator.readyState === "live" && processedStream && processedStream.getAudioTracks()[0] === generator',
        'the processed audio track never went live', {detail: 'document.getElementById("errorMsg").textContent'});
    await waitFor(driver, 'audio.srcObject === processedStream && !audio.paused', 'the processed audio never played', {detail: '({paused: audio.paused, time: audio.currentTime})'});
    const before = await read(driver, 'audio.currentTime');
    await driver.sleep(1000);
    expect(await read(driver, 'audio.currentTime')).toBeGreaterThan(before);
    expect(await read(driver, 'document.getElementById("errorMsg").textContent.trim()')).toBe('');
  });
});

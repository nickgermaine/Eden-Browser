'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {choose, click, media, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/getusermedia/record/index.html`;

async function recordAndPlay(mimeType) {
  await click(driver, webdriver, 'start-gum');
  await waitFor(driver, '!document.getElementById("record").disabled && codecPreferences.options.length > 0', 'the camera never opened for recording',
      {detail: 'document.getElementById("errorMsg").textContent'});
  await waitFor(driver, `[...codecPreferences.options].some(option => option.value === ${JSON.stringify(mimeType)})`, `Missing feature: MediaRecorder does not support ${mimeType}`,
      {timeout: 2000, detail: '[...codecPreferences.options].map(option => option.value)'});
  await choose(driver, webdriver, `#codecPreferences option[value="${mimeType}"]`);
  await click(driver, webdriver, 'record');
  await waitFor(driver, 'document.getElementById("record").textContent === "Stop Recording" && mediaRecorder && mediaRecorder.state === "recording"',
      'recording never started', {detail: '({text: document.getElementById("record").textContent, error: document.getElementById("errorMsg").textContent})'});
  await driver.sleep(2500);
  await click(driver, webdriver, 'record');
  await waitFor(driver, 'mediaRecorder.state === "inactive" && recordedBlobs.reduce((total, blob) => total + blob.size, 0) > 0', 'the recorder produced no data',
      {detail: '({state: mediaRecorder.state, blobs: recordedBlobs.length, error: document.getElementById("errorMsg").textContent})'});
  await click(driver, webdriver, 'play');
  await waitFor(driver, 'recordedVideo.src.startsWith("blob:") && recordedVideo.readyState >= 2 && recordedVideo.videoWidth > 0 && recordedVideo.error === null && !recordedVideo.paused',
      `the ${mimeType} recording never played back`, {detail: media('#recorded')});
}

describe('getusermedia record', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('records VP8 and Opus in WebM and plays it back', async () => {
    await recordAndPlay('video/webm;codecs=vp8,opus');
  });

  it('records VP9 and Opus in WebM and plays it back', async () => {
    await recordAndPlay('video/webm;codecs=vp9,opus');
  });

  it('records H.264 and AAC in MP4 and plays it back', async () => {
    await recordAndPlay('video/mp4;codecs=avc1.64003E,mp4a.40.2');
  });
});

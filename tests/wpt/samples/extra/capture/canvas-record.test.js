'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {click, media, read, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/capture/canvas-record/index.html`;

describe('capture canvas recording', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('records an animated canvas and plays the recording back', async () => {
    await waitFor(driver, 'typeof gl !== "undefined" && gl !== null && typeof controller !== "undefined" && controller !== null', 'WebGL never started in the canvas');
    await driver.executeScript('window.__spin = setInterval(() => { controller.yRot = (controller.yRot + 5) % 360; draw(); }, 33);');
    await click(driver, webdriver, 'record');
    await waitFor(driver, 'recordButton.textContent === "Stop Recording" && mediaRecorder && mediaRecorder.state === "recording"',
        'recording never started', {detail: '({text: recordButton.textContent, state: mediaRecorder && mediaRecorder.state})'});
    await driver.sleep(2500);
    await click(driver, webdriver, 'record');
    await waitFor(driver, 'mediaRecorder.state === "inactive" && recordedBlobs.reduce((total, blob) => total + blob.size, 0) > 0',
        'the recorder produced no data', {detail: '({state: mediaRecorder.state, blobs: recordedBlobs.length, type: mediaRecorder.mimeType})'});
    await click(driver, webdriver, 'play');
    await waitFor(driver, 'video.src.startsWith("blob:") && video.readyState >= 2 && video.videoWidth > 0 && video.error === null && !video.paused',
        'the recording never played back', {detail: media('#recorded')});
    expect(await read(driver, 'mediaRecorder.mimeType.length > 0')).toBe(true);
  });
});

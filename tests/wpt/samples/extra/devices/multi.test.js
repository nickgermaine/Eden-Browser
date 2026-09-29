'use strict';
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {media, read, requireFeature, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/devices/multi/index.html`;

describe('devices multi', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('plays the camera and microphone', async () => {
    await waitFor(driver, 'window.stream && window.stream.getAudioTracks().length === 1 && window.stream.getVideoTracks().length === 1', 'getUserMedia never delivered audio and video');
    await waitFor(driver, 'document.querySelector("video.gum").srcObject === window.stream && document.querySelector("video.gum").videoWidth > 0',
        'the camera never played', {detail: media('video.gum')});
    expect(await read(driver, 'document.querySelectorAll("select").length')).toBe(4);
  });

  it('routes a media element to a chosen output device', async () => {
    await requireFeature(driver, '"sinkId" in HTMLMediaElement.prototype && typeof HTMLMediaElement.prototype.setSinkId === "function"', 'audio output selection (setSinkId)');
    await waitFor(driver, 'window.stream && document.querySelector("video.gum").videoWidth > 0', 'the camera never played', {detail: media('video.gum')});
    const outputs = await driver.executeAsyncScript((done) => {
      navigator.mediaDevices.enumerateDevices().then(devices => done(devices.filter(device => device.kind === 'audiooutput').map(device => device.deviceId)), error => done({error: `${error.name}: ${error.message}`}));
    });
    expect(Array.isArray(outputs)).toBe(true);
    if (!outputs.length) {
      throw new Error('Missing feature: no audio output devices are listed by enumerateDevices()');
    }
    const chosen = await driver.executeAsyncScript((deviceId, done) => {
      const video = document.querySelector('video.gum');
      video.setSinkId(deviceId).then(() => done(video.sinkId), error => done({error: `${error.name}: ${error.message}`}));
    }, outputs[outputs.length - 1]);
    expect(chosen).toBe(outputs[outputs.length - 1]);
  });
});

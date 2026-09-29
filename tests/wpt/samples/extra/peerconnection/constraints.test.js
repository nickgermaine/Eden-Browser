'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {click, media, read, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/peerconnection/constraints/index.html`;

describe('peerconnection constraints', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('honours size constraints and connects with stats', async () => {
    await click(driver, webdriver, 'getMedia');
    await waitFor(driver, 'localVideo.videoWidth >= 300 && localVideo.videoWidth <= 640 && localVideo.videoHeight >= 200 && localVideo.videoHeight <= 480',
        'the camera did not honour the size constraints', {detail: media('div#localVideo video')});
    await click(driver, webdriver, 'connect');
    await waitFor(driver, 'pc1 && pc1.connectionState === "connected"', 'the call never connected', {detail: 'pc1 && pc1.connectionState'});
    await waitFor(driver, 'remoteVideo.videoWidth > 0', 'the remote video never played', {detail: media('div#remoteVideo video')});
    await waitFor(driver, 'document.querySelector("div#remoteVideo div").textContent.startsWith("Video dimensions:")', 'the remote video size was never reported');
    await waitFor(driver, 'document.getElementById("peer").textContent.startsWith("Connected to:")', 'the connected peer was never reported',
        {detail: 'document.getElementById("peer").textContent'});
    await waitFor(driver, 'document.getElementById("receiverStats").textContent.includes("Receiver stats")', 'receiver stats were never shown');
    expect(await read(driver, '!document.getElementById("hangup").disabled')).toBe(true);
  });
});

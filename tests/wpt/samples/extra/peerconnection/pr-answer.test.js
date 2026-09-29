'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {click, media, read, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/peerconnection/pr-answer/index.html`;

describe('peerconnection provisional answer', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('holds media on a provisional answer and starts it on the final answer', async () => {
    await click(driver, webdriver, 'callButton');
    await waitFor(driver, 'pc1 && pc1.signalingState === "have-remote-pranswer" && pc2.signalingState === "have-local-pranswer"', 'the provisional answer was never applied',
        {detail: '({pc1: pc1 && pc1.signalingState, pc2: pc2 && pc2.signalingState})'});
    expect(await read(driver, 'vid2.srcObject === remoteStream')).toBe(true);
    expect(await read(driver, 'vid2.videoWidth')).toBe(0);
    await click(driver, webdriver, 'acceptButton');
    await waitFor(driver, 'pc1.signalingState === "stable" && pc2.signalingState === "stable"', 'the final answer was never applied',
        {detail: '({pc1: pc1.signalingState, pc2: pc2.signalingState})'});
    await waitFor(driver, 'vid2.readyState >= 2 && vid2.videoWidth > 0', 'media never started after the final answer', {detail: media('#vid2')});
    expect(await read(driver, 'remoteStream.getTracks().length')).toBe(2);
  });
});

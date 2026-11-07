'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {click, media, read, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/peerconnection/always-negotiate-datachannels/index.html`;

describe('peerconnection always negotiate data channels', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('keeps the call alive through a renegotiation that stops a transceiver', async () => {
    await click(driver, webdriver, 'startButton');
    await driver.wait(() => driver.findElement(webdriver.By.id('negotiateDataChannel')).isEnabled(), 20000, 'the checkbox never became enabled');
    await driver.findElement(webdriver.By.id('negotiateDataChannel')).click();
    await click(driver, webdriver, 'callButton');
    await waitFor(driver, 'pc1 && pc1.connectionState === "connected"', 'the call never connected', {detail: 'pc1 && pc1.connectionState'});
    await waitFor(driver, 'remoteVideo.videoWidth > 0', 'the remote video never played', {detail: media('#remoteVideo')});
    const support = await read(driver, 'document.getElementById("log").textContent');
    if (!support.includes('alwaysNegotiateDataChannel is supported')) {
      throw new Error(`Missing feature: RTCConfiguration.alwaysNegotiateDataChannels (${support.trim()})`);
    }
    await click(driver, webdriver, 'renegotiateButton');
    await waitFor(driver, 'document.getElementById("log").textContent.includes("Renegotiation done")', 'the renegotiation never finished',
        {timeout: 25000, detail: 'document.getElementById("log").textContent'});
    await waitFor(driver, '!document.getElementById("hangupButton").disabled', 'the call did not survive the renegotiation');
  });
});

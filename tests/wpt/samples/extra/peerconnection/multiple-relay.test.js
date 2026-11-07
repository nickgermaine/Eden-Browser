'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {click, media, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/peerconnection/multiple-relay/index.html`;

describe('peerconnection multiple relay', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('relays a call through a second peer connection pair', async () => {
    await click(driver, webdriver, 'start');
    await click(driver, webdriver, 'call');
    await waitFor(driver, 'statusDiv.textContent === "1 element(s) in chain"', 'the first call never started', {detail: '({status: statusDiv.textContent, pipes: pipes.length})'});
    await waitFor(driver, 'video2.readyState >= 2 && video2.videoWidth > 0', 'the relayed video never played', {detail: media('#video2')});
    await click(driver, webdriver, 'insertRelay');
    await waitFor(driver, 'statusDiv.textContent === "2 element(s) in chain"', 'the relay never started', {detail: '({status: statusDiv.textContent, pipes: pipes.length})'});
    await waitFor(driver, 'pipes.length === 2 && pipes.every(pipe => ["connected", "completed"].includes(pipe.pc2.iceConnectionState))', 'the relay never connected',
        {detail: 'pipes.map(pipe => pipe.pc2.iceConnectionState)'});
    await waitFor(driver, 'video2.readyState >= 2 && video2.videoWidth > 0', 'the relayed video stopped', {detail: media('#video2')});
  });
});

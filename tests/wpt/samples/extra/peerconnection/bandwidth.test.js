'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {choose, click, media, read, rtpStats, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/peerconnection/bandwidth/index.html`;

describe('peerconnection bandwidth', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('caps the sending bitrate with setParameters', async () => {
    await click(driver, webdriver, 'callButton');
    await waitFor(driver, 'pc1 && ["connected", "completed"].includes(pc1.iceConnectionState)', 'the call never connected', {detail: 'pc1 && pc1.iceConnectionState'});
    await waitFor(driver, 'remoteVideo.videoWidth > 0', 'the remote video never played', {detail: media('#remoteVideo')});
    await waitFor(driver, '!bandwidthSelector.disabled', 'the bandwidth selector never became usable');
    await choose(driver, webdriver, '#bandwidth option[value="125"]');
    await waitFor(driver, '!bandwidthSelector.disabled && pc1.getSenders()[0].getParameters().encodings[0].maxBitrate === 125000', 'the 125 kbps cap was never applied',
        {detail: '({disabled: bandwidthSelector.disabled, encodings: pc1.getSenders()[0].getParameters().encodings})'});
    const first = await read(driver, rtpStats('pc1', 'outbound-rtp', 'video'));
    await driver.sleep(5000);
    const second = await read(driver, rtpStats('pc1', 'outbound-rtp', 'video'));
    const kbps = (second.bytesSent - first.bytesSent) * 8 / 5000;
    expect(kbps).toBeGreaterThan(20);
    expect(kbps).toBeLessThan(160);
  });
});

'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {click, media, pixels, rtpStats, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/peerconnection/webaudio-output/index.html`;

describe('peerconnection webaudio output', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('visualizes the received audio with WebAudio', async () => {
    await click(driver, webdriver, 'startButton');
    await click(driver, webdriver, 'callButton');
    await waitFor(driver, 'pc1 && ["connected", "completed"].includes(pc1.iceConnectionState)', 'the call never connected', {detail: 'pc1 && pc1.iceConnectionState'});
    await waitFor(driver, 'remoteVideo.videoWidth > 0', 'the remote video never played', {detail: media('#remoteVideo')});
    await waitFor(driver, `(async () => { const stats = await ${rtpStats('pc2', 'inbound-rtp', 'audio')}; return stats && stats.totalAudioEnergy > 0; })()`,
        'no audio energy was received', {detail: rtpStats('pc2', 'inbound-rtp', 'audio')});
    await waitFor(driver, `(() => { const stats = ${pixels('canvas', null)}; return stats && stats.lit > 0.001 && stats.white < 0.999; })()`, 'the visualizer never drew',
        {timeout: 10000, detail: pixels('canvas', null)});
  });
});

'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {choose, click, media, read, requireFeature, rtpStats, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/extensions/svc/index.html`;

async function callWith(codecSelector, mode, mimeType, reported = mode) {
  await click(driver, webdriver, 'startButton');
  await requireFeature(driver, 'supportsSetCodecPreferences', 'RTCRtpTransceiver.setCodecPreferences()');
  await waitFor(driver, `!codecPreferences.disabled && !!document.querySelector(${JSON.stringify(codecSelector)})`, `${mimeType} is not offered for sending`,
      {detail: '[...codecPreferences.options].map(option => option.value)'});
  await choose(driver, webdriver, codecSelector);
  await waitFor(driver, `!scalabilityMode.disabled && [...scalabilityMode.options].some(option => option.value === ${JSON.stringify(mode)})`,
      `Missing feature: ${mimeType} ${mode} is not reported as supported by MediaCapabilities`, {detail: '[...scalabilityMode.options].map(option => option.value)'});
  await choose(driver, webdriver, `#scalabilityMode option[value="${mode}"]`);
  await click(driver, webdriver, 'callButton');
  await waitFor(driver, 'pc1 && pc1.connectionState === "connected"', 'the call never connected', {detail: '({state: pc1 && pc1.connectionState, senders: pc1 && pc1.getSenders().length})'});
  await waitFor(driver, 'remoteVideo.readyState >= 2 && remoteVideo.videoWidth > 0', 'the remote video never played', {detail: media('#remoteVideo')});
  expect(await read(driver, 'pc1.getSenders().find(sender => sender.track && sender.track.kind === "video").getParameters().encodings[0].scalabilityMode')).toBe(mode);
  await waitFor(driver, `(async () => { const stats = await ${rtpStats('pc1', 'outbound-rtp', 'video')}; return stats && new RegExp(${JSON.stringify(reported)}).test(stats.scalabilityMode || ''); })()`,
      `the outbound stats never reported ${mode}`, {detail: rtpStats('pc1', 'outbound-rtp', 'video')});
  await waitFor(driver, `document.getElementById("actualCodec").innerText.startsWith("Using ${mimeType}")`, `the call never reported ${mimeType} as its codec`,
      {detail: 'document.getElementById("actualCodec").innerText'});
}

describe('extensions scalable video coding', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('sends VP8 with three temporal layers', async () => {
    await callWith('#codecPreferences option[value="video/VP8"]', 'L1T3', 'video/VP8', '^L1T3$');
  });

  it('sends VP9 with spatial and temporal layers', async () => {
    await callWith('#codecPreferences option[value^="video/VP9"]', 'L3T3', 'video/VP9', '^L[23]T3');
  });

  it('sends AV1 with three temporal layers', async () => {
    await callWith('#codecPreferences option[value^="video/AV1"]', 'L1T3', 'video/AV1', '^L1T3$');
  });
});

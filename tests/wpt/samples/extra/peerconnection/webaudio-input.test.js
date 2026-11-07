'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {click, read, rtpStats, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/peerconnection/webaudio-input/index.html`;

describe('peerconnection webaudio input', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('sends WebAudio-processed microphone audio with an injected clip', async () => {
    await click(driver, webdriver, 'start');
    await waitFor(driver, 'pc1 && pc1.connectionState === "connected"', 'the call never connected', {detail: '({state: pc1 && pc1.connectionState, error: document.getElementById("errorMsg").textContent})'});
    await waitFor(driver, 'audioElement.srcObject && !audioElement.paused', 'the received audio never played', {detail: '({paused: audioElement.paused})'});
    await waitFor(driver, 'webAudio.context.state === "running" && webAudio.soundBuffer !== null', 'the audio graph never ran with the clip loaded',
        {detail: '({state: webAudio.context.state, buffer: webAudio.soundBuffer !== null})'});
    const before = await read(driver, rtpStats('pc2', 'inbound-rtp', 'audio'));
    await driver.actions({async: true}).sendKeys('a').perform();
    await waitFor(driver, `(async () => { const now = await ${rtpStats('pc2', 'inbound-rtp', 'audio')}; return now && now.totalAudioEnergy > ${before && before.totalAudioEnergy || 0} + 0.0001; })()`,
        'the injected clip never reached the far end', {detail: rtpStats('pc2', 'inbound-rtp', 'audio')});
    expect(await read(driver, 'document.getElementById("errorMsg").textContent.trim()')).toBe('');
  });
});

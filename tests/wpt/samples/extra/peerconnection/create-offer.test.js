'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {read, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/peerconnection/create-offer/index.html`;

async function offer(tracks, audio, video) {
  await driver.executeScript((count, withAudio, withVideo) => {
    numAudioTracksInput.value = count;
    numAudioTracksInput.dispatchEvent(new Event('change'));
    audioInput.checked = withAudio;
    videoInput.checked = withVideo;
    outputTextarea.value = '';
  }, tracks, audio, video);
  await driver.findElement(webdriver.By.id('createOffer')).click();
  return waitFor(driver, 'outputTextarea.value.startsWith("v=0") ? outputTextarea.value : ""', 'no offer was produced', {detail: 'outputTextarea.value'});
}

describe('peerconnection create offer', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('offers one audio section per track and a video section on request', async () => {
    const sdp = await offer(3, true, true);
    expect((sdp.match(/^m=audio/mg) || []).length).toBe(3);
    expect(/^m=video/m.test(sdp)).toBe(true);
    expect(await read(driver, 'window.peerConnection.signalingState')).toBe('have-local-offer');
  });

  it('offers to receive with the legacy offer options', async () => {
    const sdp = await offer(0, true, true);
    expect((sdp.match(/^m=audio/mg) || []).length).toBe(1);
    expect(/^m=video/m.test(sdp)).toBe(true);
  });
});

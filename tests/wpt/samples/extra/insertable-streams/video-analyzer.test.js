'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {click, media, read, requireFeature, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/insertable-streams/video-analyzer/index.html`;

describe('insertable streams video analyzer', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('counts received key and delta frames and follows a resolution change', async () => {
    await requireFeature(driver, 'supportsInsertableStreams', 'RTCRtpSender.createEncodedStreams() (Chrome encoded insertable streams)');
    await click(driver, webdriver, 'startButton');
    await click(driver, webdriver, 'callButton');
    await waitFor(driver, 'pc1 && ["connected", "completed"].includes(pc1.iceConnectionState)', 'the call never connected', {detail: 'pc1 && pc1.iceConnectionState'});
    await waitFor(driver, 'remoteVideo.videoWidth > 0', 'the remote video never played', {detail: media('#remoteVideo')});
    await waitFor(driver, 'keyFrameCount >= 1 && interFrameCount >= 1', 'the analyzer never saw key and delta frames', {detail: '({keyFrameCount, interFrameCount})'});
    const before = await read(driver, 'keyFrameCount + interFrameCount');
    await driver.sleep(1500);
    expect(await read(driver, 'keyFrameCount + interFrameCount')).toBeGreaterThan(before);
    expect(await read(driver, 'duplicateCount')).toBe(0);
    await click(driver, webdriver, 'size-small');
    await waitFor(driver, 'document.getElementById("video-size").textContent.startsWith("180x")', 'the analyzer never saw the smaller frames',
        {detail: 'document.getElementById("video-size").textContent'});
  });
});

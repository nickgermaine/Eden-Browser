'use strict';
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {media, read, requireFeature, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/capture/video-pc/index.html`;

describe('capture video to peer connection', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('sends a captured video file with its audio through a peer connection', async () => {
    await requireFeature(driver, 'typeof HTMLMediaElement.prototype.captureStream === "function"', 'HTMLMediaElement.captureStream()');
    await waitFor(driver, 'stream instanceof MediaStream', 'the left video was never captured', {detail: media('#leftVideo')});
    await waitFor(driver, 'pc1 && ["connected", "completed"].includes(pc1.iceConnectionState)', 'the peer connection never connected',
        {detail: '({signaling: pc1 && pc1.signalingState, ice: pc1 && pc1.iceConnectionState})'});
    await waitFor(driver, 'rightVideo.readyState >= 2 && rightVideo.videoWidth === 480 && rightVideo.videoHeight === 270',
        'the received video never reached 480x270', {detail: media('#rightVideo')});
    expect(await read(driver, 'rightVideo.srcObject.getAudioTracks().length')).toBe(1);
  });
});

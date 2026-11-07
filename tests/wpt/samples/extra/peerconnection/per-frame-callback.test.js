'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {click, media, requireFeature, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/peerconnection/per-frame-callback/index.html`;

describe('peerconnection per frame callback', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('graphs frame timing from requestVideoFrameCallback', async () => {
    await requireFeature(driver, '"requestVideoFrameCallback" in HTMLVideoElement.prototype', 'HTMLVideoElement.requestVideoFrameCallback()');
    await click(driver, webdriver, 'callButton');
    await waitFor(driver, 'pc1 && ["connected", "completed"].includes(pc1.iceConnectionState)', 'the call never connected', {detail: 'pc1 && pc1.iceConnectionState'});
    await waitFor(driver, 'remoteVideo.videoWidth > 0', 'the remote video never played', {detail: media('#remoteVideo')});
    await waitFor(driver, 'processingSeries.getCount() > 0 && localDelaySeries.getCount() > 0', 'no frame timing was graphed',
        {detail: '({processing: processingSeries && processingSeries.getCount(), local: localDelaySeries && localDelaySeries.getCount()})'});
  });
});

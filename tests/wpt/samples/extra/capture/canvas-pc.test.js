'use strict';
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {growing, media, rtpStats, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/capture/canvas-pc/index.html`;

describe('capture canvas to peer connection', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('sends an animated WebGL canvas through a peer connection', async () => {
    await waitFor(driver, 'typeof gl !== "undefined" && gl !== null && typeof controller !== "undefined" && controller !== null', 'WebGL never started in the canvas');
    await driver.executeScript('window.__spin = setInterval(() => { controller.yRot = (controller.yRot + 5) % 360; draw(); }, 33);');
    await waitFor(driver, 'pc1 && ["connected", "completed"].includes(pc1.iceConnectionState)', 'the peer connection never connected',
        {detail: '({signaling: pc1 && pc1.signalingState, ice: pc1 && pc1.iceConnectionState, local: !!(pc1 && pc1.localDescription)})'});
    await waitFor(driver, 'video.readyState >= 2 && video.videoWidth === canvas.width && video.videoHeight === canvas.height',
        'the received video never matched the canvas size', {detail: '({video: [video.readyState, video.videoWidth, video.videoHeight], canvas: [canvas.width, canvas.height]})'});
    await growing(driver, rtpStats('pc2', 'inbound-rtp', 'video'), 'framesDecoded', 'received canvas frames');
    expect(await driver.executeScript(() => document.getElementById('autoplay').style.display)).toBe('none');
  });
});

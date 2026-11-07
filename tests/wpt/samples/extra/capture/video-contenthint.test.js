'use strict';
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {media, read, requireFeature, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/capture/video-contenthint/index.html`;

describe('capture video content hints', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('sends a captured video with motion and detail hints', async () => {
    await requireFeature(driver, 'typeof HTMLMediaElement.prototype.captureStream === "function"', 'HTMLMediaElement.captureStream()');
    await waitFor(driver, 'srcStream instanceof MediaStream', 'the source video was never captured', {detail: media('#srcVideo')});
    await waitFor(driver, 'motionVideo.readyState >= 2 && motionVideo.videoWidth > 0', 'the motion-hinted call never showed video', {detail: media('#motionVideo')});
    await waitFor(driver, 'detailVideo.readyState >= 2 && detailVideo.videoWidth > 0', 'the detail-hinted call never showed video', {detail: media('#detailVideo')});
    expect(await read(driver, 'motionStream.getVideoTracks()[0].contentHint')).toBe('motion');
    expect(await read(driver, 'detailStream.getVideoTracks()[0].contentHint')).toBe('detail');
  });
});

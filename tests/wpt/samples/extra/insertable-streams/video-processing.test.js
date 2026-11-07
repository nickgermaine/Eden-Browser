'use strict';
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {pick, pixels, read, requireFeature, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/insertable-streams/video-processing/index.html`;
const sink = '#outputVideoContainer video.sinkVideo';

async function start(transform, output) {
  await waitFor(driver, 'document.readyState === "complete" && !document.getElementById("sourceSelector").disabled', 'the controls never became usable');
  await requireFeature(driver, 'typeof MediaStreamTrackProcessor !== "undefined" && typeof MediaStreamTrackGenerator !== "undefined"',
      'MediaStreamTrackProcessor and MediaStreamTrackGenerator on the main thread');
  await pick(driver, '#transformSelector', transform, 'input');
  await pick(driver, '#sinkSelector', output, 'input');
  await pick(driver, '#sourceSelector', 'camera', 'input');
  await waitFor(driver, 'pipeline && pipeline.processedStream_ instanceof MediaStream && debug.generator && debug.generator.readyState === "live"',
      'the processing pipeline never produced a live track', {detail: '({pipeline: !!pipeline, generator: debug.generator && debug.generator.readyState})'});
  await waitFor(driver, `(() => { const video = document.querySelector(${JSON.stringify(sink)}); return !!video && video.readyState >= 2 && video.videoWidth > 0; })()`,
      'the processed video never played', {detail: `(() => { const video = document.querySelector(${JSON.stringify(sink)}); return video && [video.readyState, video.videoWidth]; })()`});
}

describe('insertable streams video processing', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('runs the camera through WebGL into a video element', async () => {
    await start('webgl', 'video');
  });

  it('runs the camera through Canvas2D and a peer connection', async () => {
    await start('canvas2d', 'pc');
    await waitFor(driver, '["connected", "completed"].includes(pipeline.sink_.pipe_.callee_.iceConnectionState)', 'the sink peer connection never connected');
    const border = await read(driver, pixels(sink, {x: 2, y: 2, width: 6, height: 6}));
    expect(Math.max(...border.mean)).toBeLessThan(40);
  });
});

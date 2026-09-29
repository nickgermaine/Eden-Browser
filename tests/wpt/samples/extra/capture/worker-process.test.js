'use strict';
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {media, pixels, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/capture/worker-process/index.html`;

describe('capture worker processing', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('round-trips frames through a worker with transferred streams', async () => {
    await waitFor(driver, 'inputStream !== null && source.videoWidth > 0', 'the camera never reached the source video', {detail: media('#source')});
    await waitFor(driver, 'reader !== null', 'the worker never returned a stream', {detail: '({input: inputStream !== null, transform: transformStream !== null, writer: writer !== null})'});
    await waitFor(driver, 'canvasOut.width > 0 && result.readyState >= 2 && result.videoWidth === source.videoWidth',
        'the processed frames never reached the result video', {detail: media('#result')});
    await waitFor(driver, `(() => { const stats = ${pixels('#result', {x: 45, y: 45, width: 10, height: 10})}; return stats && stats.mean[0] > 200 && stats.mean[1] < 60 && stats.mean[2] < 60; })()`,
        'the red square drawn before the worker never reached the result video', {detail: pixels('#result', {x: 45, y: 45, width: 10, height: 10})});
  });
});

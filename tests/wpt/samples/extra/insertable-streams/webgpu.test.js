'use strict';
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {pick, read, requireFeature, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/insertable-streams/webgpu/index.html`;

describe('insertable streams webgpu', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('renders the camera and a video through WebGPU', async () => {
    await requireFeature(driver, '!!navigator.gpu', 'WebGPU');
    await requireFeature(driver, 'typeof MediaStreamTrackProcessor !== "undefined" && typeof MediaStreamTrackGenerator !== "undefined"',
        'MediaStreamTrackProcessor and MediaStreamTrackGenerator on the main thread');
    await pick(driver, '#sourceSelector', 'main', 'input');
    await waitFor(driver, '!!document.querySelector("#outputVideo canvas") && typeof gpuTransform !== "undefined" && !!gpuTransform && !!gpuTransform.device_',
        'WebGPU never started rendering', {detail: 'document.getElementById("errorMsg").textContent'});
    await driver.sleep(2000);
    expect(await read(driver, 'document.getElementById("errorMsg").textContent.trim()')).toBe('');
  });
});

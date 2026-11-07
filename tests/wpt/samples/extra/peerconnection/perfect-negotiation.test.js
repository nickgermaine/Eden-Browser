'use strict';
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {read, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/peerconnection/perfect-negotiation/index.html`;

function remote(frame) {
  return `(() => {
    const video = document.getElementById(${JSON.stringify(frame)}).contentDocument.getElementById('remoteVideo');
    if (!video || video.readyState < 2 || video.videoWidth !== 32) {
      return null;
    }
    const canvas = Object.assign(document.createElement('canvas'), {width: 32, height: 32});
    const context = canvas.getContext('2d', {willReadFrequently: true});
    context.drawImage(video, 0, 0);
    const data = context.getImageData(0, 0, 32, 32).data;
    const sums = [0, 0, 0];
    for (let index = 0; index < data.length; index += 4) {
      sums[0] += data[index];
      sums[1] += data[index + 1];
      sums[2] += data[index + 2];
    }
    return sums.map(sum => sum / (data.length / 4));
  })()`;
}

async function expectColor(frame, test, message) {
  await waitFor(driver, `(() => { const mean = ${remote(frame)}; return mean && (${test}); })()`, message, {detail: remote(frame)});
}

describe('peerconnection perfect negotiation', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('renegotiates both ways without errors', async () => {
    await driver.executeScript('window.__errors = []; window.addEventListener("message", event => event.data && event.data.error && window.__errors.push(event.data.error));');
    await waitFor(driver, '["polite", "impolite"].every(name => { const frame = document.getElementById(name); return frame.contentDocument && frame.contentDocument.getElementById(name + "Start"); })',
        'the peers never loaded');
    await driver.executeScript('for (const name of ["polite", "impolite"]) document.getElementById(name).contentDocument.getElementById(name + "Start").click();');
    await read(driver, 'window.run(document.getElementById("polite").contentWindow, "swapTransceivers").then(() => true)');
    await expectColor('impolite', 'mean[1] > 60 && mean[0] < 30 && mean[2] < 30', 'the polite peer\'s green video never reached the impolite peer');
    await read(driver, 'window.run(document.getElementById("impolite").contentWindow, "swapTransceivers").then(() => true)');
    await expectColor('polite', 'mean[0] > 60 && mean[1] < 30 && mean[2] < 30', 'the impolite peer\'s red video never reached the polite peer');
    await read(driver, 'window.run(document.getElementById("polite").contentWindow, "swapTransceivers").then(() => true)');
    await expectColor('impolite', 'mean[1] > 60 && mean[2] > 60 && mean[0] < 30', 'the polite peer\'s cyan video never replaced the green one');
    expect(await read(driver, 'window.__errors')).toEqual([]);
  });
});

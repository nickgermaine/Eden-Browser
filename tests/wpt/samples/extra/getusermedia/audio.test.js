'use strict';
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {read, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/getusermedia/audio/index.html`;

describe('getusermedia audio', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('plays the microphone through an audio element', async () => {
    await waitFor(driver, 'window.stream && window.stream.getAudioTracks()[0].readyState === "live"', 'getUserMedia never delivered a live microphone',
        {detail: 'document.getElementById("errorMsg").textContent'});
    await waitFor(driver, 'document.getElementById("gum-local").srcObject === window.stream && !document.getElementById("gum-local").paused', 'the audio element never played the microphone',
        {detail: '({paused: document.getElementById("gum-local").paused, time: document.getElementById("gum-local").currentTime})'});
    const before = await read(driver, 'document.getElementById("gum-local").currentTime');
    await driver.sleep(1000);
    expect(await read(driver, 'document.getElementById("gum-local").currentTime')).toBeGreaterThan(before);
    expect(await read(driver, 'document.getElementById("errorMsg").textContent.trim()')).toBe('');
  });

  it('delivers sound from the microphone', async () => {
    await waitFor(driver, 'window.stream && window.stream.getAudioTracks()[0].readyState === "live"', 'getUserMedia never delivered a live microphone');
    const level = await driver.executeAsyncScript((done) => {
      const context = new AudioContext();
      const analyser = context.createAnalyser();
      context.createMediaStreamSource(window.stream).connect(analyser);
      const samples = new Float32Array(analyser.fftSize);
      let peak = 0;
      const started = performance.now();
      const measure = () => {
        analyser.getFloatTimeDomainData(samples);
        for (const sample of samples) {
          peak = Math.max(peak, Math.abs(sample));
        }
        if (peak > 0.001 || performance.now() - started > 5000) {
          done({peak, state: context.state});
          context.close();
        } else {
          setTimeout(measure, 50);
        }
      };
      context.resume().then(measure, measure);
    });
    expect(level.state).toBe('running');
    expect(level.peak).toBeGreaterThan(0.001);
  });
});

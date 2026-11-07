'use strict';
const webdriver = require('selenium-webdriver');
const seleniumHelpers = require('../../../../../test/webdriver');

let driver;
const url = `${process.env.BASEURL}/src/content/automation/host/index.html`;

describe('automation host', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('clicks are trusted and grant user activation', async () => {
    await driver.findElement(webdriver.By.id('startButton')).click();
    await driver.wait(() => driver.executeScript(() => clickTrusted !== null), 10000);
    expect(await driver.executeScript(() => clickTrusted)).toBe(true);
    expect(await driver.executeScript(() => clickActivated)).toBe(true);
  });

  it('display capture is granted with a fake screen', async () => {
    await driver.findElement(webdriver.By.id('startButton')).click();
    await driver.wait(() => driver.executeScript(() => displayTrack !== null || displayError !== null), 20000);
    expect(await driver.executeScript(() => displayError)).toBeNull();
    const settings = await driver.executeScript(() => ({state: displayTrack.readyState, width: displayTrack.getSettings().width}));
    expect(settings.state).toBe('live');
    expect(settings.width).toBeGreaterThan(0);
  });

  it('camera and microphone come from fake devices', async () => {
    const devices = await driver.executeAsyncScript((done) => {
      navigator.mediaDevices.getUserMedia({audio: true, video: true}).then(stream => {
        const video = stream.getVideoTracks()[0];
        const audio = stream.getAudioTracks()[0];
        const element = document.createElement('video');
        element.muted = true;
        element.srcObject = stream;
        element.play().then(() => {
          const settle = () => {
            if (element.videoWidth > 0) {
              done({video: video.readyState, audio: audio.readyState, width: element.videoWidth});
              stream.getTracks().forEach(track => track.stop());
            } else {
              requestAnimationFrame(settle);
            }
          };
          settle();
        }, error => done({error: `${error.name}: ${error.message}`}));
      }, error => done({error: `${error.name}: ${error.message}`}));
    });
    expect(devices.error).toBeUndefined();
    expect(devices.video).toBe('live');
    expect(devices.audio).toBe('live');
    expect(devices.width).toBeGreaterThan(0);
  });

  it('typing reaches focused inputs', async () => {
    await driver.findElement(webdriver.By.id('text')).sendKeys('eden');
    expect(await driver.findElement(webdriver.By.id('text')).getAttribute('value')).toBe('eden');
  });

  it('double clicks reach elements', async () => {
    const option = await driver.findElement(webdriver.By.css('#servers>option'));
    await driver.actions({async: true}).doubleClick(option).perform();
    await driver.wait(() => driver.executeScript(() => doubleClicks > 0), 10000);
  });

  it('tabs and popups are separate windows', async () => {
    const first = await driver.getWindowHandle();
    await driver.switchTo().newWindow('tab');
    const second = await driver.getWindowHandle();
    expect(second).not.toBe(first);
    await driver.get(url);
    expect(await driver.getTitle()).toBe('Automation fixture');
    await driver.close();
    await driver.switchTo().window(first);
    const before = await driver.getAllWindowHandles();
    await driver.findElement(webdriver.By.id('popupButton')).click();
    await driver.wait(async () => (await driver.getAllWindowHandles()).length > before.length, 10000);
    expect(await driver.executeScript(() => popupOpened)).toBe(true);
    const popup = (await driver.getAllWindowHandles()).find(handle => !before.includes(handle));
    await driver.switchTo().window(popup);
    await driver.wait(() => driver.executeScript(() => document.readyState === 'complete' && document.getElementById('opener') !== null), 10000);
    expect(await driver.findElement(webdriver.By.id('opener')).getText()).toBe('has opener');
    await driver.close();
    await driver.switchTo().window(first);
  });

  it('closed windows leave the handle list at once', async () => {
    const first = await driver.getWindowHandle();
    for (let round = 0; round < 5; ++round) {
      await driver.switchTo().newWindow('tab');
      const opened = await driver.getWindowHandle();
      await driver.close();
      expect(await driver.getAllWindowHandles()).not.toContain(opened);
      await driver.switchTo().window(first);
    }
  });

  it('scripts open windows without a gesture', async () => {
    const before = await driver.getAllWindowHandles();
    expect(await driver.executeScript(() => window.open('popup.html', 'script-popup') !== null)).toBe(true);
    await driver.wait(async () => (await driver.getAllWindowHandles()).length > before.length, 10000);
    const popup = (await driver.getAllWindowHandles()).find(handle => !before.includes(handle));
    await driver.switchTo().window(popup);
    await driver.wait(() => driver.executeScript(() => document.readyState === 'complete' && document.getElementById('opener') !== null), 10000);
    expect(await driver.findElement(webdriver.By.id('opener')).getText()).toBe('has opener');
    await driver.close();
    await driver.switchTo().window(before[0]);
  });

  it('alerts reach WebDriver', async () => {
    await driver.findElement(webdriver.By.id('alertButton')).click();
    const dialog = await driver.switchTo().alert();
    expect(await dialog.getText()).toBe('automation alert');
    await dialog.accept();
    await driver.wait(() => driver.executeScript(() => alertReturned), 10000);
  });

  it('a loopback call connects and carries data', async () => {
    const outcome = await driver.executeAsyncScript((done) => {
      const caller = new RTCPeerConnection();
      const callee = new RTCPeerConnection();
      caller.onicecandidate = event => event.candidate && callee.addIceCandidate(event.candidate);
      callee.onicecandidate = event => event.candidate && caller.addIceCandidate(event.candidate);
      const channel = caller.createDataChannel('check');
      callee.ondatachannel = event => {
        event.channel.onmessage = message => done({state: caller.connectionState, message: message.data});
      };
      channel.onopen = () => channel.send('hello');
      caller.createOffer()
          .then(offer => caller.setLocalDescription(offer))
          .then(() => callee.setRemoteDescription(caller.localDescription))
          .then(() => callee.createAnswer())
          .then(answer => callee.setLocalDescription(answer))
          .then(() => caller.setRemoteDescription(callee.localDescription))
          .catch(error => done({error: `${error.name}: ${error.message}`}));
    });
    expect(outcome.error).toBeUndefined();
    expect(outcome.message).toBe('hello');
    expect(outcome.state).toBe('connected');
  });
});

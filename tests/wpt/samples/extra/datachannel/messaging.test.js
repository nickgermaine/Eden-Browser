'use strict';
const seleniumHelpers = require('webrtc-samples/test/webdriver');
const {read, waitFor} = require('../helpers.cjs');

let driver;
const url = `${process.env.BASEURL}/src/content/datachannel/messaging/index.html`;
const element = 'document.querySelector("messaging-sample")';

describe('datachannel messaging', () => {
  beforeAll(async () => {
    driver = await seleniumHelpers.buildDriver();
  });
  afterAll(() => driver.quit());
  beforeEach(() => driver.get(url));

  it('connects two peers and sends messages both ways', async () => {
    await waitFor(driver, `customElements.get("messaging-sample") !== undefined && ${element} && ${element}.shadowRoot && ${element}.shadowRoot.querySelectorAll("button").length >= 4`,
        'the messaging element never loaded (it imports lit-element from unpkg.com)', {timeout: 30000});
    await driver.executeScript(`${element}.shadowRoot.querySelectorAll("button")[0].click();`);
    await waitFor(driver, `window.localChannel && window.localChannel.readyState === "open" && ${element}.connected === true`, 'the data channel never opened',
        {detail: `({local: window.localChannel && window.localChannel.readyState, connected: ${element}.connected})`});
    await driver.executeScript(`const root = ${element}.shadowRoot; root.querySelector("#localOutgoing").value = "from local"; root.querySelectorAll("button")[2].click();`);
    await waitFor(driver, `${element}.remoteMessages.includes("from local")`, 'the remote peer never received the message', {detail: `${element}.remoteMessages`});
    await driver.executeScript(`const root = ${element}.shadowRoot; root.querySelector("#remoteOutgoing").value = "from remote"; root.querySelector("#sendRemote").click();`);
    await waitFor(driver, `${element}.localMessages.includes("from remote")`, 'the local peer never received the reply', {detail: `${element}.localMessages`});
    await driver.executeScript(`${element}.shadowRoot.querySelectorAll("button")[1].click();`);
    await waitFor(driver, `${element}.connected === false`, 'disconnecting never finished');
    expect(await read(driver, 'window.localChannel.readyState')).not.toBe('open');
  });
});

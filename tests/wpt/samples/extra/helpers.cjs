'use strict';

const POLL = 100;

function describe(value) {
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
}

async function evaluate(driver, expression) {
  return driver.executeScript(`
    try {
      return Promise.resolve(${expression}).then(value => ({value}), error => ({error: String(error && error.name ? error.name + ': ' + error.message : error)}));
    } catch (error) {
      return {error: error.name + ': ' + error.message};
    }`);
}

async function read(driver, expression) {
  const result = await evaluate(driver, expression);
  if (result.error) {
    throw new Error(`${expression} threw ${result.error}`);
  }
  return result.value;
}

async function waitFor(driver, expression, message, {timeout = 20000, detail = null} = {}) {
  const deadline = Date.now() + timeout;
  for (;;) {
    const result = await evaluate(driver, expression);
    if (!result.error && result.value) {
      return result.value;
    }
    if (Date.now() >= deadline) {
      const state = detail ? await evaluate(driver, detail) : result;
      throw new Error(`${message} (after ${timeout} ms: ${describe(state.error ?? state.value)})`);
    }
    await driver.sleep(POLL);
  }
}

async function requireFeature(driver, expression, name) {
  const result = await evaluate(driver, expression);
  if (result.error || !result.value) {
    throw new Error(`Missing feature: ${name}${result.error ? ` (${result.error})` : ''}`);
  }
}

function media(selector) {
  return `(() => {
    const element = document.querySelector(${JSON.stringify(selector)});
    return element && {readyState: element.readyState, width: element.videoWidth, height: element.videoHeight, paused: element.paused,
      time: element.currentTime, error: element.error && element.error.code, source: !!(element.srcObject || element.currentSrc)};
  })()`;
}

function playing(selector) {
  return `(() => {
    const element = document.querySelector(${JSON.stringify(selector)});
    return !!element && element.readyState >= 2 && element.videoWidth > 0 && !element.paused;
  })()`;
}

function pixels(selector, region) {
  return `(() => {
    const element = document.querySelector(${JSON.stringify(selector)});
    if (!element) {
      return null;
    }
    const width = element.videoWidth || element.width;
    const height = element.videoHeight || element.height;
    if (!width || !height) {
      return null;
    }
    const canvas = document.createElement('canvas');
    canvas.width = width;
    canvas.height = height;
    const context = canvas.getContext('2d', {willReadFrequently: true});
    context.drawImage(element, 0, 0, width, height);
    const area = ${JSON.stringify(region || null)} || {x: 0, y: 0, width, height};
    const data = context.getImageData(area.x, area.y, area.width, area.height).data;
    const sums = [0, 0, 0];
    let lit = 0;
    let white = 0;
    for (let index = 0; index < data.length; index += 4) {
      sums[0] += data[index];
      sums[1] += data[index + 1];
      sums[2] += data[index + 2];
      if (data[index] + data[index + 1] + data[index + 2] > 48) {
        lit += 1;
      }
      if (data[index] > 240 && data[index + 1] > 240 && data[index + 2] > 240) {
        white += 1;
      }
    }
    const count = data.length / 4;
    return {mean: sums.map(sum => sum / count), lit: lit / count, white: white / count, width, height};
  })()`;
}

function rtpStats(connection, type, kind) {
  return `(async () => {
    const report = await ${connection}.getStats();
    const entry = [...report.values()].find(stats => stats.type === ${JSON.stringify(type)} && stats.kind === ${JSON.stringify(kind)});
    return entry ? {framesDecoded: entry.framesDecoded, framesEncoded: entry.framesEncoded, bytesReceived: entry.bytesReceived,
      bytesSent: entry.bytesSent, totalAudioEnergy: entry.totalAudioEnergy, scalabilityMode: entry.scalabilityMode,
      qualityLimitationReason: entry.qualityLimitationReason, frameWidth: entry.frameWidth} : null;
  })()`;
}

async function growing(driver, expression, field, message, {interval = 1500, timeout = 15000} = {}) {
  const first = await waitFor(driver, `(async () => { const value = await ${expression}; return value && value.${field} !== undefined ? value : null; })()`, `${message}: no ${field} yet`, {timeout});
  await driver.sleep(interval);
  const second = await read(driver, expression);
  if (!(second && second[field] > first[field])) {
    throw new Error(`${message}: ${field} stayed at ${first[field]} (now ${describe(second)})`);
  }
  return second;
}

async function click(driver, webdriver, id) {
  await driver.wait(() => driver.findElement(webdriver.By.id(id)).isEnabled(), 20000, `#${id} never became enabled`);
  await driver.findElement(webdriver.By.id(id)).click();
}

async function choose(driver, webdriver, selector) {
  await driver.findElement(webdriver.By.css(selector)).click();
}

async function pick(driver, selector, value, event) {
  const chosen = await driver.executeScript((target, wanted, type) => {
    const select = document.querySelector(target);
    if (!select || ![...select.options].some(option => option.value === wanted)) {
      return false;
    }
    select.value = wanted;
    select.dispatchEvent(new Event(type, {bubbles: true}));
    return true;
  }, selector, value, event);
  if (!chosen) {
    throw new Error(`${selector} has no option ${value}`);
  }
}

module.exports = {evaluate, read, waitFor, requireFeature, media, playing, pixels, rtpStats, growing, click, choose, pick};

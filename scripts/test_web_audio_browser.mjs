// Run the real shell audio UI and AudioWorklet in a device-backed browser graph.
// No game assets are required. NODE_PATH can point to a Playwright installation;
// PSPWEB_BROWSER_EXECUTABLE optionally selects an installed Chromium browser.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { createRequire } from 'node:module';

const { chromium } = createRequire(import.meta.url)('playwright');
const html = readFileSync(new URL('../profile/web/shell.html', import.meta.url), 'utf8');
const audio = html.slice(html.indexOf('  var WORKLET = ['), html.indexOf('  var PAD_BUTTONS ='));
const harness = `<!doctype html><button id="sound">Mute</button><script>
var prefs={}, Module={}; function savePrefs() {} function releaseInput() {}
${audio}
var oscillatorPhase=0;
function feedAudio() {
  var pcm=new Int16Array(882);
  for(var i=0;i<441;i++) {var s=Math.round(Math.sin(oscillatorPhase++*2*Math.PI*440/44100)*8192);pcm[i*2]=s;pcm[i*2+1]=-s;}
  Module.pspAudio(pcm);
}
</script>`;
const server = createServer((req, res) => {
  res.writeHead(200, { 'Content-Type': 'text/html' }); res.end(harness);
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
let browser;
try {
  browser = await chromium.launch({ headless: true,
    executablePath: process.env.PSPWEB_BROWSER_EXECUTABLE || undefined,
    timeout: 15000,
    args: ['--autoplay-policy=no-user-gesture-required'] });
  const page = await browser.newPage();
  page.setDefaultTimeout(15000);
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  await page.goto(`http://127.0.0.1:${server.address().port}/`);
  // A key gesture initializes audio without toggling the mute preference.
  await page.keyboard.press('Space');
  await page.waitForFunction(() => sound.node && sound.context.state === 'running');
  await page.evaluate(() => {
    // Inspect one channel; the fixture's opposite-polarity stereo would cancel
    // in an analyser's default mono downmix.
    window.analyser = sound.context.createAnalyser();
    window.splitter = sound.context.createChannelSplitter(2);
    sound.node.connect(splitter); splitter.connect(analyser, 0);
  });
  await page.evaluate(() => { window.producer = setInterval(feedAudio, 10); });
  await page.waitForFunction(() => sound.stats && sound.stats.received > 44100 && sound.stats.output > 0);
  const active = await page.evaluate(() => ({ rate: sound.context.sampleRate, ...sound.stats }));
  assert(active.queued > 0 && active.queued <= 11025);
  await page.waitForFunction(() => {
    const pcm = new Float32Array(analyser.fftSize); analyser.getFloatTimeDomainData(pcm);
    return pcm.some(value => Math.abs(value) > 0.01);
  });
  await page.evaluate(() => clearInterval(window.producer));
  await page.waitForFunction(() => sound.stats.under >= 1 && sound.stats.queued === 0);
  const starved = await page.evaluate(() => sound.stats.under);
  await page.waitForFunction(previous => sound.stats.output > previous + sound.context.sampleRate,
    await page.evaluate(() => sound.stats.output));
  assert.equal(await page.evaluate(() => sound.stats.under), starved, 'silence must not count repeated underruns');
  await page.locator('#sound').click();
  await page.waitForFunction(() => sound.gain.gain.value === 0);
  await page.locator('#sound').click();
  await page.evaluate(() => { window.producer = setInterval(feedAudio, 10); });
  await page.waitForFunction(() => sound.stats && sound.stats.received > 44100 && sound.stats.queued > 0);
  assert.equal(await page.evaluate(() => sound.gain.gain.value), 1);
  assert.deepEqual(errors, []);
  console.log(`PASS: real AudioWorklet at ${active.rate} Hz, non-silent PCM graph, stats, starvation, mute ramp and fresh unmute`);
} finally {
  // Close keep-alive sockets explicitly so browser shutdown cannot leave this
  // local test server waiting for a connection to drain on Windows.
  server.closeAllConnections();
  server.close();
  await browser?.close();
}

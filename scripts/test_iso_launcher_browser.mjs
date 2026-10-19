// Actual shell/bootstrap on both static hosting paths, with a tiny synthetic
// engine and ISO. No large game download, game assets or existing save origin.
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { readFileSync, mkdirSync, mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { createServer } from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { config, fixture } from './iso_fixture.mjs';

const require = createRequire(import.meta.url);
const { chromium } = require('playwright');
const ROOT = fileURLToPath(new URL('../', import.meta.url));
const parent = path.join(ROOT, 'build/iso-launcher-tests');
mkdirSync(parent, { recursive: true });
const work = mkdtempSync(path.join(parent, 'browser-'));
const iso = path.join(work, 'matching.iso'), wrong = path.join(work, 'wrong.iso');
writeFileSync(iso, fixture().bytes);
writeFileSync(wrong, fixture('OTHER0000').bytes);
let shell = readFileSync(path.join(ROOT, 'profile/web/shell.html'), 'utf8');
shell = shell.replace('<head>', '<head><script>window.PSPWEB_LOCAL_ISO=' + JSON.stringify({ ...config, launcher: 'iso-only-v1' }) + ';document.documentElement.classList.add("iso-launcher");</script>');
shell = shell.replace('{{{ SCRIPT }}}', '<script src="static_bootstrap.js" data-runtime="index.js"></script>');
const runtime = `
(async () => {
  const dependencies = new Set();
  globalThis.ENV = {};
  globalThis.IDBFS = {};
  globalThis.addRunDependency = name => dependencies.add(name);
  globalThis.removeRunDependency = name => dependencies.delete(name);
  globalThis.FS = { mkdirTree() {}, mount() {}, writeFile() {}, syncfs(_, done) { done(null); } };
  for (const run of Module.preRun) run();
  await new Promise(resolve => setTimeout(resolve, 0));
  if (dependencies.size) throw Error('startup gated: ' + [...dependencies]);
  if (!crossOriginIsolated || typeof SharedArrayBuffer !== 'function') throw Error('not isolated');
  const bytes = await (await fetch('index.wasm')).arrayBuffer();
  await WebAssembly.compile(bytes);
  const result = new Uint8Array(await Module.pspReadDisc('disc/PSP_GAME/DATA.BIN', 3, 8));
  if (result.length !== 6) throw Error('range bridge failed');
  Module.onRuntimeInitialized();
  localStorage.setItem('launcher-save-marker', 'preserved');
  document.title = 'READY ' + Module.pspLocalDisc.identity.discId;
})().catch(error => { document.title = 'FAIL ' + error.message; });`;
const assets = {
  'index.html': shell, 'index.js': runtime,
  'index.wasm': Buffer.from([0, 97, 115, 109, 1, 0, 0, 0]),
  'local_iso.js': readFileSync(path.join(ROOT, 'profile/web/local_iso.js')),
  'static_bootstrap.js': readFileSync(path.join(ROOT, 'profile/web/static_bootstrap.js')),
  'coi-serviceworker.js': readFileSync(path.join(ROOT, 'profile/third_party/coi_serviceworker/coi-serviceworker.js')),
};
const requests = [];
const server = createServer((request, response) => {
  const url = new URL(request.url, 'http://test');
  requests.push(url.pathname);
  const name = url.pathname.split('/').at(-1) || 'index.html';
  if (url.pathname.startsWith('/headers/')) {
    response.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    response.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    response.setHeader('Cross-Origin-Resource-Policy', 'same-origin');
  }
  const type = name.endsWith('.js') ? 'text/javascript' : name.endsWith('.wasm') ? 'application/wasm' : 'text/html';
  response.writeHead(Object.hasOwn(assets, name) ? 200 : 404, { 'Content-Type': type });
  response.end(assets[name] || 'not found');
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const origin = `http://127.0.0.1:${server.address().port}`;
let browser;
try {
  browser = await chromium.launch(process.env.PSPWEB_BROWSER_EXECUTABLE ? { executablePath: process.env.PSPWEB_BROWSER_EXECUTABLE } : {});
  for (const route of ['/headers/', '/plain/ncaa/', '/headers/mobile/']) {
    const context = await browser.newContext(route.includes('mobile') ? {viewport: {width: 390, height: 844}, hasTouch: true} : {});
    const page = await context.newPage();
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    await page.goto(origin + route);
    await page.waitForFunction(() => document.getElementById('status').textContent === 'Choose your NCAA ISO');
    assert.equal(await page.evaluate(() => crossOriginIsolated), true);
    assert.equal(await page.isVisible('.screen'), false, 'hide the inactive game screen before startup');
    assert.ok(!requests.some(url => url === route + 'index.js'), 'engine must wait for a verified ISO');
    await page.focus('#disc-file');
    const chooser = page.waitForEvent('filechooser');
    await page.keyboard.press('Enter');
    await (await chooser).setFiles(wrong);
    await page.waitForFunction(() => document.getElementById('disc-message').className === 'disc-error');
    assert.ok(!requests.some(url => url === route + 'index.js'), 'wrong ISO must not download the engine');
    assert.equal(await page.isDisabled('#disc-file'), false);
    await page.setInputFiles('#disc-file', iso);
    await page.waitForFunction(() => /^(READY|FAIL) /.test(document.title));
    assert.equal(await page.title(), 'READY ULUS10440');
    assert.equal(await page.isVisible('.screen'), true);
    assert.equal(await page.isDisabled('#disc-file'), true);
    assert.equal(await page.evaluate(() => Module.pspLocalDisc.stats.gameReads), 1);
    await page.reload();
    await page.waitForFunction(() => document.getElementById('status').textContent === 'Choose your NCAA ISO');
    assert.equal(await page.evaluate(() => localStorage.getItem('launcher-save-marker')), 'preserved');
    assert.equal(await page.evaluate(() => Module.pspReady), false);
    assert.deepEqual(errors, []);
    await context.close();
  }
  assert.ok(!requests.some(url => /\.iso$|\/disc\//i.test(url)), 'no ISO or disc upload/request');
  console.log('PASS: unified ISO-only shell, header/plain isolation, subdirectory reload, wrong-disc retry, engine gating, range bridge and storage preservation');
} finally {
  if (browser) await browser.close();
  await new Promise(resolve => server.close(resolve));
  rmSync(work, { recursive: true, force: true });
}

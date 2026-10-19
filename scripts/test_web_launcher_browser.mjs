// End-to-end launcher check in Chromium on a plain static server without isolation
// headers: import a synthetic package, launch it isolated from Cache Storage and pick
// a synthetic ISO through the package's real local ISO picker. No game data is used.
import assert from 'node:assert/strict';
import { execSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { config, fixture } from './iso_fixture.mjs';

async function loadPlaywright() {
  try { return await import('playwright'); } catch (_) {}
  try { return createRequire(import.meta.url)('playwright'); } catch (_) {}
  return createRequire(path.join(execSync('npm root -g').toString().trim(), 'noop.js'))('playwright');
}
const { chromium } = await loadPlaywright();

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
mkdirSync(path.join(ROOT, 'build/launcher-browser-tests'), { recursive: true });
const work = mkdtempSync(path.join(ROOT, 'build/launcher-browser-tests/run-'));
const site = path.join(work, 'site'), pack = path.join(work, 'static-ncaa10'), tampered = path.join(work, 'tampered');
// Mirror a GitHub Pages project site, which serves the launcher below a repository path.
const PLAY = '/NCAA-10-PSP-Recomp/play/';
cpSync(path.join(ROOT, 'profile/web/launcher'), path.join(site, PLAY), { recursive: true });

const runtime = `if (typeof document === 'undefined') {
  postMessage({ isolated: self.crossOriginIsolated });
} else (async () => {
  const result = { isolated: crossOriginIsolated, shared: typeof SharedArrayBuffer === 'function' };
  await WebAssembly.instantiateStreaming(fetch('index.wasm'));
  result.wasm = true;
  result.data = await (await fetch('index.data')).text();
  result.worker = await new Promise(resolve => { new Worker('index.js').onmessage = event => resolve(event.data.isolated); });
  const Module = {};
  PspLocalIso.install({ Module, FS: { mkdirTree() {}, writeFile() {} }, addRunDependency() {},
    removeRunDependency() { result.disc = Module.pspLocalDisc.identity.discId; document.title = 'READY ' + JSON.stringify(result); },
    document, config: window.PSPWEB_LOCAL_ISO });
  document.title = 'WAITING ' + JSON.stringify(result);
})().catch(error => { document.title = 'FAIL ' + error.message; });
`;
const files = {
  'index.html': `<!doctype html><head><script>window.PSPWEB_LOCAL_ISO = ${JSON.stringify(config)};</script><title>synthetic</title></head><body>
<section id="disc-loader" hidden><input id="disc-file" type="file"><p id="disc-message"></p></section><span id="status"></span>
<script src="local_iso.js"></script><script src="static_bootstrap.js" data-runtime="index.js"></script></body>`,
  'index.js': runtime,
  'index.wasm': Buffer.from([0, 97, 115, 109, 1, 0, 0, 0]),
  'index.data': 'synthetic boot bytes',
  'local_iso.js': readFileSync(path.join(ROOT, 'profile/web/local_iso.js')),
  'static_bootstrap.js': readFileSync(path.join(ROOT, 'profile/web/static_bootstrap.js')),
  'licenses/project-MIT.txt': readFileSync(path.join(ROOT, 'LICENSE')),
};
function writePackage(directory, contents) {
  const manifest = [];
  for (const [name, value] of Object.entries(contents)) {
    mkdirSync(path.dirname(path.join(directory, name)), { recursive: true });
    writeFileSync(path.join(directory, name), value);
    manifest.push({ path: name, bytes: Buffer.byteLength(value), sha256: createHash('sha256').update(value).digest('hex') });
  }
  writeFileSync(path.join(directory, 'static-manifest.json'), JSON.stringify({ format: 1, discId: 'ULUS10440', files: manifest }));
}
writePackage(pack, files);
writeFileSync(path.join(pack, 'private.iso'), 'never stored');
writePackage(tampered, files);
writeFileSync(path.join(tampered, 'index.data'), 'synthetic boot bytez');
const iso = path.join(work, 'NCAA-test.iso');
writeFileSync(iso, fixture().bytes);

const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm' };
const server = createServer((request, response) => {
  const name = decodeURIComponent(new URL(request.url, 'http://x').pathname);
  const file = path.join(site, name.endsWith('/') ? name + 'index.html' : name);
  // Like GitHub Pages, redirect a directory requested without its trailing slash.
  if (!name.endsWith('/') && existsSync(file) && statSync(file).isDirectory()) {
    response.writeHead(301, { Location: name + '/' });
    response.end();
    return;
  }
  try {
    const body = readFileSync(file);
    response.writeHead(200, { 'Content-Type': types[path.extname(file)] || 'application/octet-stream' });
    response.end(body);
  } catch (_) { response.writeHead(404); response.end('not found'); }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const origin = `http://127.0.0.1:${server.address().port}`;

const browser = await chromium.launch(process.env.PSPWEB_BROWSER_EXECUTABLE ? { executablePath: process.env.PSPWEB_BROWSER_EXECUTABLE } : {});
try {
  const page = await browser.newPage();
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  await page.goto(origin + PLAY);
  await page.waitForFunction(() => /Browser support is ready/.test(document.getElementById('support').textContent));
  assert.equal(await page.isDisabled('#launch'), true);

  await page.setInputFiles('#build-folder', tampered);
  await page.waitForFunction(() => document.getElementById('build-message').className === 'error');
  assert.match(await page.textContent('#build-message'), /index\.data does not match static-manifest\.json \(SHA-256\)/);
  assert.equal(await page.isDisabled('#launch'), true);

  await page.setInputFiles('#build-folder', pack);
  await page.waitForFunction(() => /imported and verified/.test(document.getElementById('build-message').textContent), null, { timeout: 30000 });
  assert.match(await page.textContent('#build-message'), /Ignored 1 file/);
  assert.match(await page.textContent('#build-summary'), /^7 files, /);
  await page.waitForFunction(() => !document.getElementById('launch').disabled);

  async function launched() {
    await page.waitForFunction(() => /^(WAITING|FAIL) /.test(document.title), null, { timeout: 30000 });
    const title = await page.title();
    assert.ok(title.startsWith('WAITING '), title);
    return JSON.parse(title.slice(8));
  }
  await page.click('#launch');
  const state = await launched();
  assert.deepEqual(state, { isolated: true, shared: true, wasm: true, data: 'synthetic boot bytes', worker: true });
  assert.equal(new URL(page.url()).pathname, PLAY + 'game/');
  assert.equal(await page.evaluate(async () => (await fetch('private.iso')).status), 404);

  await page.setInputFiles('#disc-file', iso);
  await page.waitForFunction(() => /^READY /.test(document.title), null, { timeout: 30000 });
  assert.equal(JSON.parse((await page.title()).slice(6)).disc, 'ULUS10440');

  await page.reload();
  assert.equal((await launched()).isolated, true, 'the imported build survives a reload');

  // Redirected navigations inside the worker's scope must pass through unchanged.
  await page.goto(origin + PLAY + 'game');
  assert.equal(new URL(page.url()).pathname, PLAY + 'game/');
  assert.equal((await launched()).isolated, true);

  await page.goto(origin + PLAY);
  await page.waitForFunction(() => !document.getElementById('remove').disabled);
  await page.click('#remove');
  await page.waitForFunction(() => /removed/.test(document.getElementById('build-message').textContent));
  await page.goto(origin + PLAY + 'game/');
  await page.waitForURL(origin + PLAY);
  assert.deepEqual(errors, []);
  console.log('PASS: launcher import/tamper rejection, isolated cached launch, Wasm/data/worker, local ISO picker, reload and removal');
} finally {
  await browser.close();
  server.close();
  rmSync(work, { recursive: true, force: true });
}

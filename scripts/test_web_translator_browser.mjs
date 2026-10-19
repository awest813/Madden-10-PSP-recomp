// Translator page in Chromium on a plain static server: the real page rejects a
// synthetic ISO whose executable is not the verified NCAA build, and a test-only
// worker drives the same scripts and Wasm module to a complete ZIP with a
// synthetic expected hash. Needs build/translator-web and build/translator-native.
import assert from 'node:assert/strict';
import { execFileSync, execSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { cpSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { isoImage, sfo } from './iso_fixture.mjs';
import { pspElf } from './psp_elf_fixture.mjs';

async function loadPlaywright() {
  try { return await import('playwright'); } catch (_) {}
  return createRequire(path.join(execSync('npm root -g').toString().trim(), 'noop.js'))('playwright');
}
const { chromium } = await loadPlaywright();
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const WEB = process.env.PSPWEB_TRANSLATOR_WEB || path.join(ROOT, 'build/translator-web');
const NATIVE = process.env.PSPWEB_TRANSLATOR_NATIVE || path.join(ROOT, 'build/translator-native');
mkdirSync(path.join(ROOT, 'build/translator-browser-tests'), { recursive: true });
const work = mkdtempSync(path.join(ROOT, 'build/translator-browser-tests/run-'));
const PLAY = '/NCAA-10-PSP-Recomp/play/';
const site = path.join(work, 'site'), translate = path.join(site, PLAY, 'translate');
cpSync(path.join(ROOT, 'profile/web/launcher'), path.join(site, PLAY), { recursive: true });
cpSync(path.join(ROOT, 'profile/web/local_iso.js'), path.join(translate, 'local_iso.js'));
for (const name of ['pspweb_translator.js', 'pspweb_translator.wasm']) cpSync(path.join(WEB, name), path.join(translate, name));

const elf = pspElf();
writeFileSync(path.join(work, 'test.elf'), elf);
execFileSync(path.join(NATIVE, 'pspweb_translator_tests'), ['--encrypt-fixture', path.join(work, 'test.elf'), path.join(work, 'EBOOT.BIN')]);
const signature = [0x27BDFFF8, 0xAFB00000, 0xAFBF0004, 0x00808021, 0x8FBF0004, 0x8FB00000, 0x03E00008, 0x27BD0008];
const bank = Buffer.alloc(0x200);
bank.write('ABKC', 0);
bank.writeUInt32LE(0x10100, 0x70); bank.writeUInt32LE(0x100, 0x74); bank.writeUInt32LE(0x120, 0x78);
Buffer.from(Uint32Array.from(signature).buffer).copy(bank, 0x100);
const isoPath = path.join(work, 'synthetic.iso');
writeFileSync(isoPath, isoImage({
  'PSP_GAME/PARAM.SFO': sfo('ULUS10440'), 'PSP_GAME/SYSDIR/EBOOT.BIN': readFileSync(path.join(work, 'EBOOT.BIN')),
  'PSP_GAME/USRDIR/data/audio/aems/test.abk': bank,
}));
const expected = createHash('sha256').update(elf).digest('hex');
writeFileSync(path.join(translate, 'test-worker.js'), `importScripts('local_iso.js', 'pspweb_translator.js', 'translator.js');
self.onmessage = async event => {
  try {
    const result = await PspTranslator.translateIso(event.data, { expectedSha256: '${expected}',
      createModule: options => createPspTranslator(options), progress() {} });
    const blob = new Blob(await PspTranslator.zip(result.files));
    self.postMessage({ paths: result.files.map(file => file.path), size: blob.size, provenance: result.provenance,
      zip: Array.from(new Uint8Array(await blob.slice(0, 4).arrayBuffer())) });
  } catch (error) { self.postMessage({ error: error.message }); }
};`);

const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm' };
const server = createServer((request, response) => {
  const name = decodeURIComponent(new URL(request.url, 'http://x').pathname);
  try {
    const body = readFileSync(path.join(site, name.endsWith('/') ? name + 'index.html' : name));
    response.writeHead(200, { 'Content-Type': types[path.extname(name)] || (name.endsWith('/') ? 'text/html' : 'application/octet-stream') });
    response.end(body);
  } catch (_) { response.writeHead(404); response.end('not found'); }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const origin = `http://127.0.0.1:${server.address().port}`;
const browser = await chromium.launch();
try {
  const page = await browser.newPage();
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  await page.goto(origin + PLAY + 'translate/');
  await page.setInputFiles('#iso', isoPath);
  await page.waitForFunction(() => document.getElementById('status').className === 'error', null, { timeout: 60000 });
  assert.match(await page.textContent('#status'), /does not match the verified NCAA build \(SHA-256 [0-9a-f]{64}\)/);
  assert.equal(await page.isHidden('#download'), true);
  assert.equal(await page.isDisabled('#iso'), false);

  await page.evaluate(() => { const input = document.createElement('input'); input.type = 'file'; input.id = 'test-iso'; document.body.append(input); });
  await page.setInputFiles('#test-iso', isoPath);
  const summary = await page.evaluate(() => new Promise(resolve => {
    const worker = new Worker('test-worker.js');
    worker.onmessage = event => resolve(event.data);
    worker.postMessage(document.getElementById('test-iso').files[0]);
  }));
  assert.equal(summary.error, undefined, summary.error);
  assert.deepEqual(summary.zip, [0x50, 0x4B, 0x03, 0x04]);
  assert.ok(summary.paths.includes('games/ncaa10/root/EBOOT.BIN'));
  assert.ok(summary.paths.includes('profile/generated/ncaa10/generated_unit_0000.cpp'));
  assert.ok(summary.paths.includes('profile/generated/ncaa10_sound/ncaa_sound_000.inc'));
  assert.equal(summary.provenance.executableSha256, expected);
  assert.equal(summary.provenance.audioPrograms, 1);
  // A missing module reports an error rather than leaving the page waiting.
  rmSync(path.join(translate, 'pspweb_translator.wasm'));
  await page.reload();
  await page.setInputFiles('#iso', isoPath);
  await page.waitForFunction(() => document.getElementById('status').className === 'error', null, { timeout: 30000 });
  assert.match(await page.textContent('#status'), /Could not load the translator \(HTTP 404\)/);
  assert.deepEqual(errors, []);
  console.log('PASS: translator page rejects an unverified executable; Chromium worker decrypts, translates and zips a synthetic ISO; a missing module is reported');
} finally {
  await browser.close();
  server.close();
  rmSync(work, { recursive: true, force: true });
}

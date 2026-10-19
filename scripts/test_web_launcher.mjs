// Static-host launcher import, verification and service-worker serving with synthetic packages.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
await import('../profile/web/launcher/launcher.js');
const { CACHE, readPackage, storePackage, storedPackage, relativePath, contentType, describe } = globalThis.PspLauncher;

const BASE = 'https://example.github.io/NCAA-10-PSP-Recomp/play/';
const wasm = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]);
const contents = {
  'index.html': '<script>window.PSPWEB_LOCAL_ISO = {};</script><script src="static_bootstrap.js" data-runtime="index.js"></script>',
  'index.js': 'synthetic runtime', 'index.wasm': wasm, 'index.data': 'synthetic boot bytes',
  'local_iso.js': 'local iso helper', 'static_bootstrap.js': 'bootstrap', 'licenses/project-MIT.txt': 'MIT',
};
const bytes = value => typeof value === 'string' ? new TextEncoder().encode(value) : value;
const hash = value => createHash('sha256').update(bytes(value)).digest('hex');
function manifest(overrides = {}) {
  const files = Object.entries(contents).map(([path, value]) => ({ path, bytes: bytes(value).length, sha256: hash(value) }));
  return JSON.stringify({ format: 1, discId: 'ULUS10440', files, ...overrides });
}
function folder(files, root = 'static-ncaa10') {
  return Object.entries(files).map(([path, value]) => {
    const file = new File([bytes(value)], path.split('/').pop());
    Object.defineProperty(file, 'webkitRelativePath', { value: root ? root + '/' + path : '' });
    return file;
  });
}
const valid = () => ({ ...contents, 'static-manifest.json': manifest() });
async function rejects(files, pattern) {
  await assert.rejects(readPackage(folder(files)), pattern);
}

class MemoryCache {
  constructor() { this.entries = new Map(); }
  async put(url, response) { this.entries.set(url, { body: new Uint8Array(await response.arrayBuffer()), headers: new Headers(response.headers) }); }
  async match(url) {
    const entry = this.entries.get(typeof url === 'string' ? url : url.url);
    return entry ? new Response(entry.body, { headers: entry.headers }) : undefined;
  }
}
class MemoryStorage {
  constructor() { this.caches = new Map(); }
  async has(name) { return this.caches.has(name); }
  async open(name) { if (!this.caches.has(name)) this.caches.set(name, new MemoryCache()); return this.caches.get(name); }
  async delete(name) { return this.caches.delete(name); }
}

// Path handling for folder and loose-file selections.
assert.equal(relativePath(folder({ 'licenses/a.txt': 'x' })[0]), 'licenses/a.txt');
assert.equal(relativePath(folder({ 'index.js': 'x' }, '')[0]), 'index.js');
assert.equal(contentType('index.wasm'), 'application/wasm');
assert.equal(contentType('index.data'), 'application/octet-stream');
assert.equal(contentType('index.html'), 'text/html; charset=utf-8');

// A valid package imports every listed file and never reads unlisted ones such as an ISO.
const progress = [];
const pack = await readPackage(folder({ ...valid(), 'NCAA.iso': 'private disc' }), text => progress.push(text));
assert.deepEqual(pack.entries.map(entry => entry.path).sort(), Object.keys(contents).sort());
assert.deepEqual(pack.ignored, ['NCAA.iso']);
assert.ok(progress.includes('Verifying index.wasm…'));
// Loose files lose their folders, so optional licenses/ entries are skipped rather than misplaced.
const loose = await readPackage(folder(valid(), ''));
assert.equal(loose.entries.length, Object.keys(contents).length - 1);
assert.deepEqual(loose.ignored, ['project-MIT.txt']);
const { 'licenses/project-MIT.txt': _, ...withoutOptional } = valid();
assert.equal((await readPackage(folder(withoutOptional))).entries.length, Object.keys(contents).length - 1);

// Rejections: missing or invalid manifest, wrong title, tampering, missing required files, raw builds.
await rejects({ ...contents }, /static-manifest\.json is missing/);
await rejects({ ...valid(), 'static-manifest.json': '{' }, /not valid JSON/);
await rejects({ ...valid(), 'static-manifest.json': manifest({ format: 2 }) }, /Unsupported/);
await rejects({ ...valid(), 'static-manifest.json': manifest({ discId: 'UCUS98653' }) }, /ULUS10440/);
await rejects({ ...valid(), 'index.data': 'synthetic boot bytez' }, /index\.data does not match static-manifest\.json \(SHA-256\)/);
await rejects({ ...valid(), 'index.js': 'longer synthetic runtime' }, /index\.js does not match static-manifest\.json \(size\)/);
const { 'index.wasm': __, ...withoutWasm } = valid();
await rejects(withoutWasm, /missing index\.wasm/);
for (const path of ['../escape', '/absolute', 'a\\b', 'a//b']) {
  const files = [...JSON.parse(manifest()).files, { path, bytes: 1, sha256: hash('x') }];
  await rejects({ ...valid(), 'static-manifest.json': JSON.stringify({ format: 1, discId: 'ULUS10440', files }) }, /invalid or duplicate/);
}
const unlisted = JSON.parse(manifest());
unlisted.files = unlisted.files.filter(item => item.path !== 'local_iso.js');
await rejects({ ...valid(), 'static-manifest.json': JSON.stringify(unlisted) }, /does not list local_iso\.js/);
for (const [path, value, pattern] of [['index.wasm', 'notwasm!', /not a WebAssembly module/],
  ['index.html', '<script src="index.js"></script>', /not a local-ISO static package/]]) {
  const changed = { ...contents, [path]: value };
  const files = Object.entries(changed).map(([name, data]) => ({ path: name, bytes: bytes(data).length, sha256: hash(data) }));
  await rejects({ ...changed, 'static-manifest.json': JSON.stringify({ format: 1, discId: 'ULUS10440', files }) }, pattern);
}
await assert.rejects(readPackage([...folder(valid()), ...folder({ 'index.js': 'x' })]), /twice/);

// Storage writes the launch record last; a missing file makes the build unlaunchable.
const storage = new MemoryStorage();
assert.equal(await storedPackage(storage, BASE), null);
const record = await storePackage(storage, BASE, pack);
assert.equal(record.files.length, Object.keys(contents).length);
const cache = storage.caches.get(CACHE);
assert.deepEqual(Array.from(cache.entries.keys()).at(-1), BASE + 'launcher-build.json');
assert.ok(!Array.from(cache.entries.keys()).some(url => url.endsWith('.iso')));
const stored = await cache.match(BASE + 'game/index.wasm');
assert.equal(stored.headers.get('Content-Type'), 'application/wasm');
assert.deepEqual(new Uint8Array(await stored.arrayBuffer()), wasm);
assert.equal((await storedPackage(storage, BASE)).importedAt, record.importedAt);
assert.match(describe(record), /^7 files, 1 KiB · index\.wasm [0-9a-f]{12} · imported /);
cache.entries.delete(BASE + 'game/index.data');
assert.equal(await storedPackage(storage, BASE), null);
await storePackage(storage, BASE, loose);
assert.ok(await storedPackage(storage, BASE));

// Service worker: cached game files with isolation headers, 404s for missing files,
// the published stub only for game/index.html, and isolated pass-through elsewhere.
const network = [];
let fetchHandler, redirect = null;
const worker = {
  registration: { scope: BASE }, location: { origin: new URL(BASE).origin },
  addEventListener(type, handler) { if (type === 'fetch') fetchHandler = handler; },
  skipWaiting() {}, clients: { claim() {} },
};
vm.runInNewContext(readFileSync(new URL('../profile/web/launcher/launcher-sw.js', import.meta.url), 'utf8'), {
  self: worker, caches: storage, URL, Headers, Response,
  fetch: async url => {
    network.push(String(url.url || url));
    return redirect ? redirect : new Response('network', { headers: { 'Content-Type': 'text/html' } });
  },
});
async function request(url, method = 'GET') {
  let response = null;
  fetchHandler({ request: { url, method }, respondWith(promise) { response = promise; } });
  return response && await response;
}
for (const url of [BASE + 'game/', BASE + 'game/index.html?x=1']) {
  const response = await request(url);
  assert.equal(response.headers.get('Cross-Origin-Opener-Policy'), 'same-origin');
  assert.equal(response.headers.get('Cross-Origin-Embedder-Policy'), 'require-corp');
  assert.match(await response.text(), /PSPWEB_LOCAL_ISO/);
}
const served = await request(BASE + 'game/index.wasm');
assert.equal(served.headers.get('Content-Type'), 'application/wasm');
assert.equal(served.headers.get('Cross-Origin-Resource-Policy'), 'same-origin');
assert.equal((await request(BASE + 'game/NCAA.iso')).status, 404);
assert.equal((await request(BASE + 'game/coi-serviceworker.js')).status, 404);
assert.deepEqual(network, []);
const launcherPage = await request(BASE);
assert.equal(launcherPage.headers.get('Cross-Origin-Embedder-Policy'), 'require-corp');
assert.deepEqual(network, [BASE]);
assert.equal(await request('https://fonts.example.com/font.woff2'), null);
// A navigation's opaque redirect (status 0) passes through untouched.
redirect = { type: 'opaqueredirect', status: 0, headers: new Headers() };
assert.equal(await request(BASE + 'translate'), redirect);
redirect = null;
await storage.delete(CACHE);
assert.equal(await (await request(BASE + 'game/')).text(), 'network');
assert.equal(network.at(-1), BASE + 'game/index.html');
assert.equal((await request(BASE + 'game/index.js')).status, 404);
console.log('launcher tests passed');

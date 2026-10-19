// Real local ISO parser, startup gate and Wasm disc bridge with synthetic data.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import { boot, config, fixture, record } from './iso_fixture.mjs';
await import('../profile/web/local_iso.js');
const { IsoDisc, prepare, install } = globalThis.PspLocalIso;
const sample = fixture();
const progress = [];
const disc = await (await IsoDisc.index(sample.file,stage=>progress.push(stage))).verify(config,stage=>progress.push(stage));
assert.equal(progress.length,3); assert.match(progress[0],/index/); assert.match(progress[1],/title/); assert.match(progress[2],/revision/);
assert.equal(disc.identity.discId, 'ULUS10440');
assert.equal(disc.entries.size, 6);
assert.match(disc.manifest(), /F \d+ PSP_GAME\/DATA.BIN/);
assert.match(disc.manifest(), /F \d+ __umd.iso/);
assert.deepEqual(new Uint8Array(await disc.read('disc/psp_game/data.bin', 256 * 1024 - 3, 256 * 1024 + 7)), sample.payload.slice(256 * 1024 - 3, 256 * 1024 + 8));
assert.deepEqual(new Uint8Array(await disc.read('disc/__umd.iso', 17, 100)), sample.bytes.slice(17, 101));
const final = sample.payload.length - 1;
assert.deepEqual(new Uint8Array(await disc.read('disc/PSP_GAME/DATA.BIN', final - 8, final)), sample.payload.slice(final - 8));
await assert.rejects(disc.read('disc/PSP_GAME/DATA.BIN', 0, sample.payload.length), /Invalid range/);
await assert.rejects(disc.read('https://remote/disc/PSP_GAME/DATA.BIN', 0, 1), /Invalid local/);
await assert.rejects(disc.verify({ ebootHashes: ['wrong revision'] }), /revision/);
await assert.rejects((await IsoDisc.index(fixture('OTHER0000').file)).verify(config), /ULUS10440/);
await assert.rejects(IsoDisc.index(new File([new Uint8Array(40000)], 'wrong.cso')), /ISO9660/);
await assert.rejects(IsoDisc.index(new File([sample.bytes.slice(0, 24 * 2048)], 'truncated.iso')), /Truncated/);
const cycle = sample.bytes.slice();
new DataView(cycle.buffer).setUint32(20 * 2048 + 34 + 34 + 2, 20, true);
await assert.rejects(IsoDisc.index(new File([cycle], 'cycle.iso')), /directory tree/);
const interleaved = sample.bytes.slice();
interleaved[22 * 2048 + 26] = 1;
await assert.rejects(IsoDisc.index(new File([interleaved], 'interleaved.iso')), /unsupported/);
const crossed = sample.bytes.slice();
new DataView(crossed.buffer).setUint32(16 * 2048 + 166, 4096, true);
crossed.fill(0, 20 * 2048, 22 * 2048);
for (let i = 0; i < 59; ++i) crossed.set(record(0, 20, 4096, true), 20 * 2048 + i * 34);
crossed.set(record('CROSSING.BIN', 24, boot.length), 20 * 2048 + 59 * 34);
await assert.rejects(IsoDisc.index(new File([crossed], 'cross-sector.iso')), /directory record/);
const indexedRanges = [];
const oversized = {size: 96 * 1024 * 1024, slice(start, stop) {
  indexedRanges.push(stop - start);
  return {async arrayBuffer() {
    const bytes = new Uint8Array(stop - start);
    if (start === 16 * 2048) {
      bytes.set(sample.bytes.slice(start, stop));
      new DataView(bytes.buffer).setUint32(166, 16 * 1024 * 1024, true);
    } else if (start === 20 * 2048) {
      bytes.set(record('LARGE', 10000, 16 * 1024 * 1024, true));
    }
    return bytes.buffer;
  }};
}};
await assert.rejects(IsoDisc.index(oversized), /excessively large/);
assert.deepEqual(indexedRanges, [2048, 16 * 1024 * 1024], 'enforce the index budget before reading another large directory');

class Element {
  hidden = true; disabled = false; value = ''; textContent = ''; className = ''; files = []; handlers = {};
  addEventListener(name, fn) { this.handlers[name] = fn; }
}
const elements = new Map(['disc-loader', 'disc-file', 'disc-message', 'status'].map(id => [id, new Element()]));
const Module = {}, written = new Map();
let dependencies = 0;
install({ Module, FS: { mkdirTree() {}, writeFile(path, bytes) { written.set(path, bytes); } },
  addRunDependency() { ++dependencies; }, removeRunDependency() { --dependencies; },
  document: { getElementById(id) { return elements.get(id); } }, config });
assert.equal(dependencies, 1, 'do not boot before a verified ISO is chosen');
const input = elements.get('disc-file');
input.files = [fixture('OTHER0000').file]; await input.handlers.change();
assert.equal(dependencies, 1); assert.equal(input.disabled, false); assert.equal(Module.pspReadDisc, undefined);
input.files = [sample.file];
const retrySelection = input.handlers.change();
assert.match(elements.get('status').textContent,/Checking local NCAA ISO/,'retry must replace the earlier error status immediately');
await retrySelection;
assert.equal(dependencies, 0); assert.equal(input.disabled, true);
assert.match(written.get('/game/local-disc.manifest'), /__umd.iso/);
await input.handlers.change(); assert.equal(dependencies, 0, 'startup dependency is released once');

// The unified launcher verifies once before loading the engine. preRun must
// consume that same File/index, without asking again or reading the ISO twice.
const preflightElements = new Map(['disc-loader', 'disc-file', 'disc-message', 'status'].map(id => [id, new Element()]));
const preflightDocument = { getElementById(id) { return preflightElements.get(id); } };
const ready = prepare({ document: preflightDocument, config });
assert.equal(prepare({ document: preflightDocument, config }), ready);
const preflightInput = preflightElements.get('disc-file');
preflightInput.files = [sample.file];
await preflightInput.handlers.change();
const selectedDisc = await ready;
let preflightDependencies = 0;
const preflightModule = {};
await install({ Module: preflightModule, FS: { mkdirTree() {}, writeFile() {} },
  addRunDependency() { ++preflightDependencies; }, removeRunDependency() { --preflightDependencies; },
  document: preflightDocument, config });
assert.equal(preflightDependencies, 0);
assert.equal(preflightModule.pspLocalDisc, selectedDisc);
assert.equal(selectedDisc.stats.gameReads, 0, 'preflight must not preload game payloads');

// Verify Madden NFL 10 (ULUS10441) ISO identity, title check, and drag-and-drop selection.
const maddenConfig = { discId: 'ULUS10441', title: 'Madden NFL 10', ebootHashes: config.ebootHashes };
const maddenSample = fixture('ULUS10441');
const maddenDisc = await (await IsoDisc.index(maddenSample.file)).verify(maddenConfig);
assert.equal(maddenDisc.identity.discId, 'ULUS10441');
await assert.rejects((await IsoDisc.index(sample.file)).verify(maddenConfig), /ULUS10441/);
const maddenElements = new Map(['disc-loader', 'disc-file', 'disc-message', 'status'].map(id => [id, new Element()]));
const maddenDocument = { getElementById(id) { return maddenElements.get(id); } };
const maddenReady = prepare({ document: maddenDocument, config: maddenConfig });
await maddenElements.get('disc-loader').handlers.drop({ preventDefault() {}, dataTransfer: { files: [maddenSample.file] } });
const droppedMaddenDisc = await maddenReady;
assert.equal(droppedMaddenDisc.identity.discId, 'ULUS10441');
assert.match(maddenElements.get('disc-message').textContent, /Madden10-test\.iso .* ULUS10441 · loaded locally/);
assert.match(maddenElements.get('status').textContent, /Loading Madden NFL 10 engine/);

// Execute the actual EM_JS implementation so a local read cannot accidentally
// fall through to HTTP, and HTTP 200 fallback still respects the byte offset.
const cpp = readFileSync(new URL('../profile/host/webfs.cpp', import.meta.url), 'utf8');
const body = cpp.slice(cpp.indexOf('    const target ='), cpp.indexOf('    attempt(4);') + '    attempt(4);'.length);
async function bridge(local, start, end, response = null) {
  let requests = 0, resolve;
  const completed = new Promise(done => { resolve = done; });
  const heap = new Uint8Array(1024);
  const context = { request: 1, url: 'disc/PSP_GAME/DATA.BIN', start, end, Module: { pspReadDisc: local },
    UTF8ToString: value => value, Promise, Uint8Array, HEAPU8: heap, _malloc: () => 8, _free() {},
    _pspweb_chunk_done: (_, pointer, length) => resolve(pointer ? heap.slice(pointer, pointer + length) : null),
    fetch: async () => { ++requests; if (!response) throw new Error('Unexpected HTTP'); return response; },
    console: { error() {} }, setTimeout };
  vm.runInNewContext(body, context);
  return { data: await completed, requests };
}
const local = await bridge(Module.pspReadDisc, 10, 20);
assert.equal(local.requests, 0); assert.deepEqual(local.data, sample.payload.slice(10, 21));
assert.equal((await bridge(async () => new ArrayBuffer(1), 0, 3)).data, null, 'short reads report failure');
const remote = await bridge(null, 2, 5, { status: 200, arrayBuffer: async () => new Uint8Array([0, 1, 2, 3, 4, 5, 6]).buffer });
assert.deepEqual(remote.data, new Uint8Array([2, 3, 4, 5]));
console.log('PASS: local ISO indexing/identity/revision, file/raw/tail ranges, malformed discs, startup retry/gate, no-upload Wasm bridge and HTTP 200 offsets');

// Bootstrap is tested independently of the large game engine: it must gate
// threaded Wasm until isolation is available, without reload loops.
const bootstrap = readFileSync(new URL('../profile/web/static_bootstrap.js', import.meta.url), 'utf8');
async function runBootstrap({isolated = false, secure = true, reloaded = false, storageBlocked = false, registerStalls = false, registerFails = false, launcher = false, sharedMemory = true} = {}) {
  const status = {}, retry = {hidden: true}, appended = [], registrations = [];
  let reloads = 0, preflights = 0;
  const listeners = new Set(), clearedTimers = [];
  const worker = {controller: null, addEventListener(_, fn) {listeners.add(fn);}, removeEventListener(_, fn) {listeners.delete(fn);},
    async register(url, options) {
      registrations.push({url, options});
      if (registerStalls) return new Promise(() => {});
      if (registerFails) throw new Error('Registration denied');
      this.controller = {scriptURL: url};
    }};
  const storage = {getItem() {if (storageBlocked) throw new Error('Storage unavailable'); return reloaded ? '1' : null;}, setItem() {},
    removeItem() {if (storageBlocked) throw new Error('Storage unavailable');}};
  await vm.runInNewContext(bootstrap, {crossOriginIsolated: isolated, isSecureContext: secure,
    document: {currentScript: {dataset: {runtime: 'index.js'}}, getElementById: id => id === 'startup-retry' ? retry : status,
      createElement: () => ({}), body: {appendChild: item => appended.push(item)}},
    location: {pathname: '/ncaa/', href: 'https://example.test/ncaa/', reload() {++reloads;}},
    navigator: {serviceWorker: worker}, sessionStorage: storage, URL,
    setTimeout(callback) {if (registerStalls) queueMicrotask(callback); return 1;},
    clearTimeout(id) {clearedTimers.push(id);},
    PSPWEB_LOCAL_ISO: launcher ? {launcher: 'iso-only-v1'} : null,
    SharedArrayBuffer: sharedMemory ? SharedArrayBuffer : undefined,
    PspLocalIso: {async prepare() {++preflights; assert.equal(appended.length, 0);}}});
  return {status, retry, appended, registrations, get reloads() {return reloads;}, listeners, clearedTimers, preflights};
}
const direct = await runBootstrap({isolated: true, storageBlocked: true});
assert.equal(direct.appended[0].src, 'index.js');
assert.equal(direct.registrations.length, 0);
assert.equal((await runBootstrap({isolated: true, launcher: true})).preflights, 1);
assert.equal((await runBootstrap({launcher: true})).preflights, 0, 'select the File only after the isolation reload');
const fallback = await runBootstrap();
assert.equal(fallback.registrations[0].url, 'https://example.test/ncaa/coi-serviceworker.js');
assert.equal(fallback.registrations[0].options.scope, './');
assert.equal(fallback.reloads, 1); assert.equal(fallback.appended.length, 0);
assert.equal(fallback.listeners.size, 0); assert.deepEqual(fallback.clearedTimers, [1]);
const timeout = await runBootstrap({registerStalls: true});
assert.match(timeout.status.textContent, /timed out/);
assert.equal(timeout.reloads, 0); assert.equal(timeout.appended.length, 0);
assert.equal(timeout.listeners.size, 0); assert.deepEqual(timeout.clearedTimers, [1]);
const failedRegistration = await runBootstrap({registerFails: true});
assert.match(failedRegistration.status.textContent, /Registration denied/);
assert.equal(failedRegistration.listeners.size, 0); assert.deepEqual(failedRegistration.clearedTimers, [1]);
assert.match((await runBootstrap({storageBlocked: true})).status.textContent, /Allow site storage/);
assert.match((await runBootstrap({secure: false})).status.textContent, /HTTPS/);
const loop = await runBootstrap({reloaded: true});
assert.equal(loop.reloads, 0); assert.equal(loop.appended.length, 0);
assert.match(loop.status.textContent, /isolation/);
assert.equal(loop.retry.hidden, false);
loop.retry.onclick();
assert.equal(loop.reloads, 1, 'an explicit retry may reload after an isolation failure');
const unsupported = await runBootstrap({isolated: true, launcher: true, sharedMemory: false});
assert.equal(unsupported.preflights, 0);
assert.equal(unsupported.appended.length, 0);
assert.match(unsupported.status.textContent, /shared memory/);
direct.appended[0].onerror();
assert.equal(direct.retry.hidden, false);
assert.match(direct.status.textContent, /connection/);
console.log('PASS: header-enabled bootstrap, blocked storage, subdirectory worker registration, isolation gating, stalled/failed registration cleanup and reload-loop rejection');

/* Static-host launcher: imports a locally built NCAA static package into this
 * browser's Cache Storage, verified against its static-manifest.json, then
 * opens it under launcher-sw.js. The package's own page chooses the ISO.
 * Nothing selected here is uploaded; game files never come from this site. */
(function (root) {
  'use strict';
  const CACHE = 'ncaa-launcher-build-v1';
  const RECORD = 'launcher-build.json';
  const GAME = 'game/';
  const REQUIRED = ['index.html', 'index.js', 'index.wasm', 'index.data', 'local_iso.js', 'static_bootstrap.js'];
  const TYPES = {
    html: 'text/html; charset=utf-8', js: 'text/javascript; charset=utf-8', wasm: 'application/wasm',
    data: 'application/octet-stream', json: 'application/json', md: 'text/markdown; charset=utf-8',
    txt: 'text/plain; charset=utf-8',
  };
  const MAX_FILES = 256;
  function fail(message) { throw new Error(message); }

  function safePath(path) {
    return typeof path === 'string' && path.length > 0 && path.length < 512 && !path.startsWith('/') &&
      !path.includes('\\') && !path.includes('\0') && path.split('/').every(part => part && part !== '.' && part !== '..');
  }
  // A folder selection names files "<folder>/<path>"; a plain multi-file selection has bare names.
  function relativePath(file) {
    const full = file.webkitRelativePath || file.name;
    const slash = file.webkitRelativePath ? full.indexOf('/') : -1;
    return slash >= 0 ? full.slice(slash + 1) : full;
  }
  function contentType(path) {
    const dot = path.lastIndexOf('.');
    return TYPES[dot >= 0 ? path.slice(dot + 1).toLowerCase() : ''] || 'application/octet-stream';
  }
  async function sha256(file) {
    const digest = new Uint8Array(await root.crypto.subtle.digest('SHA-256', await file.arrayBuffer()));
    return Array.from(digest, value => value.toString(16).padStart(2, '0')).join('');
  }

  function parseManifest(text) {
    let manifest;
    try { manifest = JSON.parse(text); } catch (_) { fail('static-manifest.json is not valid JSON.'); }
    if (!manifest || manifest.format !== 1 || !Array.isArray(manifest.files)) fail('Unsupported static-manifest.json; rebuild with scripts/prepare_static_ncaa.py.');
    if (manifest.discId !== 'ULUS10440' && manifest.discId !== 'ULUS10441') fail('This package is not an NCAA Football 10 PSP US (ULUS10440) build.');
    if (manifest.files.length > MAX_FILES) fail('static-manifest.json lists too many files.');
    const entries = new Map();
    for (const item of manifest.files) {
      if (!item || !safePath(item.path) || !Number.isSafeInteger(item.bytes) || item.bytes < 0 ||
          typeof item.sha256 !== 'string' || !/^[0-9a-f]{64}$/.test(item.sha256) || entries.has(item.path)) {
        fail('static-manifest.json contains an invalid or duplicate file entry.');
      }
      entries.set(item.path, item);
    }
    for (const name of REQUIRED) if (!entries.has(name)) fail('static-manifest.json does not list ' + name + '.');
    entries.discId = manifest.discId;
    return entries;
  }

  // Selects and verifies the package files. Files the manifest does not list,
  // such as an ISO accidentally placed in the folder, are never read or stored.
  async function readPackage(files, progress = () => {}) {
    const selected = new Map();
    for (const file of Array.from(files || [])) {
      const path = relativePath(file);
      if (selected.has(path)) fail('The selection contains ' + path + ' twice.');
      selected.set(path, file);
    }
    const manifestFile = selected.get('static-manifest.json');
    if (!manifestFile) fail('Choose the folder made by scripts/prepare_static_ncaa.py; static-manifest.json is missing.');
    if (manifestFile.size > 1024 * 1024) fail('static-manifest.json is too large.');
    const manifest = parseManifest(await manifestFile.text());
    const entries = [];
    for (const [path, item] of manifest) {
      const file = selected.get(path);
      if (!file) {
        if (REQUIRED.includes(path)) fail('The selection is missing ' + path + '.');
        continue;
      }
      if (file.size !== item.bytes) fail(path + ' does not match static-manifest.json (size).');
      progress('Verifying ' + path + '…');
      if (await sha256(file) !== item.sha256) fail(path + ' does not match static-manifest.json (SHA-256).');
      entries.push({ path, file, bytes: item.bytes, sha256: item.sha256, type: contentType(path) });
    }
    const byPath = new Map(entries.map(entry => [entry.path, entry]));
    const magic = new Uint8Array(await byPath.get('index.wasm').file.slice(0, 8).arrayBuffer());
    if (magic.join() !== '0,97,115,109,1,0,0,0') fail('index.wasm is not a WebAssembly module.');
    const html = await byPath.get('index.html').file.text();
    if (!html.includes('window.PSPWEB_LOCAL_ISO') || !html.includes('static_bootstrap.js')) {
      fail('index.html is not a local-ISO static package; run scripts/prepare_static_ncaa.py on your build.');
    }
    const ignored = Array.from(selected.keys()).filter(path => path !== 'static-manifest.json' && !manifest.has(path));
    return { discId: manifest.discId || 'ULUS10440', entries, ignored };
  }

  function recordURL(base) { return new URL(RECORD, base).href; }
  function gameURL(base, path) { return new URL(GAME + path, base).href; }

  async function storePackage(storage, base, pack, progress = () => {}) {
    // The record is written last, so an interrupted import is never launchable.
    await storage.delete(CACHE);
    const cache = await storage.open(CACHE);
    for (const entry of pack.entries) {
      progress('Storing ' + entry.path + '…');
      await cache.put(gameURL(base, entry.path), new Response(entry.file, {
        headers: { 'Content-Type': entry.type, 'Content-Length': String(entry.bytes) },
      }));
    }
    const record = {
      format: 1, discId: pack.discId || 'ULUS10440', importedAt: new Date().toISOString(),
      files: pack.entries.map(({ path, bytes, sha256 }) => ({ path, bytes, sha256 })),
    };
    await cache.put(recordURL(base), new Response(JSON.stringify(record), { headers: { 'Content-Type': 'application/json' } }));
    return record;
  }

  async function storedPackage(storage, base) {
    if (!(await storage.has(CACHE))) return null;
    const cache = await storage.open(CACHE);
    const response = await cache.match(recordURL(base));
    if (!response) return null;
    let record;
    try { record = await response.json(); } catch (_) { return null; }
    if (!record || record.format !== 1 || !Array.isArray(record.files)) return null;
    for (const name of REQUIRED) if (!record.files.some(item => item.path === name)) return null;
    for (const item of record.files) if (!(await cache.match(gameURL(base, item.path)))) return null;
    return record;
  }

  function sizeText(bytes) {
    return bytes >= 1048576 ? (bytes / 1048576).toFixed(1) + ' MiB' : Math.max(1, Math.round(bytes / 1024)) + ' KiB';
  }
  function describe(record) {
    const total = record.files.reduce((sum, item) => sum + item.bytes, 0);
    const wasm = record.files.find(item => item.path === 'index.wasm');
    return record.files.length + ' files, ' + sizeText(total) + ' · index.wasm ' + wasm.sha256.slice(0, 12) +
      ' · imported ' + new Date(record.importedAt).toLocaleString();
  }

  function setup(document) {
    const $ = id => document.getElementById(id);
    const support = $('support'), message = $('build-message'), summary = $('build-summary');
    const folder = $('build-folder'), loose = $('build-files'), launch = $('launch'), remove = $('remove');
    const base = document.baseURI;
    let worker = null, busy = false;
    const show = (element, text, error = false) => { element.textContent = text; element.className = error ? 'error' : ''; };
    async function refresh() {
      let record = null;
      try { record = await storedPackage(root.caches, base); } catch (_) {}
      summary.textContent = record ? describe(record) : 'No build imported in this browser yet.';
      launch.disabled = !record || !worker || busy;
      remove.disabled = !record || busy;
    }
    async function importFiles(input) {
      if (busy || !input.files || !input.files.length) return;
      busy = true;
      folder.disabled = loose.disabled = true;
      launch.disabled = remove.disabled = true;
      try {
        const pack = await readPackage(input.files, text => show(message, text));
        await storePackage(root.caches, base, pack, text => show(message, text));
        if (root.navigator.storage && root.navigator.storage.persist) {
          try { await root.navigator.storage.persist(); } catch (_) {}
        }
        show(message, 'Build imported and verified.' + (pack.ignored.length
          ? ' Ignored ' + pack.ignored.length + ' file(s) not listed in static-manifest.json.' : ''));
      } catch (error) {
        show(message, error.message || String(error), true);
      } finally {
        busy = false;
        folder.disabled = loose.disabled = false;
        input.value = '';
        await refresh();
      }
    }
    folder.addEventListener('change', () => importFiles(folder));
    loose.addEventListener('change', () => importFiles(loose));
    launch.addEventListener('click', () => { root.location.href = new URL(GAME, base).href; });
    remove.addEventListener('click', async () => {
      await root.caches.delete(CACHE);
      show(message, 'Imported build removed from this browser. Saves are kept.');
      await refresh();
    });
    (async function () {
      try {
        if (!root.isSecureContext || !root.navigator.serviceWorker || !root.caches) {
          fail('Open this launcher over HTTPS or localhost in a browser with service workers and site storage enabled.');
        }
        await root.navigator.serviceWorker.register('launcher-sw.js', { scope: './' });
        worker = await root.navigator.serviceWorker.ready;
        show(support, 'Browser support is ready. Files you choose stay on this device.');
      } catch (error) {
        show(support, error.message || String(error), true);
      }
      await refresh();
    })();
  }

  root.PspLauncher = { CACHE, RECORD, REQUIRED, relativePath, contentType, parseManifest, readPackage, storePackage, storedPackage, describe };
  if (root.document && root.document.getElementById('launcher')) setup(root.document);
})(globalThis);

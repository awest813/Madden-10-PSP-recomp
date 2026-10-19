/* Runs the translator off the page thread. The selected ISO File is read in
 * ranges here; results come back as a ZIP Blob. */
'use strict';
importScripts('local_iso.js', 'pspweb_translator.js', 'translator.js');

// Compile the module once and reuse it for every instance. Streaming needs the
// application/wasm MIME type; fall back to a buffered compile on other hosts.
let compiled = null;
async function compile() {
  const response = await fetch('pspweb_translator.wasm');
  if (!response.ok) throw new Error('Could not load the translator (HTTP ' + response.status + ').');
  try {
    return await WebAssembly.compileStreaming(response.clone());
  } catch (_) {
    return WebAssembly.compile(await response.arrayBuffer());
  }
}

self.onmessage = async event => {
  const started = performance.now();
  try {
    if (!compiled) compiled = compile();
    let module;
    try { module = await compiled; } catch (error) { compiled = null; throw error; }
    const createModule = options => createPspTranslator({
      ...options,
      instantiateWasm(imports, success) {
        WebAssembly.instantiate(module, imports).then(instance => success(instance, module),
          error => self.postMessage({ type: 'error', text: 'Could not start the translator: ' + error.message }));
        return {};
      },
    });
    const result = await PspTranslator.translateIso(event.data.file, {
      createModule, progress: text => self.postMessage({ type: 'progress', text }),
    });
    self.postMessage({ type: 'progress', text: 'Packing the ZIP…' });
    const blob = new Blob(await PspTranslator.zip(result.files), { type: 'application/zip' });
    self.postMessage({ type: 'done', blob, provenance: result.provenance, seconds: (performance.now() - started) / 1000 });
  } catch (error) {
    self.postMessage({ type: 'error', text: error.message || String(error) });
  }
};

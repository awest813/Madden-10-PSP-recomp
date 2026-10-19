/* Start the threaded build only after the page is cross-origin isolated.
 * The same-origin service worker provides headers on hosts such as Pages. */
(async function () {
  'use strict';
  const runtime = document.currentScript.dataset.runtime;
  const status = document.getElementById('status');
  const reloadKey = 'ncaa-isolation-reload:' + location.pathname;
  function reportError(message) {
    status.textContent = message;
    status.className = 'status err';
    const retry = document.getElementById('startup-retry');
    if (retry) {
      retry.hidden = false;
      retry.onclick = () => {
        // Explicit user retry permits another setup attempt; automatic reloads
        // retain their guard so a broken host cannot loop indefinitely.
        try { sessionStorage.removeItem(reloadKey); } catch (_) {}
        location.reload();
      };
    }
  }
  try {
    if (!globalThis.crossOriginIsolated) {
      if (!globalThis.isSecureContext || !navigator.serviceWorker) {
        throw new Error('Open this site over HTTPS or localhost, with service workers enabled.');
      }
      let reloaded;
      try { reloaded = sessionStorage.getItem(reloadKey); } catch (_) {
        throw new Error('Allow site storage for browser setup, or configure COOP: same-origin and COEP: require-corp headers.');
      }
      if (reloaded) {
        throw new Error('This host did not enable browser isolation. Serve COOP: same-origin and COEP: require-corp headers.');
      }
      status.textContent = 'Preparing browser support…';
      const workerURL = new URL('coi-serviceworker.js', location.href).href;
      let timer, changed;
      try {
        await new Promise((resolve, reject) => {
          changed = () => {
            if (navigator.serviceWorker.controller && navigator.serviceWorker.controller.scriptURL === workerURL) {
              resolve();
            }
          };
          timer = setTimeout(() => reject(new Error('Browser support setup timed out. Reload to retry.')), 15000);
          navigator.serviceWorker.addEventListener('controllerchange', changed);
          Promise.resolve(navigator.serviceWorker.register(workerURL, { scope: './' })).then(changed, reject);
        });
      } finally {
        clearTimeout(timer);
        if (changed) navigator.serviceWorker.removeEventListener('controllerchange', changed);
      }
      sessionStorage.setItem(reloadKey, '1');
      location.reload();
      return;
    }
    // Header-enabled hosts do not need storage for the one-reload guard.
    try { sessionStorage.removeItem(reloadKey); } catch (_) {}
    // Do browser isolation first: its one-time reload must happen before a
    // player selects a File. The prepared launcher downloads no large engine
    // files until the ISO passes the title and executable checks.
    if (globalThis.PSPWEB_LOCAL_ISO && globalThis.PSPWEB_LOCAL_ISO.launcher === 'iso-only-v1') {
      if (typeof globalThis.WebAssembly !== 'object' || typeof globalThis.SharedArrayBuffer !== 'function') {
        throw new Error('This browser cannot run the threaded WebAssembly player. Use a current browser with shared memory enabled.');
      }
      await globalThis.PspLocalIso.prepare({ document, config: globalThis.PSPWEB_LOCAL_ISO });
    }
    const script = document.createElement('script');
    script.src = runtime;
    script.async = true;
    script.onerror = () => reportError('Could not load the game engine. Check your connection and retry startup.');
    document.body.appendChild(script);
  } catch (error) {
    reportError(error.message);
  }
})();

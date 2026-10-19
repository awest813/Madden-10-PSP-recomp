/* Serves the build imported by launcher.js from Cache Storage under game/ and
 * adds the cross-origin isolation headers threaded WebAssembly needs on hosts
 * such as GitHub Pages. Missing game files are 404s, never network fallbacks,
 * except game/index.html, whose published stub returns to the launcher. */
'use strict';
const CACHE = 'ncaa-launcher-build-v1';

function gameRoot() { return new URL('game/', self.registration.scope).href; }

function isolated(response) {
  // Navigations fetch with manual redirects, which yields an opaque redirect
  // (status 0) for e.g. a host's trailing-slash redirect. Such responses cannot
  // be rebuilt and are returned as they are; the browser follows them.
  if (response.type === 'opaqueredirect' || response.status === 0) return response;
  const headers = new Headers(response.headers);
  headers.set('Cross-Origin-Opener-Policy', 'same-origin');
  headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
  headers.set('Cross-Origin-Resource-Policy', 'same-origin');
  return new Response(response.body, { status: response.status, statusText: response.statusText, headers });
}

async function serveGame(request) {
  const url = new URL(request.url);
  let key = url.origin + url.pathname;
  if (key.endsWith('/')) key += 'index.html';
  if (await caches.has(CACHE)) {
    const cached = await (await caches.open(CACHE)).match(key);
    if (cached) return isolated(cached);
  }
  if (key === gameRoot() + 'index.html') return isolated(await fetch(key, { cache: 'no-cache' }));
  return isolated(new Response('Not in the imported build.', { status: 404, headers: { 'Content-Type': 'text/plain' } }));
}

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', event => event.waitUntil(self.clients.claim()));
self.addEventListener('fetch', event => {
  const request = event.request;
  if (new URL(request.url).origin !== self.location.origin) return;
  if (request.method === 'GET' && request.url.startsWith(gameRoot())) {
    event.respondWith(serveGame(request));
  } else {
    event.respondWith(fetch(request).then(isolated));
  }
});

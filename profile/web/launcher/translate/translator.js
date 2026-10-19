/* In-browser NCAA translation (phase one of compiling an ISO in the browser).
 * Reads the selected ISO locally, converts its executable to the verified
 * plain ELF, runs the unchanged PSPRecomp code generator compiled to
 * WebAssembly (pspweb_translator.wasm) and extracts the EA audio programs the
 * same way as scripts/generate.sh and scripts/generate_ncaa_sound.py. Results
 * are returned as files for a ZIP laid out like the repository. Nothing is
 * uploaded and no game data comes from the hosting site. */
(function (root) {
  'use strict';
  // Keep in sync with scripts/verify_ncaa10.py (checked by test_web_translator.mjs).
  const NCAA_EBOOT_SHA256 = '2b743dc6d746d822969a9b967b13422a369ff03c6a3a3e2604e051c1642b94f6';
  const LOAD_BASE = '0x08804000', UNIT_SPAN = '0x4000';
  const AUDIO_ROOT = 'psp_game/usrdir/data/audio/aems/';
  const BANK_MAGIC = Uint8Array.from([0x41, 0x42, 0x4b, 0x43]); // 'ABKC'
  const SOUND_SIGNATURE = Uint8Array.from([0xf8, 0xff, 0xbd, 0x27, 0x00, 0x00, 0xb0, 0xaf, 0x04, 0x00, 0xbf, 0xaf, 0x21, 0x80, 0x80, 0x00]);
  const encoder = new TextEncoder();
  function fail(message) { throw new Error(message); }

  async function sha256(bytes) {
    const digest = new Uint8Array(await root.crypto.subtle.digest('SHA-256', bytes));
    return Array.from(digest, value => value.toString(16).padStart(2, '0')).join('');
  }
  function hex(value, width = 0) { return value.toString(16).padStart(width, '0'); }

  // Runs one PSPRecomp command line in a fresh module instance, so no state
  // carries between invocations, and returns its exit status and output.
  async function runCodegen(createModule, args, files = {}) {
    const output = [];
    const module = await createModule({ print: text => output.push(text), printErr: text => output.push(text) });
    for (const [path, data] of Object.entries(files)) {
      module.FS.mkdirTree(path.slice(0, path.lastIndexOf('/')) || '/');
      module.FS.writeFile(path, data);
    }
    let status;
    try { status = module.callMain(args); } catch (error) {
      status = typeof error.status === 'number' ? error.status : fail('Code generator failed: ' + (error.message || error));
    }
    return { module, status, log: output.join('\n') };
  }
  function readTree(FS, directory, prefix) {
    const files = [];
    for (const name of FS.readdir(directory).filter(name => name !== '.' && name !== '..').sort()) {
      const path = directory + '/' + name;
      if (FS.isDir(FS.stat(path).mode)) files.push(...readTree(FS, path, prefix + name + '/'));
      else files.push({ path: prefix + name, data: FS.readFile(path) });
    }
    return files;
  }

  async function executableToElf(createModule, executable) {
    const module = await createModule({ print() {}, printErr() {} });
    module.FS.writeFile('/EBOOT.BIN', executable);
    const errorSize = 512, error = module._malloc(errorSize);
    try {
      const status = module.ccall('pspweb_convert_executable', 'number', ['string', 'string', 'number', 'number'],
        ['/EBOOT.BIN', '/EBOOT.ELF', error, errorSize]);
      if (status !== 0) fail(module.UTF8ToString(error));
      return module.FS.readFile('/EBOOT.ELF');
    } finally { module._free(error); }
  }

  async function generateExecutable(createModule, elf) {
    const run = await runCodegen(createModule, ['/in/EBOOT.BIN', '--auto', '/out', LOAD_BASE, UNIT_SPAN], { '/in/EBOOT.BIN': elf });
    if (run.status !== 0) fail('Code generation failed:\n' + run.log);
    return { files: readTree(run.module.FS, '/out', ''), log: run.log };
  }

  function matchesAt(data, needle, at) {
    if (at < 0 || at + needle.length > data.length) return false;
    for (let j = 0; j < needle.length; ++j) if (data[at + j] !== needle[j]) return false;
    return true;
  }
  function indexOf(data, needle, from) {
    for (let i = Math.max(0, from); i + needle.length <= data.length; ++i) {
      if (data[i] === needle[0] && matchesAt(data, needle, i)) return i;
    }
    return -1;
  }
  // Python slice semantics for data[start:stop].
  function slice(data, start, stop) {
    const from = Math.min(start, data.length);
    return data.subarray(from, Math.max(from, Math.min(stop, data.length)));
  }
  function key(bytes) { return Array.from(bytes, value => hex(value, 2)).join(''); }
  // Path order of sorted(pathlib.Path.rglob()) on POSIX: compare part by part.
  function comparePaths(a, b) {
    const x = a.split('/'), y = b.split('/');
    for (let i = 0; i < Math.min(x.length, y.length); ++i) if (x[i] !== y[i]) return x[i] < y[i] ? -1 : 1;
    return x.length - y.length;
  }

  // Port of scripts/generate_ncaa_sound.py's bank scan over ISO files.
  async function findAudioPrograms(files) {
    const programs = new Map();
    for (const { path, read } of [...files].sort((a, b) => comparePaths(a.path, b.path))) {
      const data = await read();
      const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
      for (let search = 0, bank; (bank = indexOf(data, BANK_MAGIC, search)) >= 0;) {
        search = bank + 4;
        const first = indexOf(data, SOUND_SIGNATURE, bank);
        if (first < 0) continue;
        const entries = [];
        for (let offset = bank + 0x70; offset < first; offset += 4) {
          if (offset + 12 > data.length) fail('Truncated audio bank table in ' + path);
          const kind = view.getUint32(offset, true), code = view.getUint32(offset + 4, true), payload = view.getUint32(offset + 8, true);
          // Python compares data[bank+code:bank+code+16], so a signature cut short by the end never matches.
          if (kind >= 0x10100 && kind <= 0x101ff && matchesAt(data, SOUND_SIGNATURE, bank + code)) entries.push([code, payload]);
        }
        if (!entries.length) continue;
        const starts = [...new Set(entries.map(([code]) => code))].sort((a, b) => a - b);
        const end = Math.min(...entries.map(([, payload]) => payload));
        starts.forEach((start, index) => {
          const stop = index + 1 < starts.length ? starts[index + 1] : end;
          const blob = slice(data, bank + start, bank + stop);
          if (!blob.length || blob.length % 4 || blob.length > 16384) fail(`Invalid code range in ${path}: ${hex(start)}-${hex(stop)}`);
          const id = key(blob);
          if (!programs.has(id)) programs.set(id, { blob: blob.slice(), origins: [] });
          programs.get(id).origins.push(`${path}@${hex(bank + start)}`);
        });
      }
    }
    if (!programs.size) fail('No EA audio programs found; supply the complete verified NCAA Football 10 disc.');
    return [...programs.values()];
  }

  async function generateSound(createModule, programs) {
    const files = [], manifest = [];
    const source = ['#include "psprecomp/runtime.hpp"', '#include <bit>', '#include <cmath>',
      '#include <cstdint>', '#include <limits>', 'namespace psprecomp {'];
    for (const [index, { blob, origins }] of programs.entries()) {
      const name = 'ncaa_sound_' + String(index).padStart(3, '0');
      const run = await runCodegen(createModule, ['--blob', `/sound/${name}.bin`, `/sound/${name}.inc`, name], { [`/sound/${name}.bin`]: blob });
      if (run.status !== 0) fail(`Audio program ${name} failed to translate:\n` + run.log);
      files.push({ path: `${name}.bin`, data: blob }, { path: `${name}.inc`, data: run.module.FS.readFile(`/sound/${name}.inc`) });
      source.push(`#include "${name}.inc"`);
      const view = new DataView(blob.buffer, blob.byteOffset, blob.byteLength), words = [];
      for (let at = 0; at < blob.length; at += 4) words.push('0x' + hex(view.getUint32(at, true), 8) + 'u');
      source.push(`static const std::uint32_t words_${index}[] = {` + words.join(',') + '};');
      manifest.push({ name, bytes: blob.length, sha256: await sha256(blob), origins });
    }
    source.push('const NcaaSoundProgram ncaa_sound_programs[] = {');
    programs.forEach((_, index) => source.push(`{words_${index}, sizeof(words_${index}) / 4u, &ncaa_sound_${String(index).padStart(3, '0')}_entry},`));
    source.push('};', `const std::uint32_t ncaa_sound_program_count = ${programs.length}u;`, '}');
    source.unshift('#include "../../host/ncaa_sound.hpp"');
    files.push({ path: 'ncaa_sound.cpp', data: encoder.encode(source.join('\n') + '\n') });
    files.push({ path: 'manifest.json', data: encoder.encode(JSON.stringify(manifest, null, 2) + '\n') });
    files.sort((a, b) => (a.path < b.path ? -1 : 1));
    return files;
  }

  // Full pipeline for an ISO File. createModule is the Emscripten factory.
  async function translateIso(file, { createModule, expectedSha256 = NCAA_EBOOT_SHA256, progress = () => {} }) {
    const disc = await root.PspLocalIso.IsoDisc.index(file, progress);
    const metadata = disc.entries.get('psp_game/param.sfo'), executable = disc.entries.get('psp_game/sysdir/eboot.bin');
    if (!metadata || metadata.directory || metadata.size > 1024 * 1024 || !executable || executable.directory || executable.size > 64 * 1024 * 1024) {
      fail('This ISO is missing the NCAA PSP disc metadata or executable.');
    }
    progress('Checking NCAA title…');
    const info = root.PspLocalIso.sfo(await disc.bytes(metadata.offset, metadata.size));
    if (info.DISC_ID !== 'ULUS10440' || !/FOOTBALL 10/i.test(info.TITLE || '')) fail('This translator requires NCAA Football 10 PSP US (ULUS10440).');
    progress('Decrypting the executable…');
    const encrypted = await disc.bytes(executable.offset, executable.size);
    const elf = await executableToElf(createModule, encrypted);
    const elfSha256 = await sha256(elf);
    if (elfSha256 !== expectedSha256) {
      fail('The decrypted executable does not match the verified NCAA build (SHA-256 ' + elfSha256 + '). Translation stopped.');
    }
    progress('Translating the executable to C++ (this can take several minutes)…');
    const executableCorpus = await generateExecutable(createModule, elf);
    progress('Finding EA audio programs…');
    const audio = [];
    for (const entry of disc.entries.values()) {
      if (!entry.directory && entry.path.toLowerCase().startsWith(AUDIO_ROOT)) {
        audio.push({ path: entry.path, read: () => disc.bytes(entry.offset, entry.size) });
      }
    }
    const programs = await findAudioPrograms(audio);
    progress(`Translating ${programs.length} audio programs…`);
    const sound = await generateSound(createModule, programs);
    const provenance = {
      format: 1, title: info.TITLE, discId: info.DISC_ID, encryptedExecutableSha256: await sha256(encrypted),
      executableSha256: elfSha256, executableBytes: elf.length, executableUnits: executableCorpus.files.length,
      audioPrograms: programs.length, audioBankEntries: programs.reduce((sum, item) => sum + item.origins.length, 0),
      loadBase: LOAD_BASE, unitSpan: UNIT_SPAN, generatedAt: new Date().toISOString(),
    };
    const files = [
      { path: 'games/ncaa10/root/EBOOT.BIN', data: elf },
      ...executableCorpus.files.map(item => ({ path: 'profile/generated/ncaa10/' + item.path, data: item.data })),
      ...sound.map(item => ({ path: 'profile/generated/ncaa10_sound/' + item.path, data: item.data })),
      { path: 'TRANSLATION.json', data: encoder.encode(JSON.stringify(provenance, null, 2) + '\n') },
    ];
    return { files, provenance, log: executableCorpus.log };
  }

  // Minimal ZIP writer (deflate when CompressionStream is available, else
  // stored), returning Blob parts so large corpora need not be contiguous.
  const CRC_TABLE = (() => {
    const table = new Uint32Array(256);
    for (let n = 0; n < 256; ++n) { let c = n; for (let k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; table[n] = c >>> 0; }
    return table;
  })();
  function crc32(bytes) {
    let c = 0xFFFFFFFF;
    for (let i = 0; i < bytes.length; ++i) c = CRC_TABLE[(c ^ bytes[i]) & 0xFF] ^ (c >>> 8);
    return (c ^ 0xFFFFFFFF) >>> 0;
  }
  async function deflateRaw(bytes) {
    if (typeof root.CompressionStream !== 'function') return null;
    const stream = new Blob([bytes]).stream().pipeThrough(new root.CompressionStream('deflate-raw'));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  }
  async function zip(files) {
    const parts = [], central = [];
    let offset = 0;
    for (const { path, data } of files) {
      const name = encoder.encode(path), crc = crc32(data);
      const packed = await deflateRaw(data);
      const method = packed && packed.length < data.length ? 8 : 0, body = method ? packed : data;
      if (offset + body.length > 0xFFFFFFFF || files.length > 0xFFFF) fail('The translation is too large for a ZIP32 archive.');
      const local = new Uint8Array(30 + name.length), view = new DataView(local.buffer);
      view.setUint32(0, 0x04034b50, true); view.setUint16(4, 20, true); view.setUint16(6, 0x800, true); view.setUint16(8, method, true);
      view.setUint16(12, 0x21, true); view.setUint32(14, crc, true); view.setUint32(18, body.length, true); view.setUint32(22, data.length, true);
      view.setUint16(26, name.length, true); local.set(name, 30);
      const record = new Uint8Array(46 + name.length), entry = new DataView(record.buffer);
      entry.setUint32(0, 0x02014b50, true); entry.setUint16(4, 20, true); entry.setUint16(6, 20, true); entry.setUint16(8, 0x800, true);
      entry.setUint16(10, method, true); entry.setUint16(14, 0x21, true); entry.setUint32(16, crc, true); entry.setUint32(20, body.length, true);
      entry.setUint32(24, data.length, true); entry.setUint16(28, name.length, true); entry.setUint32(42, offset, true); record.set(name, 46);
      parts.push(local, body); central.push(record);
      offset += local.length + body.length;
    }
    const size = central.reduce((sum, record) => sum + record.length, 0);
    const end = new Uint8Array(22), view = new DataView(end.buffer);
    view.setUint32(0, 0x06054b50, true); view.setUint16(8, files.length, true); view.setUint16(10, files.length, true);
    view.setUint32(12, size, true); view.setUint32(16, offset, true);
    return [...parts, ...central, end];
  }

  root.PspTranslator = { NCAA_EBOOT_SHA256, comparePaths, executableToElf, generateExecutable, findAudioPrograms, generateSound, translateIso, zip, crc32 };
})(globalThis);

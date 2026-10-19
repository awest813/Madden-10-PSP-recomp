// Browser translator (Wasm) against the native tools on synthetic inputs: executable
// decryption, PSPRecomp --auto output, EA audio-program extraction, the full ISO
// pipeline and the ZIP. Requires build/translator-web, build/translator-native and
// build/tools from scripts/build_translator.sh and scripts/setup.sh. No game data.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { isoImage, sfo } from './iso_fixture.mjs';
import { pspElf } from './psp_elf_fixture.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const WEB = process.env.PSPWEB_TRANSLATOR_WEB || path.join(ROOT, 'build/translator-web');
const NATIVE = process.env.PSPWEB_TRANSLATOR_NATIVE || path.join(ROOT, 'build/translator-native');
const TOOLS = process.env.PSPWEB_TOOLS || path.join(ROOT, 'build/tools');
const createModule = createRequire(import.meta.url)(path.join(WEB, 'pspweb_translator.js'));
await import('../profile/web/local_iso.js');
await import('../profile/web/launcher/translate/translator.js');
const T = globalThis.PspTranslator;
const sha = data => createHash('sha256').update(data).digest('hex');

mkdirSync(path.join(ROOT, 'build/translator-tests'), { recursive: true });
const work = mkdtempSync(path.join(ROOT, 'build/translator-tests/run-'));
try {
  // The page's expected executable hash matches the build's identity check.
  const verify = readFileSync(path.join(ROOT, 'scripts/verify_ncaa10.py'), 'utf8');
  assert.equal(T.NCAA_EBOOT_SHA256, /EXPECTED_EBOOT_SHA256 = '([0-9a-f]{64})'/.exec(verify)[1]);

  // Decryption: plain ELF passes through; synthetic encrypted executables (plain and
  // gzip) decrypt in Wasm to exactly the input; failures surface as errors.
  const elf = pspElf();
  writeFileSync(path.join(work, 'test.elf'), elf);
  assert.deepEqual(await T.executableToElf(createModule, elf), elf);
  const tests = path.join(NATIVE, 'pspweb_translator_tests');
  execFileSync(tests, ['--encrypt-fixture', path.join(work, 'test.elf'), path.join(work, 'EBOOT.BIN')]);
  const encrypted = readFileSync(path.join(work, 'EBOOT.BIN'));
  assert.deepEqual(await T.executableToElf(createModule, encrypted), elf);
  execFileSync('python3', ['-c', `import gzip,sys;open(sys.argv[2],'wb').write(gzip.compress(open(sys.argv[1],'rb').read(),mtime=0))`,
    path.join(work, 'test.elf'), path.join(work, 'test.elf.gz')]);
  execFileSync(tests, ['--encrypt-fixture', path.join(work, 'test.elf.gz'), path.join(work, 'EBOOT-gzip.BIN'), 'gzip']);
  assert.deepEqual(await T.executableToElf(createModule, readFileSync(path.join(work, 'EBOOT-gzip.BIN'))), elf);
  const native = path.join(work, 'native.elf');
  execFileSync(path.join(NATIVE, 'pspweb_prx_decrypt'), [path.join(work, 'EBOOT-gzip.BIN'), native]);
  assert.deepEqual(new Uint8Array(readFileSync(native)), elf);
  const damaged = Uint8Array.from(encrypted); damaged[0x160] ^= 1;
  await assert.rejects(T.executableToElf(createModule, damaged), /CMAC/);
  await assert.rejects(T.executableToElf(createModule, new Uint8Array(400)), /Not an encrypted/);

  // --auto: identical tree to native psp_recomp.
  const nativeOut = path.join(work, 'native-auto');
  execFileSync(path.join(TOOLS, 'psp_recomp'), [path.join(work, 'test.elf'), '--auto', nativeOut, '0x08804000', '0x4000'], { stdio: 'pipe' });
  const tree = directory => readdirSync(directory, { recursive: true }).filter(name => statSync(path.join(directory, name)).isFile()).sort()
    .map(name => ({ path: name.split(path.sep).join('/'), data: readFileSync(path.join(directory, name)) }));
  const wasmAuto = await T.generateExecutable(createModule, elf);
  assert.deepEqual(wasmAuto.files.map(file => [file.path, sha(file.data)]), tree(nativeOut).map(file => [file.path, sha(file.data)]));
  assert.ok(wasmAuto.files.some(file => file.path === 'generated_unit_0000.cpp'));

  // EA audio programs: identical output to scripts/generate_ncaa_sound.py.
  const signature = [0x27BDFFF8, 0xAFB00000, 0xAFBF0004, 0x00808021];
  const program = tail => Buffer.from(Uint32Array.from([...signature, ...tail]).buffer);
  const epilogue = [0x8FBF0004, 0x8FB00000, 0x03E00008, 0x27BD0008];
  function bank(programs, padding = 0x10) {
    const table = 0x70, code = 0x100;
    let at = code;
    const offsets = programs.map(item => { const offset = at; at += item.length; return offset; });
    const payload = at;
    const data = Buffer.alloc(padding + payload + 0x20);
    data.write('ABKC', padding);
    programs.forEach((item, i) => {
      data.writeUInt32LE(0x10100 + i, padding + table + 12 * i);
      data.writeUInt32LE(offsets[i], padding + table + 12 * i + 4);
      data.writeUInt32LE(payload, padding + table + 12 * i + 8);
      item.copy(data, padding + offsets[i]);
    });
    return data;
  }
  const a = program([0x24420001, ...epilogue]), b = program([0x00441021, 0x24420002, ...epilogue]);
  const audio = {
    'PSP_GAME/USRDIR/data/audio/aems/sfx/bank_b.abk': bank([b, a], 0x40),
    'PSP_GAME/USRDIR/data/audio/aems/music.abk': bank([a]),
    'PSP_GAME/USRDIR/data/audio/aems/sfx/bank_a.abk': bank([a, b]),
  };
  const disc = path.join(work, 'disc');
  for (const [name, data] of Object.entries(audio)) {
    mkdirSync(path.dirname(path.join(disc, name)), { recursive: true });
    writeFileSync(path.join(disc, name), data);
  }
  const pythonOut = path.join(work, 'native-sound');
  execFileSync('python3', ['-I', path.join(ROOT, 'scripts/generate_ncaa_sound.py'), disc, path.join(TOOLS, 'psp_recomp'), pythonOut], { stdio: 'pipe' });
  const programs = await T.findAudioPrograms(Object.entries(audio).map(([name, data]) => ({ path: name, read: async () => new Uint8Array(data) })));
  assert.equal(programs.length, 2);
  assert.deepEqual(programs[0].origins, ['PSP_GAME/USRDIR/data/audio/aems/music.abk@110', 'PSP_GAME/USRDIR/data/audio/aems/sfx/bank_a.abk@110', 'PSP_GAME/USRDIR/data/audio/aems/sfx/bank_b.abk@168']);
  const sound = await T.generateSound(createModule, programs);
  assert.deepEqual(sound.map(file => [file.path, sha(file.data)]), tree(pythonOut).map(file => [file.path, sha(file.data)]));
  await assert.rejects(T.findAudioPrograms([]), /No EA audio programs/);
  assert.equal(T.comparePaths('a/b', 'a.b'), -1);

  // Full ISO pipeline on a synthetic disc with the encrypted executable.
  const param = sfo('ULUS10440');
  const iso = new File([isoImage({ 'PSP_GAME/PARAM.SFO': param, 'PSP_GAME/SYSDIR/EBOOT.BIN': encrypted, ...audio })], 'synthetic.iso');
  const stages = [];
  const result = await T.translateIso(iso, { createModule, expectedSha256: sha(elf), progress: text => stages.push(text) });
  const byPath = new Map(result.files.map(file => [file.path, file.data]));
  assert.deepEqual(byPath.get('games/ncaa10/root/EBOOT.BIN'), elf);
  for (const file of wasmAuto.files) assert.deepEqual(byPath.get('profile/generated/ncaa10/' + file.path), file.data);
  for (const file of sound) assert.deepEqual(byPath.get('profile/generated/ncaa10_sound/' + file.path), file.data);
  assert.equal(result.provenance.executableSha256, sha(elf));
  assert.equal(result.provenance.encryptedExecutableSha256, sha(encrypted));
  assert.equal(result.provenance.audioPrograms, 2);
  assert.ok(stages.some(text => /Decrypting/.test(text)) && stages.some(text => /audio programs/.test(text)));
  await assert.rejects(T.translateIso(iso, { createModule, progress() {} }), /does not match the verified NCAA build/);
  const wrongTitle = new File([isoImage({ 'PSP_GAME/PARAM.SFO': sfo('ULUS10000'), 'PSP_GAME/SYSDIR/EBOOT.BIN': encrypted })], 'other.iso');
  await assert.rejects(T.translateIso(wrongTitle, { createModule, progress() {} }), /ULUS10440/);

  // ZIP: Python's zipfile reads every entry back with matching CRCs.
  const zipPath = path.join(work, 'translation.zip');
  writeFileSync(zipPath, Buffer.from(await new Blob(await T.zip(result.files)).arrayBuffer()));
  const listing = JSON.parse(execFileSync('python3', ['-c',
    'import json,sys,zipfile,hashlib;z=zipfile.ZipFile(sys.argv[1]);assert z.testzip() is None;print(json.dumps({i.filename:hashlib.sha256(z.read(i)).hexdigest() for i in z.infolist()}))', zipPath]));
  assert.deepEqual(listing, Object.fromEntries(result.files.map(file => [file.path, sha(file.data)])));
  console.log('PASS: Wasm decryption (plain/gzip, native-identical), PSPRecomp --auto and audio-program output identical to native, ISO pipeline, hash/title rejection and ZIP');
} finally {
  rmSync(work, { recursive: true, force: true });
}

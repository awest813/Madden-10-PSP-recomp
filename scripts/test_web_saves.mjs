// Exercise the actual browser save ZIP/parser/import transaction with synthetic data.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {deflateRawSync} from 'node:zlib';
import vm from 'node:vm';

const html = readFileSync(new URL('../profile/web/shell.html', import.meta.url), 'utf8');
const source = html.slice(html.indexOf('  var CRC_TABLE ='), html.indexOf("  document.getElementById('export-saves')"));
const root = '/game/ms0/PSP/SAVEDATA';
const disk = new Map([[root + '/BASE/DATA.BIN', new Uint8Array([7])]]);
const dirs = new Set([root, root + '/BASE']);
let failWrite = null, syncError = null, syncCalls = 0, notes = [];
const FS = {
  analyzePath(path) { return {exists: disk.has(path) || dirs.has(path)}; },
  readFile(path) { if (!disk.has(path)) throw Error('not a file'); return disk.get(path).slice(); },
  mkdirTree(path) { dirs.add(path); },
  writeFile(path, data) { if (path === failWrite) { failWrite = null; throw Error('write failure'); } disk.set(path, data.slice()); },
  unlink(path) { disk.delete(path); },
  rmdir(path) { assert(![...disk.keys()].some(key => key.startsWith(path + '/'))); dirs.delete(path); },
  syncfs(populate, cb) { assert.equal(populate, false); ++syncCalls; cb(syncError); },
};
const context = vm.createContext({TextEncoder, TextDecoder, Blob, Response, DecompressionStream, Set, FS, SAVE_ROOT:root});
vm.runInContext(source, context);
assert.equal(context.crc32(new TextEncoder().encode('123456789')), 0xcbf43926, 'standard CRC32 check vector');
const payload = new TextEncoder().encode('123456789');
// Independently constructed ZIP containing the standard CRC vector.
function zip(names = ['TEST/DATA.BIN'], {compressed = false, descriptor = false, declared = 9} = {}) {
  const local = [], central = []; let offset = 0;
  for (const name of names) {
    const encoded = Buffer.from(name), packed = compressed ? deflateRawSync(payload) : Buffer.from(payload);
    const flags = 0x800 | (descriptor ? 8 : 0), head = Buffer.alloc(30), entry = Buffer.alloc(46);
    head.writeUInt32LE(0x04034b50); head.writeUInt16LE(20,4); head.writeUInt16LE(flags,6); head.writeUInt16LE(compressed ? 8 : 0,8);
    if (!descriptor) { head.writeUInt32LE(0xcbf43926,14); head.writeUInt32LE(packed.length,18); head.writeUInt32LE(declared,22); }
    head.writeUInt16LE(encoded.length,26);
    entry.writeUInt32LE(0x02014b50); entry.writeUInt16LE(20,4); entry.writeUInt16LE(20,6); entry.writeUInt16LE(flags,8);
    entry.writeUInt16LE(compressed ? 8 : 0,10); entry.writeUInt32LE(0xcbf43926,16); entry.writeUInt32LE(packed.length,20);
    entry.writeUInt32LE(declared,24); entry.writeUInt16LE(encoded.length,28); entry.writeUInt32LE(offset,42);
    local.push(head, encoded, packed);
    offset += head.length + encoded.length + packed.length;
    if (descriptor) {
      const desc = Buffer.alloc(16); desc.writeUInt32LE(0x08074b50); desc.writeUInt32LE(0xcbf43926,4);
      desc.writeUInt32LE(packed.length,8); desc.writeUInt32LE(declared,12); local.push(desc); offset += 16;
    }
    central.push(entry, encoded);
  }
  const directory = Buffer.concat(central), end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50); end.writeUInt16LE(names.length,8); end.writeUInt16LE(names.length,10);
  end.writeUInt32LE(directory.length,12); end.writeUInt32LE(offset,16);
  return Buffer.concat([...local, directory, end]);
}
const array = bytes => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
for (const options of [{}, {compressed:true}, {compressed:true,descriptor:true}]) {
  const parsed = await context.readZip(array(zip(undefined,options)));
  assert.equal(parsed[0].name, 'TEST/DATA.BIN'); assert.deepEqual([...parsed[0].data], [...payload]);
}
const exported = context.makeZip([{name:'TEST/DATA.BIN',data:payload}]);
assert.deepEqual([...(await context.readZip(await exported.arrayBuffer()))[0].data], [...payload], 'export/import round trip');
const valid = zip(), corrupt = Buffer.from(valid); corrupt[30 + Buffer.byteLength('TEST/DATA.BIN')] ^= 1;
await assert.rejects(context.readZip(array(corrupt)), /CRC/);
await assert.rejects(context.readZip(array(valid.subarray(0,-1))), /ZIP/);
const badHeader = Buffer.from(valid); badHeader.writeUInt16LE(8,8);
await assert.rejects(context.readZip(array(badHeader)), /headers disagree/);
const badCentral = Buffer.from(valid); badCentral.writeUInt32LE(0xffffffff,badCentral.length-6);
await assert.rejects(context.readZip(array(badCentral)), /ZIP/);
await assert.rejects(context.readZip(array(zip(undefined,{compressed:true,declared:1}))), /expanded size/);
await assert.rejects(context.readZip(array(zip(undefined,{declared:17*1024*1024}))), /too large/);
for (const name of ['../DATA.BIN','PSP/SAVEDATA/../DATA.BIN','/TEST/DATA.BIN','C:/TEST/DATA.BIN','TEST/./DATA.BIN'])
  assert.throws(() => context.saveImportFiles([{name,data:payload}]), /unsafe/);
assert.throws(() => context.saveImportFiles([{name:'TEST/DATA.BIN',data:payload},{name:'PSP/SAVEDATA/test/data.bin',data:payload}]), /duplicate/);
const normalized = context.saveImportFiles([{name:'PSP/SAVEDATA/TEST/DATA.BIN',data:payload}]);
assert.equal(normalized[0].name, 'TEST/DATA.BIN');
const encryptedSfo = new Uint8Array(20 + 16 + 16 + 128), sfoView = new DataView(encryptedSfo.buffer);
sfoView.setUint32(0,0x46535000,true); sfoView.setUint32(8,36,true); sfoView.setUint32(12,52,true); sfoView.setUint32(16,1,true);
sfoView.setUint16(22,4,true); sfoView.setUint32(24,128,true);
encryptedSfo.set(new TextEncoder().encode('SAVEDATA_PARAMS\0'),36); encryptedSfo[52]=0x41;
const nativeRoster = [{name:'PSP/SAVEDATA/ULUS10440011/PARAM.SFO',data:encryptedSfo},
  {name:'PSP/SAVEDATA/ULUS10440011/DATA.BIN',data:payload}];
assert.throws(()=>context.saveImportFiles(nativeRoster),/Encrypted NCAA save/,'reject before the import mutates files');
encryptedSfo[52]=0;
assert.equal(context.saveImportFiles(nativeRoster).length,2,'decrypted save metadata is accepted');
assert.equal(context.saveImportFiles([{name:'PSP/SAVEDATA/TEST/',data:new Uint8Array()}]).length,0);
const duplicate = await context.readZip(array(zip(['TEST/DATA.BIN','PSP/SAVEDATA/test/data.bin'])));
assert.throws(() => context.saveImportFiles(duplicate), /duplicate/);
const overlap = zip(['TEST/DATA.BIN','TEST/DATA.BIN']);
const centralAt = overlap.readUInt32LE(overlap.length-6);
overlap.writeUInt32LE(0,centralAt + 46 + Buffer.byteLength('TEST/DATA.BIN') + 42);
await assert.rejects(context.readZip(array(overlap)), /overlap/);
failWrite = root + '/NEW/FAIL.BIN';
assert.throws(() => context.applySaveImport([{name:'BASE/DATA.BIN',data:payload},{name:'NEW/OK.BIN',data:payload},{name:'NEW/FAIL.BIN',data:payload}]), /write failure/);
assert.deepEqual([...disk], [[root + '/BASE/DATA.BIN', new Uint8Array([7])]], 'failed write restores existing files and removes newly created files');
assert(!dirs.has(root + '/NEW'));
const imported = context.applySaveImport(normalized); assert.equal(imported.count,1); imported.rollback();
assert(!disk.has(root + '/TEST/DATA.BIN'));
// Run the actual change handler, including persistence failure and startup guards.
const input = {files:[],value:'',addEventListener(event,callback) { this.change = callback; }};
Object.assign(context, {document:{getElementById() { return input; }}, Module:{pspReady:true}, note(text) { notes.push(text); },
  savesLoaded:true, saveStatusEl:{textContent:''}, console:{warn() {}}});
vm.runInContext(html.slice(html.indexOf('  var saveSync ='), html.indexOf('  var savesEl =')),context);
vm.runInContext(html.slice(html.indexOf('  var importInput ='), html.indexOf('</script>', html.indexOf('  var importInput ='))),context);
async function upload(bytes) { input.files=[{size:bytes.byteLength, async arrayBuffer() {return array(bytes);}}]; await input.change(); }
syncError = Error('storage failed'); await upload(valid);
assert(!disk.has(root + '/TEST/DATA.BIN')); assert.match(notes.at(-1), /Import failed.*storage failed/);
syncError = null; await upload(valid); assert.match(notes.at(-1), /Imported 1 save \(1 files\)/); assert.equal(syncCalls,2);
const snapshot = [...disk].map(([path,data]) => [path,[...data]]);
await upload(corrupt); assert.match(notes.at(-1), /CRC/);
assert.deepEqual([...disk].map(([path,data]) => [path,[...data]]),snapshot, 'bad archive leaves saves unchanged');
context.Module.pspReady = false; await upload(valid); assert.match(notes.at(-1), /startup/); assert.equal(syncCalls,2);
context.Module.pspReady = true;
let releaseAutosave;
FS.syncfs = (populate,callback) => {assert.equal(populate,false); releaseAutosave=callback;};
const earlierAutosave = context.persistSaves();
for (let i=0;i<10;++i) await Promise.resolve();
const queuedImport = upload(zip(['QUEUED/DATA.BIN']));
for (let i=0;i<20;++i) await Promise.resolve();
assert(!disk.has(root+'/QUEUED/DATA.BIN'),'an import must not mutate files inside an earlier autosave transaction');
FS.syncfs = (populate,callback) => {assert.equal(populate,false); callback(null);};
releaseAutosave(null); await earlierAutosave; await queuedImport;
assert(disk.has(root+'/QUEUED/DATA.BIN'));
context.savesLoaded=false; await upload(valid); assert.match(notes.at(-1),/storage is unavailable/);
context.savesLoaded=true;
let exportClick, downloads=0;
Object.assign(context,{
  saveRestoreDone:false,
  document:{getElementById(){return {addEventListener(event,callback){exportClick=callback;}};},
    createElement(){return {click(){++downloads;}};}},
  URL:{createObjectURL(){return 'blob:test';},revokeObjectURL(){}},setTimeout(){},
});
vm.runInContext(html.slice(html.indexOf("  document.getElementById('export-saves')"),html.indexOf('  var importInput =')),context);
exportClick(); assert.match(notes.at(-1),/finish loading/); assert.equal(downloads,0);
context.saveRestoreDone=true; context.importBusy=true;
exportClick(); assert.match(notes.at(-1),/import to finish/); assert.equal(downloads,0);
context.importBusy=false;
FS.readdir=()=>{throw Error('read denied');};
exportClick(); assert.match(notes.at(-1),/Export failed.*read denied/); assert.equal(downloads,0);
FS.readdir=()=>[];
exportClick(); assert.equal(notes.at(-1),'No saves yet');
FS.readdir=path=>[...new Set([...disk.keys(),...dirs].filter(p=>p.startsWith(path+'/')).map(p=>p.slice(path.length+1).split('/')[0]))];
FS.stat=path=>({mode:dirs.has(path)?1:0}); FS.isDir=mode=>mode===1;
exportClick(); assert.equal(downloads,1,'a completed restore can export a consistent snapshot');
console.log('PASS: save ZIP integrity/limits, safe paths, rollback, persistence errors, import waits for earlier autosaves, startup/storage/export guards and read-error reporting');

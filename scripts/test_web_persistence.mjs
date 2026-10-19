// Exercise the actual browser storage startup and persistence queue.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

const html = readFileSync(new URL('../profile/web/shell.html', import.meta.url), 'utf8');
const mount = html.slice(html.indexOf('  function mountSaves()'), html.indexOf('  // Which GPU'));
const persistence = html.slice(html.indexOf('  var saveSync ='), html.indexOf('  var savesEl ='));
function boot(mode = 'success') {
  const state = {dependencies:0, added:0, removed:0, writes:0, callback:null, timers:new Map(), serial:0};
  const context = vm.createContext({
    FS: {
      mkdirTree() {if (mode === 'mkdir') throw Error('mkdir denied');},
      mount() {if (mode === 'mount') throw Error('mount denied');},
      syncfs(populate, callback) {
        assert.equal(populate,true); state.callback = callback;
        if (mode === 'throw') throw Error('storage denied');
        if (mode !== 'async') callback(mode === 'failure' ? Error('restore failed') : null);
      },
    },
    IDBFS: {}, SAVE_ROOT: '/game/ms0/PSP/SAVEDATA', Module: {},
    addRunDependency() {++state.dependencies; ++state.added;},
    removeRunDependency() {--state.dependencies; ++state.removed;},
    console: {warn() {}}, saveStatusEl: {textContent:''}, savesLoaded:false,
    setTimeout(callback) {state.timers.set(++state.serial,callback); return state.serial;},
    clearTimeout(id) {state.timers.delete(id);},
  });
  vm.runInContext(mount + '\nmountSaves();\n' + persistence,context);
  return {context,state};
}
for (const mode of ['mkdir','mount','throw','failure','success']) {
  const {context,state} = boot(mode);
  assert.equal(state.dependencies,0,'every startup failure must release its dependency');
  assert.equal(state.added,state.removed,'only registered dependencies may be released');
  assert.equal(context.savesLoaded,mode === 'success');
  if (mode !== 'success') assert.match(context.saveStatusEl.textContent,/Export saves/);
  // Some broken storage wrappers throw and still invoke their callback later.
  state.callback?.(null);
  assert.equal(state.dependencies,0,'startup completion must run exactly once');
  assert.equal(context.savesLoaded,mode === 'success','a late callback cannot undo a failed restore');
}
const delayed = boot('async');
assert.equal(delayed.state.dependencies,1,'startup must wait for asynchronous restore');
delayed.state.callback(null);
assert.equal(delayed.state.dependencies,0);
assert.equal(delayed.context.savesLoaded,true);

const {context,state} = boot();
let active = 0, maximum = 0;
const pending = [];
context.FS.syncfs = (populate,callback) => {
  assert.equal(populate,false); ++state.writes; maximum = Math.max(maximum,++active);
  pending.push(error => {--active; callback(error);});
};
async function flush() {for (let i = 0; i < 10; ++i) await Promise.resolve();}
const first = context.persistSaves().catch(error => error.message);
const second = context.persistSaves();
await flush();
assert.equal(state.writes,1,'simultaneous save requests must not overlap IDBFS transactions');
pending.shift()(Error('quota exceeded'));
await flush();
assert.equal(await first,'quota exceeded');
assert.match(context.saveStatusEl.textContent,/could not be stored.*Export saves/);
assert.equal(state.writes,2,'a rejected write must allow the queued save to run');
pending.shift()(null); await second;
assert.equal(maximum,1);
assert.equal(context.saveStatusEl.textContent,'','successful retry clears the persistent storage warning');

context.FS.syncfs = () => {throw Error('storage threw');};
await assert.rejects(context.persistSaves(),/storage threw/);
assert.match(context.saveStatusEl.textContent,/Export saves/);
context.FS.syncfs = (populate,callback) => {assert.equal(populate,false); ++state.writes; callback(null);};
await context.persistSaves();
assert.equal(context.saveStatusEl.textContent,'');
context.Module.pspSavesChanged(); context.Module.pspSavesChanged(); context.Module.pspSavesChanged();
assert.equal(state.timers.size,1,'rapid guest save notifications are debounced');
const before = state.writes;
state.timers.values().next().value(); state.timers.clear(); await flush();
assert.equal(state.writes,before+1,'debounced autosave uses the same persistence queue');

const failedRestore = boot('failure');
failedRestore.context.FS.syncfs = () => {throw Error('must not overwrite unrestored browser data');};
await assert.rejects(failedRestore.context.persistSaves(),/not restored/);
const abortSource = html.match(/onAbort: function \(\) \{([\s\S]*?)\n    \},/)[1];
const retry={hidden:true};
const aborted={Module:{pspReady:true},statusEl:{},document:{getElementById:()=>retry}};
vm.runInNewContext(abortSource,aborted);
assert.equal(aborted.Module.pspReady,false,'a failed engine must stop accepting guest input/imports');
assert.equal(aborted.statusEl.className,'status err');
assert.match(aborted.statusEl.textContent,/Reload.*retry/);
assert.equal(retry.hidden,false,'an aborted engine exposes a native retry button');
console.log('PASS: browser-save startup dependency cleanup, async restore, persistent errors, serialized writes, retry recovery, autosave debounce and failed-restore write protection');

// Execute the shell's actual controller poller against synthetic devices.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const html = readFileSync(new URL('../profile/web/shell.html', import.meta.url), 'utf8');
const source = html.slice(html.indexOf('  function pollInput()'), html.indexOf("  var noteEl ="));
const mapping = html.match(/var PAD_BUTTONS = \[[^\]]+\];/)[0];
let pads = [], calls = [], queued = 0;
const events = {};
const context = vm.createContext({
  Module: { pspReady: true, _pspweb_set_pad(...args) { calls.push(args); } },
  document: { hidden: false }, window: { addEventListener(name, fn) { events[name] = fn; } },
  navigator: { getGamepads() { return pads; } }, requestAnimationFrame() { ++queued; },
  held: {}, KEYS: { KeyZ: 0x4000 }, STICK: { KeyI: [0, -1] },
  fingers: {}, knob: null, inputFocused: true,
  touchPad: { buttons: 0, x: 0, y: 0, stick: false },
  saveUi: { info: null }, oskUi: null, padPrev: {}, padQuiet: false,
  applyScale() {},
});
const focusSource = html.slice(html.indexOf('  function releaseInput()'), html.indexOf('  // Touch controls.'));
const keySource = html.slice(html.indexOf('  function onKey('), html.indexOf("  window.addEventListener('keydown'"));
vm.runInContext(mapping + focusSource + keySource + source, context);
let prevented = 0;
const key = (code, interactive = false) => ({code, target: {closest: () => interactive}, preventDefault() {++prevented;}});
context.Module.pspReady = false;
context.onKey(key('Enter'), true);
assert.equal(prevented, 0, 'Enter must open the native ISO picker before startup');
context.Module.pspReady = true;
context.onKey(key('KeyZ'), true);
assert.equal(context.held.KeyZ, true);
context.onKey(key('KeyZ', true), false);
assert.equal(context.held.KeyZ, undefined, 'keyup in a native control releases a prior game key');
context.onKey(key('Enter', true), true);
assert.equal(context.held.Enter, undefined, 'focused buttons must keep their native Enter action');
function poll() { context.pollInput(); return calls.at(-1); }
function controller(button = -1, axes = [0, 0]) {
  return { buttons: Array.from({ length: 16 }, (_, i) => ({ pressed: i === button })), axes };
}
const expected = [0x4000, 0x2000, 0x8000, 0x1000, 0x100, 0x200, 0, 0, 1, 8, 0, 0, 0x10, 0x40, 0x80, 0x20];
for (let button = 0; button < expected.length; ++button) {
  pads = [null, controller(button)];
  assert.deepEqual(poll(), [expected[button], 128, 128]);
}
pads = [controller(-1, [-1, 1])];
assert.deepEqual(poll(), [0, 1, 255]);
pads = [controller(-1, [0.1, -0.1])];
assert.deepEqual(poll(), [0, 128, 128], 'stick deadzone');
pads = [controller(0, [1, -1])];
context.padQuiet = true;
assert.deepEqual(poll(), [0, 128, 128], 'dialog close suppresses held button and stick');
pads = [controller(-1, [1, 0])];
assert.deepEqual(poll(), [0, 128, 128], 'releasing button alone must not rearm stick');
pads = [controller()];
assert.deepEqual(poll(), [0, 128, 128]);
pads = [controller(0, [1, -1])];
assert.deepEqual(poll(), [0x4000, 255, 1], 'neutral sample rearms input');
events.blur();
assert.deepEqual(poll(), [0, 128, 128], 'controller must remain neutral while unfocused');
events.focus();
assert.deepEqual(poll(), [0, 128, 128], 'held controller must not resume on focus');
pads = [controller()]; poll();
pads = [controller(1)];
assert.deepEqual(poll(), [0x2000, 128, 128]);
pads = [];
assert.deepEqual(poll(), [0, 128, 128], 'disconnect releases controller');
context.held.KeyZ = true; context.held.KeyI = true;
assert.deepEqual(poll(), [0x4000, 128, 1], 'keyboard remains available without a controller');
context.document.hidden = true;
assert.deepEqual(poll(), [0, 128, 128], 'hidden page cannot send controls');
context.Module.pspReady = false;
const before = calls.length; poll();
assert.equal(calls.length, before, 'no guest exports before startup');
assert(queued > 10, 'poll loop continues across transitions');
console.log('PASS: standard controller buttons/stick/deadzone, dialog neutral rearm, blur/focus, disconnect, keyboard, hidden/startup guards');

// Execute the actual dialog code against a minimal DOM and WebAssembly bridge.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
const html = readFileSync(new URL('../profile/web/shell.html', import.meta.url), 'utf8');
const source = html.slice(html.indexOf('var oskUi = null;'), html.indexOf('  function pollSaveDialog'));
class Element {
  children = []; listeners = {}; hidden = true; value = ''; maxLength = 0;
  set textContent(text) { this.text = text; this.children = []; }
  get textContent() { return this.text; }
  appendChild(child) { this.children.push(child); }
  addEventListener(name, fn) { this.listeners[name] = fn; }
  focus() { this.focused = true; }
  select() { this.selected = true; }
}
const elements = new Map(['osk', 'osk-fields', 'osk-form', 'osk-cancel'].map(id => [id, new Element()]));
const oldFocus = new Element(), fields = [], completions = [], freed = [];
const heap = new Uint16Array(1024);
let released = 0;
const context = vm.createContext({
  document: { activeElement: oldFocus, getElementById(id) { return elements.get(id); }, createElement() { return new Element(); } },
  HEAPU16: heap, padQuiet: false, padPrev: {}, releaseInput() { ++released; },
  Module: {
    _malloc() { return 32; }, _free(ptr) { freed.push(ptr); },
    _pspweb_osk_field(index, ptr, length) { fields.push([index, String.fromCharCode(...heap.subarray(ptr >> 1, (ptr >> 1) + length))]); },
    _pspweb_osk_complete(canceled) { completions.push(canceled); },
  },
});
vm.runInContext(source, context);
context.Module.pspOskDialog([{ label: 'Dynasty name <test>', initial: 'NCAATEST', limit: 8 }]);
assert.equal(elements.get('osk').hidden, false);
assert.equal(elements.get('osk-fields').children[0].textContent, 'Dynasty name <test>');
let input = elements.get('osk-fields').children[0].children[0];
assert.equal(input.value, 'NCAATEST');
assert.equal(input.maxLength, 8);
assert(input.focused && input.selected && context.padQuiet);
assert.equal(context.padPrev.ok, true, 'opening controller press must be released first');
input.value = 'WEB\u{1F3C8}TEST';
elements.get('osk-form').listeners.submit({ preventDefault() {} });
assert.deepEqual(fields, [[0, 'WEB\u{1F3C8}TES']]);
assert.deepEqual(completions, [0]);
assert.deepEqual(freed, [32]);
assert.equal(elements.get('osk').hidden, true);
assert(oldFocus.focused);
context.Module.pspOskDialog([{ label: '', initial: 'AB', limit: 2 }]);
elements.get('osk-cancel').listeners.click();
assert.deepEqual(completions, [0, 1]);
assert.equal(fields.length, 1, 'cancel must not submit edited fields');
assert.equal(released, 4, 'open and close release guest controls');
console.log('PASS: actual OSK DOM, labels/initial text, UTF-16 submission/bounds, cancel, focus and held-controller suppression');

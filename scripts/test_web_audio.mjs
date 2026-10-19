// Exercise the actual shell AudioWorklet, without a browser or game assets.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const html = readFileSync(new URL('../profile/web/shell.html', import.meta.url), 'utf8');
for (const script of html.matchAll(/<script>([\s\S]*?)<\/script>/g)) new vm.Script(script[1]);
const releaseSource = html.match(/function releaseInput\(\) \{[\s\S]*?\n  \}/)[0];
const padCalls = [];
const input = {
  held: { KeyI: true, KeyZ: true }, fingers: { 1: {} },
  touchPad: { buttons: 16384, x: 1, y: -1, stick: true },
  knob: { style: { transform: 'translate(20px, -20px)' } },
  Module: { pspReady: true, _pspweb_set_pad(...args) { padCalls.push(args); } },
};
vm.runInNewContext(releaseSource + '; releaseInput();', input);
assert.equal(Object.keys(input.held).length, 0);
assert.equal(Object.keys(input.fingers).length, 0);
assert.equal(input.touchPad.buttons, 0);
assert.equal(input.touchPad.stick, false);
assert.equal(input.knob.style.transform, '');
assert.deepEqual(padCalls, [[0, 128, 128]]);
input.Module.pspReady = false;
vm.runInNewContext(releaseSource + '; releaseInput();', input);
assert.equal(padCalls.length, 1, 'focus loss before wasm startup must not call guest exports');
const visibilitySource = html.match(/document.addEventListener\('visibilitychange', function \(\) \{[\s\S]*?\n  \}\);/)[0];
let visibilityCallback, pacingResets = 0;
const visibility = {
  frames: 12, cpuTotal: 5, stepTotal: 24, lastFps: 1,
  performance: { now: () => 99 },
  document: { addEventListener(name, cb) { assert.equal(name, 'visibilitychange'); visibilityCallback = cb; } },
  Module: { pspReady: true, _pspweb_reset_frame_pacer() { ++pacingResets; } },
};
vm.runInNewContext(visibilitySource, visibility);
visibilityCallback();
assert.equal(pacingResets, 1, 'visibility changes reset guest pacing even if hidden rAF never ran');
assert.deepEqual([visibility.frames, visibility.cpuTotal, visibility.stepTotal, visibility.lastFps], [0, 0, 0, 99]);
visibility.Module.pspReady = false;
visibilityCallback();
assert.equal(pacingResets, 1, 'visibility reset must respect wasm startup');
const start = html.indexOf('var WORKLET = [');
const end = html.indexOf("].join('\\n');", start) + "].join('\\n');".length;
assert(start >= 0 && end > start);
const scope = {};
vm.runInNewContext(html.slice(start, end), scope);
let Output;
vm.runInNewContext(scope.WORKLET, {
  AudioWorkletProcessor: class { constructor() { this.port = { postMessage() {} }; } },
  registerProcessor(name, implementation) { assert.equal(name, 'psp-output'); Output = implementation; },
});
function push(output, count, value = 0.25) {
  const samples = new Float32Array(count * 2);
  for (let i = 0; i < count; ++i) { samples[i * 2] = value; samples[i * 2 + 1] = -value; }
  output.port.onmessage({ data: samples });
}
function render(output) {
  const channels = [new Float32Array(128), new Float32Array(128)];
  assert.equal(output.process([], [channels]), true);
  for (const channel of channels) assert(channel.every(Number.isFinite));
  return channels;
}
for (const rate of [22050, 44100, 48000, 96000]) {
  const output = new Output({ processorOptions: { rate } });
  push(output, 2048);
  assert(render(output)[0].every(x => x === 0), 'startup must reserve enough audio for callback jitter');
  push(output, 3072);
  assert(render(output)[0].every(x => x === 0.25));
  // Simulate ten seconds of PSP production, with bursts every three host
  // audio blocks and varying chunk lengths. Check rate conversion and drift.
  let produced = 0;
  const blocks = Math.floor(rate * 10 / 128);
  for (let block = 1; block <= blocks; ++block) {
    if (block % 3 === 0) {
      const required = Math.floor(block * 128 * 44100 / rate);
      push(output, required - produced);
      produced = required;
    }
    const [left, right] = render(output);
    assert(left.every(x => x === 0.25) && right.every(x => x === -0.25), 'stereo/chunk boundary continuity');
    assert(output.frames >= 0 && output.frames <= 11025);
  }
  assert.equal(output.under, 0, 'steady 44.1 kHz producer must not underrun');
  output.port.onmessage({ data: { reset: true } });
  assert.equal(output.frames, 0);
  assert.equal(output.pos, 0);
  assert.equal(output.drift, 0);
  assert(render(output)[0].every(x => x === 0), 'reset must discard stale samples');
  assert.equal(output.under, 0, 'intentional silence is not an underrun');
  push(output, 5120, 0.5);
  assert(render(output)[0].every(x => x === 0.5), 'resume primes fresh audio');
  for (let i = 0; i < rate / 128; ++i) render(output);
  assert.equal(output.under, 1, 'one starvation event, rather than one event per silent sample');
  assert.equal(output.frames, 0);
  assert.equal(output.pos, 0);
  push(output, 11000);
  push(output, 11000);
  assert(output.frames <= 11025, 'excess latency must remain bounded');
}
// A low output rate can cross several source chunks in one output sample.
// Splitting the same stereo ramp must not change resampling or phase.
for (const rate of [22050, 44100, 48000, 96000]) {
  const contiguous = new Output({ processorOptions: { rate } });
  const split = new Output({ processorOptions: { rate } });
  contiguous.target = split.target = 1;
  const ramp = Float32Array.from({ length: 2000 }, (_, i) => (i % 2 ? -1 : 1) * Math.floor(i / 2) / 1000);
  contiguous.port.onmessage({ data: ramp });
  for (let i = 0; i < ramp.length; i += 2) split.port.onmessage({ data: ramp.slice(i, i + 2) });
  for (let block = 0; block < 3; ++block) {
    const expected = render(contiguous), actual = render(split);
    for (let channel = 0; channel < 2; ++channel) {
      assert.deepEqual(actual[channel], expected[channel], 'chunking must preserve interpolated stereo samples');
    }
    assert.equal(split.frames, contiguous.frames, 'chunking must preserve queued frame accounting');
  }
}
const oversized = new Output({ processorOptions: { rate: 44100 } });
push(oversized, 30000);
assert(oversized.frames <= 11025, 'a single oversized chunk must also respect the latency cap');
assert.equal(oversized.over,1);
assert.equal(oversized.dropped,30000-11025,'count every discarded source frame');
const trimmed = new Output({ processorOptions: { rate: 48000 } });
push(trimmed,6000); render(trimmed);
const remaining = 6000-Math.floor(trimmed.pos);
assert(trimmed.pos % 1 !== 0,'exercise a fractional resampling position');
push(trimmed,6000);
assert.equal(trimmed.frames,6000);
assert.equal(trimmed.dropped,remaining,'trimming must count source frames, not fractional phase');
assert.equal(trimmed.over,1);

// Exercise actual context creation and stats delivery with a 48 kHz device.
// A stalled render thread must not be mistaken for an overproducing guest.
let clock = 1000, contextOptions;
const audioScope = {
  document:{hidden:false}, sound:{context:null,muted:false}, WORKLET:scope.WORKLET,
  window:{AudioContext:class {
    constructor(options) {contextOptions=options; this.sampleRate=48000; this.audioWorklet={addModule:async()=>{}};}
    createGain() {return {gain:{value:0},connect(){}};}
  },AudioWorkletNode:true},
  AudioWorkletNode:class {constructor(){this.port={};} connect(gain){return gain;}},
  URL:{createObjectURL(){return 'blob:test';}},Blob:class {},
  performance:{now:()=>clock}, console,
};
vm.runInNewContext(html.slice(html.indexOf('  function startSound()'),html.indexOf("  ['pointerup'")),audioScope);
audioScope.startSound(); await Promise.resolve();
assert.equal(contextOptions.sampleRate,undefined,'use the device graph rate instead of forcing a second conversion');
assert.equal(contextOptions.latencyHint,'interactive');
const stats = (received,output) => audioScope.sound.node.port.onmessage({data:{received,output}});
stats(44100,48000); clock+=1500; stats(88200,96000);
assert.equal(audioScope.sound.stats.inputHz,29400,'producer rate uses elapsed wall time');
assert.equal(audioScope.sound.stats.renderHz,32000,'expose missed device deadlines independently');
clock+=1000; stats(0,0);
assert.equal(audioScope.sound.stats.inputHz,undefined,'reset counters must not yield negative rates');
clock+=1000; stats(44100,48000);
assert.equal(audioScope.sound.stats.inputHz,44100);
assert.equal(audioScope.sound.stats.renderHz,48000);
console.log('PASS: focus/startup guards, stereo/chunk boundaries, 22.05/44.1/48/96 kHz, drift, reset/starvation recovery, bounded queue with exact drop counts, device-native graph selection and wall-time producer/render diagnostics');

// Verify the actual reference CLI against the documented asynchronous protocol.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {resolve,sep} from 'node:path';
import vm from 'node:vm';
const source = readFileSync(new URL('./debug_ppsspp_reference.mjs',import.meta.url),'utf8').replace(/^import .*;\r?\n/gm,'');
let stepping = false, output = [], commands = [], captures = [], endpoints = [];
let gpuMode = 'fail';
const png = Buffer.from('89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000b49444154789c636000020000050001a5f645400000000049454e44ae426082','hex');
class Socket {
  handlers = {};
  constructor(url) { endpoints.push(url); queueMicrotask(() => this.emit('open',{})); }
  addEventListener(name,fn) { (this.handlers[name] ||= []).push(fn); }
  emit(name,value) { for (const fn of this.handlers[name] || []) fn(value); }
  send(json) {
    const req = JSON.parse(json); commands.push(req.event);
    let reply = {event:req.event,ticket:req.ticket};
    if (req.event === 'version') reply.version='synthetic';
    if (req.event === 'cpu.status') reply.stepping=stepping;
    if (req.event === 'cpu.stepping' || req.event === 'cpu.resume' || req.event === 'cpu.stepInto') {
      stepping = req.event !== 'cpu.resume';
      reply = {event:stepping ? 'cpu.stepping' : 'cpu.resume'}; // No ticket, per PPSSPP.
    }
    if (req.event === 'gpu.buffer.renderColor') {
      if (gpuMode === 'fail') reply = {event:'error',ticket:req.ticket,message:'Could not download output'};
      else if (req.type === 'uri') reply.uri = 'data:image/png;base64,' + (gpuMode === 'bad-png' ? Buffer.from('not png') : png).toString('base64');
      else Object.assign(reply,{base64:'AQIDBA==',width:1,height:1,format:3,flipped:true});
    }
    if (req.event === 'memory.read') reply.base64 = 'AQID';
    queueMicrotask(() => this.emit('message',{data:JSON.stringify(reply)}));
  }
  close() { this.closed = true; }
}
const script = new vm.Script('(async function(){'+source+'})()');
const context = vm.createContext({WebSocket:Socket,resolve,sep,Buffer,setTimeout,clearTimeout,
  process:{argv:['node','script','synthetic.jsonl'],stdin:{}},
  console:{log(line) { output.push(JSON.parse(line)); }},
  async appendFile() {}, async writeFile(path,bytes) { captures.push([path,[...Buffer.from(bytes)]]); },
  createInterface() { return (async function*() {
    yield JSON.stringify([{event:'cpu.stepping'},{event:'cpu.stepping'},{event:'cpu.stepInto'},
      {event:'gpu.buffer.renderColor'},{event:'cpu.resume'},{event:'cpu.resume'},
      {event:'memory.read',address:0,size:3,saveBase64As:'synthetic.bin'}]);
  })(); },
});
await script.runInContext(context);
assert.deepEqual(commands, ['version','cpu.status','cpu.stepping','cpu.status','cpu.stepInto',
  'gpu.buffer.renderColor','cpu.status','cpu.resume','cpu.status','memory.read']);
assert(output.some(item => item.error === 'Could not download output'));
assert(!output.some(item => item.error?.includes('timeout')));
assert.equal(stepping,false,'failed capture must still execute the following resume');
assert.deepEqual(captures[0][1],[1,2,3]);
console.log('PASS: unticketed pause/step/resume events, already-paused/running guards, failed-capture resume and binary capture');

// Exercise the actual CLI in a separate port/profile with typed GPU captures.
commands=[];captures=[];output=[];endpoints=[];gpuMode='ok';
context.process.env={PSPWEB_REFERENCE_PORT:'8673'};
context.createInterface=()=> (async function*(){
 yield JSON.stringify([
  {event:'gpu.buffer.renderColor',type:'base64',savePNGAs:'frame.png'},
  {event:'gpu.buffer.renderColor',type:'base64',saveBase64As:'frame.raw'},
  {event:'gpu.buffer.renderColor',type:'base64',saveBase64As:'wrong.png'},
  {event:'memory.read',size:3,savePNGAs:'wrong-memory.png'},
 ]);
})();
await script.runInContext(context);
assert.equal(endpoints[0],'ws://127.0.0.1:8673/debugger');
const stored=name=>captures.find(([path])=>path.endsWith(sep+name));
assert.deepEqual(stored('frame.png')?.[1],[...png],'PNG capture must request an encoded URI even when raw type was supplied');
assert.deepEqual(stored('frame.raw')?.[1],[1,2,3,4]);
const metadata=JSON.parse(Buffer.from(stored('frame.raw.json')[1]).toString());
assert.equal(metadata.format,3);assert.equal(metadata.flipped,true);assert.equal(metadata.width,1);
assert.equal(metadata.base64,undefined,'raw metadata sidecar must not duplicate the pixel payload');
assert(!stored('wrong.png'),'raw GPU data must not be mislabeled as PNG');
assert(!stored('wrong-memory.png'),'PNG capture is only supported for GPU images');
assert(output.some(r=>r.error?.includes('savePNGAs')));
captures=[];output=[];gpuMode='bad-png';
context.createInterface=()=> (async function*(){yield JSON.stringify({event:'gpu.buffer.renderColor',savePNGAs:'bad.png'});})();
await script.runInContext(context);
assert.equal(captures.length,0,'invalid encoded image must not be written');
assert(output.some(r=>r.error?.includes('PNG')));
context.process.env.PSPWEB_REFERENCE_PORT='8673/remote';
await assert.rejects(script.runInContext(context),/port/i);
console.log('PASS: isolated loopback port, encoded PNG validation, raw image metadata and invalid capture rejection');

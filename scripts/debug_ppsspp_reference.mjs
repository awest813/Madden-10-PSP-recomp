// Local, reversible debugger controls for the official PPSSPP reference.
// Uses the documented v1.20.4 protocol; never writes guest memory/registers.
// JSON requests arrive on stdin. Logs and optional binary captures stay in build/.
import {createInterface} from 'node:readline';
import {appendFile, writeFile} from 'node:fs/promises';
import {resolve, sep} from 'node:path';
const allowed = new Set(['version','cpu.status','cpu.evaluate','cpu.getAllRegs',
  'cpu.getReg','cpu.stepping','cpu.resume','cpu.runUntil','cpu.stepInto',
  'cpu.breakpoint.add','cpu.breakpoint.update','cpu.breakpoint.remove','cpu.breakpoint.list',
  'input.buttons.press','input.buttons.send','input.analog.send',
  'hle.module.list','hle.thread.list','memory.mapping','memory.info.list',
  'memory.read','memory.read_u32','gpu.buffer.screenshot','gpu.buffer.renderColor']);
const outputRoot = resolve('build/reference-ppsspp');
const eventsPath = resolve(outputRoot, process.argv[2] || 'heap-events.jsonl');
const port = process.env?.PSPWEB_REFERENCE_PORT || '8672';
if (!/^\d+$/.test(port) || Number(port) < 1 || Number(port) > 65535)
  throw new Error('Invalid reference debugger port');
const ws = new WebSocket(`ws://127.0.0.1:${Number(port)}/debugger`,'debugger.ppsspp.org');
const pending = new Map();
let serial = 0, logChain = Promise.resolve();
ws.addEventListener('message', e => {
  const r = JSON.parse(e.data);
  // CPU run/step commands complete through unticketed broadcasts, not replies.
  // Only match broadcasts to commands that explicitly expect that event.
  const key = r.ticket ?? [...pending].find(([, p]) => p.completion === r.event)?.[0];
  const p = pending.get(key);
  if(p) {pending.delete(key); clearTimeout(p.timer); p.resolve(r);}
  else {
    logChain = logChain.then(() => appendFile(eventsPath,JSON.stringify(r)+'\n'));
    if(r.event === 'cpu.stepping' || r.event === 'error')
      console.log(JSON.stringify(r));
  }
});
await new Promise((res,rej) => {
  ws.addEventListener('open',res,{once:true}); ws.addEventListener('error',rej,{once:true});
  setTimeout(()=>rej(new Error('Connection timeout')),10000).unref();
});
async function request(input) {
  if(!allowed.has(input.event)) throw new Error('Unsupported reference command');
  const {saveBase64As,savePNGAs,saveJSONAs,sendOnly,...wire} = input;
  const gpuImage = wire.event.startsWith('gpu.buffer.');
  if (savePNGAs) {
    if (!gpuImage) throw new Error('savePNGAs requires a GPU buffer command');
    wire.type = 'uri'; // Protocol base64 is raw pixels, not an encoded image.
  }
  if (!sendOnly && (wire.event === 'cpu.stepping' || wire.event === 'cpu.resume')) {
    const status = await request({event:'cpu.status'});
    if (status.stepping === (wire.event === 'cpu.stepping')) return status;
  }
  const ticket = `heap-${++serial}`;
  if(sendOnly) {ws.send(JSON.stringify({...wire,ticket})); return {sent:wire.event};}
  const r = await new Promise((resolve,reject)=> {
    const timer=setTimeout(()=>{pending.delete(ticket);reject(new Error('Request timeout: '+wire.event));},10000);
    const completion = ['cpu.stepping','cpu.stepInto','cpu.runUntil'].includes(wire.event)
      ? 'cpu.stepping' : wire.event === 'cpu.resume' ? 'cpu.resume' : null;
    pending.set(ticket,{resolve,timer,completion}); ws.send(JSON.stringify({...wire,ticket}));
  });
  if(r.event === 'error') throw new Error(r.message);
  if(saveJSONAs) {
    const path=resolve(outputRoot,saveJSONAs);
    if(!path.startsWith(outputRoot+sep)) throw new Error('Capture must stay in the reference output directory');
    await writeFile(path,JSON.stringify(r,null,2)+'\n');
    return {event:r.event,path};
  }
  if(savePNGAs) {
    const path=resolve(outputRoot,savePNGAs);
    if(!path.startsWith(outputRoot+sep)) throw new Error('Capture must stay in the reference output directory');
    if(!r.uri?.startsWith('data:image/png;base64,')) throw new Error('No PNG URI in capture response');
    const bytes=Buffer.from(r.uri.slice('data:image/png;base64,'.length),'base64');
    if(!bytes.subarray(0,8).equals(Buffer.from('89504e470d0a1a0a','hex')))
      throw new Error('Invalid PNG signature in capture response');
    await writeFile(path,bytes);
    return {event:r.event,path,bytes:bytes.length};
  }
  if(saveBase64As) {
    const path=resolve(outputRoot,saveBase64As);
    if(!path.startsWith(outputRoot+sep)) throw new Error('Capture must stay in the reference output directory');
    const data=r.base64 || (r.uri?.startsWith('data:') ? r.uri.split(',')[1] : null);
    if(!data) throw new Error('No base64 data in capture response: '+Object.keys(r));
    const bytes=Buffer.from(data,'base64');
    if(gpuImage && r.base64 && /\.png$/i.test(path))
      throw new Error('Raw GPU pixels require their format metadata; use savePNGAs for PNG');
    await writeFile(path,bytes);
    if(gpuImage) {
      const {base64,uri,...metadata}=r;
      await writeFile(path+'.json',JSON.stringify(metadata,null,2)+'\n');
    }
    return {event:r.event,path,bytes:bytes.length};
  }
  return r;
}
console.log(JSON.stringify(await request({event:'version'})));
const lines=createInterface({input:process.stdin});
try {
  for await(const line of lines) {
    if(!line.trim())continue;
    try {
      const commands=JSON.parse(line);
      for(const command of Array.isArray(commands)?commands:[commands]) {
        // A failed capture must not discard the following resume command.
        try { console.log(JSON.stringify(await request(command))); }
        catch(e) { console.log(JSON.stringify({error:e.message})); }
      }
    } catch(e) {console.log(JSON.stringify({error:e.message}));}
  }
} finally {await logChain; ws.close();}

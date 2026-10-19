// Read-only client for the official PPSSPP WebSocket debugger protocol.
// This is a reference-analysis tool; the port does not depend on PPSSPP.
import { writeFile } from 'node:fs/promises';

const allowed = new Set(['version', 'cpu.status', 'cpu.evaluate', 'cpu.getReg',
  'cpu.getAllRegs', 'hle.thread.list', 'hle.module.list', 'hle.backtrace',
  'memory.mapping', 'memory.info.list', 'memory.info.search', 'memory.read',
  'memory.read_u8', 'memory.read_u16', 'memory.read_u32', 'memory.readString',
  'gpu.buffer.screenshot']);
const requests = JSON.parse(process.argv[2] || '[{"event":"hle.module.list"},{"event":"hle.thread.list"},{"event":"memory.info.list","address":142606336,"size":25165824,"type":"alloc"}]');
if (!Array.isArray(requests) || requests.some(r => !allowed.has(r.event)))
  throw new Error('Only documented read-only reference requests are allowed');
const ws = new WebSocket('ws://127.0.0.1:8672/debugger', 'debugger.ppsspp.org');
const pending = new Map();
ws.addEventListener('message', e => {
  const result = JSON.parse(e.data);
  const waiter = pending.get(result.ticket);
  if (waiter) { pending.delete(result.ticket); waiter(result); }
});
await new Promise((resolve, reject) => {
  ws.addEventListener('open', resolve, { once: true });
  ws.addEventListener('error', reject, { once: true });
  setTimeout(() => reject(new Error('Reference connection timed out')), 10000).unref();
});
try {
  const results = [];
  // Identify the reference before examining any guest data.
  for (const [i, request] of [{event: 'version'}, ...requests].entries()) {
    const ticket = `reference-${i}`;
    const result = await new Promise((resolve, reject) => {
      pending.set(ticket, resolve);
      setTimeout(() => reject(new Error(`Reference request timed out: ${request.event}`)), 10000).unref();
      ws.send(JSON.stringify({...request, ticket}));
    });
    if (result.event === 'error') throw new Error(result.message);
    results.push(result);
  }
  if (process.argv[3]) await writeFile(process.argv[3], JSON.stringify(results, null, 2) + '\n');
  else console.log(JSON.stringify(results, null, 2));
} finally {
  ws.close();
}

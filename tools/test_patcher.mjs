// node tools/test_patcher.mjs [path/to/Spel2.exe]
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';
import assert from 'node:assert/strict';
import os from 'node:os';

const here = path.dirname(fileURLToPath(import.meta.url));
const html = fs.readFileSync(path.join(here, '..', 'index.html'), 'utf8');
const m = html.match(/<script id="core">([\s\S]*?)<\/script>/);
assert(m, 'core script not found');
const P = new Function(m[1] + '\n;return Spel2Patch;')();

const exe = process.argv[2] || path.join(here, '..', 'Spel2.exe');
const SCRATCH = fs.mkdtempSync(path.join(os.tmpdir(), 'spel2-patch-'));
const out = path.join(SCRATCH, 'Spel2.patched.exe');

async function fileReader(p) {
  const fh = await fsp.open(p, 'r');
  const size = (await fh.stat()).size;
  const read = async (off, len) => {
    const b = Buffer.alloc(len);
    const { bytesRead } = await fh.read(b, 0, len, off);
    return new Uint8Array(b.buffer, b.byteOffset, bytesRead);
  };
  return { read, size, close: () => fh.close() };
}
const brief = (r) => ({ state: r.state, via: r.via, sites: r.sites.map((s) => `${s.offset.toString(16)}:${s.from.toString(16)}->${s.to.toString(16)}`) });
const check = (r, state, via) => {
  assert.equal(r.state, state, r.message);
  assert.equal(r.via, via);
  assert.equal(r.sites.length, 2);
  assert.deepEqual(r.sites.map((s) => [s.offset, s.from, s.to]), [[0x22CBCA2B, 0x07, 0x00], [0x22CBCABC, 0xFE, 0x7E]]);
};

try {
  const f = await fileReader(exe);
  let r = await P.analyze(f.read, f.size);
  console.log('fast  :', JSON.stringify(brief(r)));
  check(r, 'stock', 'fast');

  let last = -1;
  const t0 = Date.now();
  r = await P.analyze(f.read, f.size, { skipFast: true, onProgress: (p) => { last = p; } });
  console.log('scan  :', JSON.stringify(brief(r)), `progress=${last} ${((Date.now() - t0) / 1000).toFixed(1)}s`);
  check(r, 'stock', 'scan');
  assert.equal(last, 1);
  await f.close();

  await fsp.copyFile(exe, out);
  const fh = await fsp.open(out, 'r+');
  await fh.write(Buffer.from([0x00]), 0, 1, 0x22CBCA2B);
  await fh.write(Buffer.from([0x7E]), 0, 1, 0x22CBCABC);
  await fh.close();

  const g = await fileReader(out);
  r = await P.analyze(g.read, g.size);
  console.log('patched fast:', JSON.stringify(brief(r)));
  check(r, 'patched', 'fast');
  r = await P.analyze(g.read, g.size, { skipFast: true });
  console.log('patched scan:', JSON.stringify(brief(r)));
  check(r, 'patched', 'scan');
  await g.close();

  let cmpOut;
  try { cmpOut = execFileSync('cmp', ['-l', exe, out], { encoding: 'utf8' }); }
  catch (e) { if (e.status !== 1) throw e; cmpOut = e.stdout; } // cmp exits 1 when files differ
  const diff = cmpOut.trim().split('\n');
  console.log('cmp -l:', diff.join(' | '), '(1-based offset, octal old, octal new)');
  assert.equal(diff.length, 2);
} finally {
  await fsp.rm(out, { force: true });
  console.log('scratch copy deleted');
}

// synthetic tests
const mk = (n, ...parts) => { const b = new Uint8Array(n); b[0] = 0x4D; b[1] = 0x5A; for (const [o, s] of parts) b.set(s, o); return b; };
const rd = (b) => async (o, l) => b.slice(o, o + l);
const [A, B] = P.SITES;
const dup = mk(1 << 20, [0x10000, A.stock], [0x20000, A.stock], [0x30000, B.stock]);
let s = await P.analyze(rd(dup), dup.length);
console.log('synthetic duplicate:', s.state, '-', s.message);
assert.equal(s.state, 'error');
const ok = mk(1 << 20, [0x10000, A.stock], [0x30000, B.stock]);
s = await P.analyze(rd(ok), ok.length);
assert.equal(s.state, 'stock'); assert.equal(s.via, 'scan');
const mixed = mk(1 << 20, [0x10000, A.patched], [0x30000, B.stock]);
s = await P.analyze(rd(mixed), mixed.length);
console.log('synthetic mixed:', s.state); assert.equal(s.state, 'error');
const none = mk(1 << 20);
assert.equal((await P.analyze(rd(none), none.length)).state, 'error');
const nomz = mk(1 << 20, [0x10000, A.stock], [0x30000, B.stock]); nomz[0] = 0;
s = await P.analyze(rd(nomz), nomz.length);
console.log('synthetic no MZ:', s.state, '-', s.message); assert.equal(s.state, 'error');
// signature straddling a chunk boundary must be found (and not double counted)
const edge = mk(P.CHUNK * 2 + 100, [P.CHUNK - 5, A.stock], [P.CHUNK * 2 - 3, B.stock]);
s = await P.analyze(rd(edge), edge.length);
assert.equal(s.state, 'stock'); assert.equal(s.sites[0].offset, P.CHUNK - 5 + 7);
console.log('synthetic chunk-boundary: stock OK');
console.log('ALL TESTS PASSED');

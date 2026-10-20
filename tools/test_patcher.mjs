// node tools/test_patcher.mjs [path/to/Spel2.exe]
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';
import assert from 'node:assert/strict';
import os from 'node:os';
import crypto from 'node:crypto';

const here = path.dirname(fileURLToPath(import.meta.url));
const html = fs.readFileSync(path.join(here, '..', 'index.html'), 'utf8');
const m = html.match(/<script id="core">([\s\S]*?)<\/script>/);
assert(m, 'core script not found');
const P = new Function(m[1] + '\n;return Spel2Patch;')();
const pm = html.match(/<script id="payload">([\s\S]*?)<\/script>/);
assert(pm, 'payload script not found');
const { MOD_DLL_SHA256, MOD_DLL_B64 } = new Function(pm[1] + '\n;return { MOD_DLL_SHA256, MOD_DLL_B64 };')();
const MOD = new Uint8Array(Buffer.from(MOD_DLL_B64, 'base64'));
assert.equal(crypto.createHash('sha256').update(MOD).digest('hex'), MOD_DLL_SHA256, 'payload sha256 mismatch');
const builtDll = path.join(here, '..', 'discord_game_sdk.dll');
if (fs.existsSync(builtDll)) {
  assert.equal(crypto.createHash('sha256').update(fs.readFileSync(builtDll)).digest('hex'), MOD_DLL_SHA256,
    'index.html embeds a different dll than discord_game_sdk.dll: run node tools/embed_dll.mjs');
}

const exe = process.argv[2] || path.join(here, '..', 'Spel2.exe');
const SCRATCH = fs.mkdtempSync(path.join(os.tmpdir(), 'spel2-patch-'));
const out = path.join(SCRATCH, 'Spel2.patched.exe');
const full = path.join(SCRATCH, 'Spel2.43.exe');

// write assemble() output, with slices as lazy ranges of the source file
async function writeParts(src, dst, parts) {
  const i = await fsp.open(src, 'r'), o = await fsp.open(dst, 'w');
  const buf = Buffer.alloc(16 << 20);
  for (const p of parts) {
    if (p instanceof Uint8Array) { await o.write(p); continue; }
    for (let pos = p.a; pos < p.b;) {
      const { bytesRead } = await i.read(buf, 0, Math.min(buf.length, p.b - pos), pos);
      await o.write(buf, 0, bytesRead); pos += bytesRead;
    }
  }
  await i.close(); await o.close();
}
const objdump = (...a) => execFileSync('x86_64-w64-mingw32-objdump', a, { encoding: 'utf8', maxBuffer: 64 << 20 });

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

  // full patch: heap fix + embedded mod, from stock and from heap-only
  const f2 = await fileReader(exe);
  const pl = await P.plan(f2.read, f2.size, MOD);
  await f2.close();
  assert.equal(pl.state, 'ready', pl.message); assert.equal(pl.heap, 'stock'); assert.equal(pl.edits.length, 3);
  await writeParts(exe, full, P.assemble((a, b) => ({ a, b }), f2.size, pl));
  const h = objdump('-h', full), pe = objdump('-p', full);
  const sec = h.match(/\.m43\s+([0-9a-f]+)\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)/);
  assert(sec, '.m43 section missing'); assert.equal(parseInt(sec[2], 16), 0x140000000 + pl.va);
  assert.equal(parseInt(pe.match(/AddressOfEntryPoint\s+([0-9a-f]+)/)[1], 16), pl.stubRva);
  assert.equal(parseInt(sec[3], 16), 0x22E3BC00);
  const base = 0x140000000, at = (r) => '0x' + (base + r).toString(16);
  const dis = objdump('-d', '--no-show-raw-insn', '--start-address=' + at(pl.stubRva), '--stop-address=' + at(pl.stubRva + 34), full);
  const tgt = [...dis.matchAll(/# (0x[0-9a-f]+)|(?:call|jmp)\s+(0x[0-9a-f]+)/g)].map((x) => parseInt(x[1] || x[2], 16) - base);
  const ent = P.peInfo(MOD); const mapped = [];
  console.log('stub  :', dis.split('\n').filter((l) => /^\s+[0-9a-f]+:/.test(l)).map((l) => l.split('\t').pop().trim()).join(' ; '));
  assert.deepEqual(tgt.slice(0, 2), [0, pl.va]);
  assert.equal(tgt[3], 0x22D08784);
  let full2 = await fileReader(full);
  const again = await P.plan(full2.read, full2.size, MOD);
  console.log('re-patch:', again.state, '-', again.message); assert.equal(again.state, 'done');
  await full2.close();
  assert.equal((await fsp.stat(full)).size, 0x22E3BC00 + pl.append.length);
  let cmp2;
  try { cmp2 = execFileSync('cmp', ['-l', exe, full], { encoding: 'utf8', maxBuffer: 64 << 20 }); }
  catch (e) { if (e.status !== 1) throw e; cmp2 = e.stdout; }
  const changed = cmp2.trim().split('\n').map((l) => parseInt(l, 10) - 1);
  assert(changed.every((o) => o < 0x400 || o === 0x22CBCA2B || o === 0x22CBCABC), 'bytes changed outside header + heap sites');
  console.log('full  : ok, +' + pl.append.length + ' bytes, ' + changed.length + ' original bytes changed (header + 2 heap)');

  const g2 = await fileReader(out);
  const ph = await P.plan(g2.read, g2.size, MOD);
  await g2.close();
  console.log('heap-only -> full:', ph.state, ph.heap, ph.edits.length, 'edit'); assert.equal(ph.state, 'ready'); assert.equal(ph.heap, 'patched'); assert.equal(ph.edits.length, 1);
} finally {
  await fsp.rm(out, { force: true });
  await fsp.rm(full, { force: true });
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
// embed refusals: a non-PE file with valid heap sites, and the real header on a truncated file
s = await P.plan(rd(ok), ok.length, MOD);
console.log('synthetic not PE:', s.state, '-', s.message); assert.equal(s.state, 'error');
const realHead = new Uint8Array(0x1000);
{ const fh = await fsp.open(exe, 'r'); await fh.read(realHead, 0, 0x1000, 0); await fh.close(); }
const trunc = mk(1 << 20, [0, realHead], [0x10000, A.stock], [0x30000, B.stock]);
s = await P.plan(rd(trunc), trunc.length, MOD);
console.log('synthetic truncated:', s.state, '-', s.message); assert.equal(s.state, 'error'); assert.match(s.message, /truncated/);
console.log('ALL TESTS PASSED');

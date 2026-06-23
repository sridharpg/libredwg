// Scale/perf test for the O(n²)->linear fix: build N booths (PLINE+HATCH+MTEXT)
// and report build_dwg wall time. Pass N as arg (default 2000).
//   node wasm/tests/test-scale.mjs 6000
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const __d = path.dirname(fileURLToPath(import.meta.url));
const DIST = path.join(__d, '..', 'dist');
const { default: createLibreDWG } = await import(path.join(DIST, 'libredwg.mjs'));

const N = Number(process.argv[2] || 2000);
const errs = [];
const M = await createLibreDWG({
  print() {}, printErr(m) { if (errs.length < 40) errs.push(String(m)); },
  locateFile(p) { return p.endsWith('.wasm') ? path.join(DIST, 'libredwg.wasm') : path.join(DIST, p); }
});

// Build a description: N "booths" each = 1 closed PLINE + 1 HATCH (solid fill) + 1 MTEXT label.
const lines = ['DWGADD1', 'VER r2000', 'LAYERS 3', '7 1:0', '3 12:booth-shapes', '5 12:booth-labels'];
const ents = [];
for (let i = 0; i < N; i++) {
  const x = (i % 60) * 30, y = Math.floor(i / 60) * 25;
  const pts = [[x,y],[x+20,y],[x+20,y+10],[x,y+10]];
  const hv = pts.map(p => `${p[0]} ${p[1]}`).join(' ');     // x y pairs (HATCH)
  const pv = pts.map(p => `${p[0]} ${p[1]} 0`).join(' ');   // x y bulge triples (PLINE)
  ents.push(`HATCH 1 3 1 4 ${hv}`);           // solid hatch: nloops=1, npts=4
  ents.push(`PLINE 1 256 1 4 ${pv}`);         // closed outline, per-vertex bulge=0
  const label = `B${i}`;
  ents.push(`MTEXT 2 256 ${x+2} ${y+8} 2 16 ${(i%4)*15} 1 ${label.length}:${label}`);  // att=1 (top-left)
}
lines.push(`ENTS ${ents.length}`); for (const e of ents) lines.push(e); lines.push("END");
const desc = lines.join('\n');
M.FS.writeFile('/in.desc', desc);
console.log(`desc: ${desc.length} bytes, ${ents.length} entities (${N} booths)`);

const t0 = Date.now();
const rc = M.ccall('build_dwg', 'number', ['string', 'string', 'string'], ['/in.desc', '/out.dwg', 'r2000']);
const ms = Date.now() - t0;
console.log(`build_dwg rc=${rc} (0x${(rc>>>0).toString(16)}) in ${ms}ms`);
if (rc !== 0) { console.log(errs.join('\n')); process.exit(1); }
const dwg = M.FS.readFile('/out.dwg');
console.log(`out.dwg = ${dwg.length} bytes, magic="${Buffer.from(dwg.slice(0,6)).toString('latin1')}"`);

// verify via round-trip
errs.length = 0;
const rc2 = M.ccall('libredwg_dwg2dxf', 'number', ['string','string','string','number','number'], ['/out.dwg','/rt.dxf','',0,0]);
const dxf = M.FS.readFile('/rt.dxf', { encoding: 'utf8' });
const count = t => (dxf.match(new RegExp(`\\n${t}\\r?\\n`, 'g')) || []).length;
console.log(`dwg2dxf rc=${rc2}: HATCH=${count('HATCH')} LWPOLYLINE=${count('LWPOLYLINE')} MTEXT=${count('MTEXT')}`);
if (errs.length) console.log('notes:', errs.slice(0,5).join(' | '));

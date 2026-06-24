// XCLIP/SPATIAL_FILTER smoke test for build_dwg's new BLOCKS + XINSERT grammar.
//   node wasm/tests/test-xclip.mjs
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const __d = path.dirname(fileURLToPath(import.meta.url));
const DIST = path.join(__d, '..', 'dist');
const { default: createLibreDWG } = await import(path.join(DIST, 'libredwg.mjs'));

const errs = [];
const M = await createLibreDWG({
  print() {}, printErr(m) { if (errs.length < 80) errs.push(String(m)); },
  locateFile(p) { return p.endsWith('.wasm') ? path.join(DIST, 'libredwg.wasm') : path.join(DIST, p); }
});

// 2 layers: "0" (idx 0) and "drawings" (idx 1).
// One FLOOR_CLIP_DRAWINGS block holding 2 entities on layer 1.
// One top-level XINSERT of that block carrying a 4-pt clip polygon, plus a
// top-level booth LINE on layer 0 (outside the clip wrapper).
const L = s => `${Buffer.byteLength(s, 'utf8')}:${s}`; // length-prefixed string
const BLOCK = 'FLOOR_CLIP_DRAWINGS';
const clip = [[0, 0], [200, 0], [200, 150], [0, 150]];
const desc = [
  'DWGADD1',
  'VER r2004',
  'LAYERS 2',
  `7 ${L('0')}`,
  `3 ${L('drawings')}`,
  'BLOCKS 1',
  `${L(BLOCK)} 2`,
  'LINE 1 256 10 10 180 10',
  'PLINE 1 256 1 4 10 10 0 180 10 0 180 130 0 10 130 0',
  'ENTS 2',
  `XINSERT 1 256 ${L(BLOCK)} 0 0 1 1 0 ${clip.length} ${clip.map(p => `${p[0]} ${p[1]}`).join(' ')}`,
  'LINE 0 256 5 5 50 50',
  'END'
].join('\n');

M.FS.writeFile('/in.desc', desc);
const rc = M.ccall('build_dwg', 'number', ['string', 'string', 'string'], ['/in.desc', '/out.dwg', 'r2004']);
console.log(`build_dwg rc=${rc} (0x${(rc >>> 0).toString(16)})`);
if (rc !== 0) { console.log('ERRS:\n' + errs.join('\n')); process.exit(1); }

const dwg = M.FS.readFile('/out.dwg');
console.log(`out.dwg = ${dwg.length} bytes, magic="${Buffer.from(dwg.slice(0, 6)).toString('latin1')}"`);

// dwg -> dxf round-trip
errs.length = 0;
const rc2 = M.ccall('libredwg_dwg2dxf', 'number', ['string', 'string', 'string', 'number', 'number'], ['/out.dwg', '/rt.dxf', '', 0, 0]);
console.log(`dwg2dxf rc=${rc2} (0x${(rc2 >>> 0).toString(16)})`);
const dxf = M.FS.readFile('/rt.dxf', { encoding: 'utf8' });

const lines = dxf.split(/\r?\n/);
const countMarker = t => lines.filter((l, i) => l.trim() === '0' && lines[i + 1]?.trim() === t).length;
console.log('entities:',
  ['INSERT', 'LINE', 'LWPOLYLINE', 'SPATIAL_FILTER'].map(t => `${t}=${countMarker(t)}`).join(' '));

// The chain is only an ACTIVE XCLIP if the INSERT carries its extension
// dictionary (102 {ACAD_XDICTIONARY ... 360 <handle>). At R2004+ the encoder
// drops that handle unless the entity's is_xdic_missing flag is cleared, so a
// SPATIAL_FILTER object can exist while AutoCAD shows no clip. Assert linkage.
const xdictLinks = (dxf.match(/ACAD_XDICTIONARY/g) || []).length;
console.log('INSERT->xdict links (ACAD_XDICTIONARY):', xdictLinks);

// extract SPATIAL_FILTER clip points (code 70 count, then 10/20 pairs)
let inSF = false, pend = null, pts = [], n70 = null;
for (let i = 0; i < lines.length - 1; i += 2) {
  const code = lines[i].trim(), val = lines[i + 1]?.trim();
  if (code === '0' && val === 'SPATIAL_FILTER') { inSF = true; continue; }
  if (code === '0' && inSF) { break; }
  if (!inSF) { continue; }
  if (code === '70') { n70 = parseInt(val, 10); }
  if (code === '10') { pend = parseFloat(val); }
  if (code === '20' && pend !== null) { pts.push([pend, parseFloat(val)]); pend = null; }
}
console.log('SPATIAL_FILTER num_clip_verts(70)=', n70, 'points=', JSON.stringify(pts));

const ok = countMarker('SPATIAL_FILTER') >= 1 && n70 === clip.length && xdictLinks >= 1 &&
  pts.length === clip.length && pts.every((p, i) => Math.abs(p[0] - clip[i][0]) < 1e-6 && Math.abs(p[1] - clip[i][1]) < 1e-6);
console.log(ok ? 'PASS: spatial filter round-tripped with exact polygon + linked to INSERT' : 'FAIL');
if (errs.length) { console.log('decode notes:\n' + errs.slice(0, 12).join('\n')); }
process.exit(ok ? 0 : 1);

// Basic build_dwg smoke test: exports present, build several entity types, and
// confirm they survive a dwg -> dxf round-trip.
//   node wasm/tests/test-build.mjs
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const __d = path.dirname(fileURLToPath(import.meta.url));
const DIST = path.join(__d, '..', 'dist');
const { default: createLibreDWG } = await import(path.join(DIST, 'libredwg.mjs'));

const errs = [];
const M = await createLibreDWG({
  print() {}, printErr(m) { if (errs.length < 60) errs.push(String(m)); },
  locateFile(p) { return p.endsWith('.wasm') ? path.join(DIST, 'libredwg.wasm') : path.join(DIST, p); }
});
console.log('exports present:',
  ['_build_dwg', '_libredwg_dwg2dxf', '_libredwg_dxf2dwg', '_libredwg_version']
    .map(f => `${f}=${typeof M[f]}`).join(' '));

// --- description: 2 layers, several entity types ---
const desc = [
  'DWGADD1',
  'VER r2000',
  'LAYERS 2',
  '7 1:0',
  '3 6:booths',
  'ENTS 6',
  'LINE 1 256 0 0 100 0',
  'PLINE 1 256 1 4 0 0 0 50 0 0 50 50 0 0 50 0',
  'CIRCLE 0 1 25 25 10',
  'ARC 0 2 60 60 15 0 90',
  'TEXT 1 256 10 10 5 0 0 0 5:Hello',
  'MTEXT 1 256 20 20 5 40 30 5 6:Hall A',
  'END'
].join('\n');

M.FS.writeFile('/in.desc', desc);
const t0 = Date.now();
const rc = M.ccall('build_dwg', 'number', ['string', 'string', 'string'], ['/in.desc', '/out.dwg', 'r2000']);
console.log(`build_dwg rc=${rc} (0x${(rc>>>0).toString(16)}) in ${Date.now()-t0}ms`);
if (rc !== 0) { console.log('ERRS:\n' + errs.join('\n')); process.exit(1); }

const dwg = M.FS.readFile('/out.dwg');
const magic = Buffer.from(dwg.slice(0, 6)).toString('latin1');
console.log(`out.dwg = ${dwg.length} bytes, magic="${magic}"`);

// round-trip: dwg -> dxf, confirm entities survived
errs.length = 0;
const rc2 = M.ccall('libredwg_dwg2dxf', 'number', ['string','string','string','number','number'], ['/out.dwg','/rt.dxf','',0,0]);
console.log(`dwg2dxf rc=${rc2} (0x${(rc2>>>0).toString(16)})`);
const dxf = M.FS.readFile('/rt.dxf', { encoding: 'utf8' });
const count = t => (dxf.match(new RegExp(`\\n${t}\\r?\\n`, 'g')) || []).length;
console.log('round-trip DXF entities:',
  ['LINE','LWPOLYLINE','CIRCLE','ARC','TEXT','MTEXT','HATCH'].map(t => `${t}=${count(t)}`).join(' '));
console.log(errs.length ? 'decode notes:\n' + errs.slice(0,8).join('\n') : 'no decode errors');

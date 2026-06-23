// DWG->DXF import parity: decode the same .dwg with a REFERENCE module and the
// freshly-built one, and diff entity counts + output length. Use before deploying
// a rebuilt module to confirm the import path is byte-stable.
//   node wasm/tests/test-import-parity.mjs path/to/file.dwg
//   REF_WASM=/path/to/other/wasm node wasm/tests/test-import-parity.mjs file.dwg
//
// REF_WASM defaults to the floor-planner's currently-shipped module.
import path from 'node:path';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';
const __d = path.dirname(fileURLToPath(import.meta.url));
const DIST = path.join(__d, '..', 'dist');
const REF = process.env.REF_WASM
  || '/Users/sridhar-2346/Development/Official/floor-planner/webapps/editor/public/wasm';

async function load(wasmDir) {
  const { default: create } = await import(path.join(wasmDir, 'libredwg.mjs'));
  return create({ print(){}, printErr(){}, locateFile(p){ return p.endsWith('.wasm') ? path.join(wasmDir,'libredwg.wasm') : path.join(wasmDir, p); } });
}
const OLD = await load(REF);
const NEW = await load(DIST);

const dwgFile = process.argv[2];
if (!dwgFile) { console.error('usage: node wasm/tests/test-import-parity.mjs file.dwg'); process.exit(2); }
const bytes = fs.readFileSync(dwgFile);
function dwg2dxf(M, tag) {
  M.FS.writeFile('/in.dwg', new Uint8Array(bytes));
  const rc = M.ccall('libredwg_dwg2dxf','number',['string','string','string','number','number'],['/in.dwg', `/${tag}.dxf`,'',0,0]);
  return { rc, dxf: rc===0 ? M.FS.readFile(`/${tag}.dxf`,{encoding:'utf8'}) : '' };
}
const o = dwg2dxf(OLD,'o'), n = dwg2dxf(NEW,'n');
console.log(`file=${path.basename(dwgFile)}  ref=${REF}`);
console.log(`REF rc=${o.rc} dxf=${o.dxf.length}B  NEW rc=${n.rc} dxf=${n.dxf.length}B`);
const ce = (s,t)=> (s.match(new RegExp(`\\n${t}\\r?\\n`,'g'))||[]).length;
for (const t of ['LINE','LWPOLYLINE','CIRCLE','ARC','TEXT','MTEXT','INSERT','HATCH','SPLINE']) {
  const a=ce(o.dxf,t), b=ce(n.dxf,t);
  if (a||b) console.log(`  ${t}: ref=${a} new=${b} ${a===b?'OK':'*** DIFF ***'}`);
}
console.log(o.dxf.length === n.dxf.length ? 'BYTE-IDENTICAL length' : `length delta=${n.dxf.length-o.dxf.length}`);

// True-color round-trip oracle: layer 420 (BYLAYER inherit), entity 420, and
// MTEXT attachment(71)+position(10) survive build_dwg -> dwg2dxf.
//   node wasm/tests/verify-color.mjs
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const __d = path.dirname(fileURLToPath(import.meta.url));
const DIST = path.join(__d, '..', 'dist');
const { default: createLibreDWG } = await import(path.join(DIST, 'libredwg.mjs'));
const M = await createLibreDWG({ print(){}, printErr(){}, locateFile(p){ return p.endsWith('.wasm') ? path.join(DIST,'libredwg.wasm') : path.join(DIST,p);} });

// Layer "booths" carries true-color red (BYLAYER line must inherit it);
// second line carries explicit true-color green; MTEXT uses attachment 5 (middle-center).
const desc = ['DWGADD1','VER r2004','LAYERS 2','7 1:0','#FF0000 6:booths','ENTS 3',
  'LINE 1 256 0 0 100 0','LINE 1 #00FF00 0 0 50 50','MTEXT 1 256 20 20 5 40 0 5 6:Hall A','END'].join('\n');
M.FS.writeFile('/in.desc', desc);
const rc = M.ccall('build_dwg','number',['string','string','string'],['/in.desc','/out.dwg','r2004']);
const rc2 = M.ccall('libredwg_dwg2dxf','number',['string','string','string','number','number'],['/out.dwg','/rt.dxf','',0,0]);
if (rc || rc2) { console.log(`FAIL build/roundtrip rc=${rc} rc2=${rc2}`); process.exit(1); }

const L = M.FS.readFile('/rt.dxf',{encoding:'utf8'}).split(/\r?\n/);
const recs=[]; let cur=null;
for(let i=0;i+1<L.length;i+=2){ const c=L[i].trim(),v=L[i+1]; if(c==='0'){if(cur)recs.push(cur);cur={t:v.trim(),g:{}};}else if(cur){(cur.g[c]=cur.g[c]||[]).push(v.trim());}}
if(cur)recs.push(cur);
const rgb = v => v==null ? null : ((Number(v)>>>0) & 0xffffff);

const booths = recs.find(r=>r.t==='LAYER' && (r.g['2']||[])[0]==='booths');
const greenLine = recs.filter(r=>r.t==='LINE').map(r=>rgb((r.g['420']||[])[0])).find(x=>x===0x00FF00);
const mtext = recs.find(r=>r.t==='MTEXT');

const layerOk = booths && rgb((booths.g['420']||[])[0]) === 0xFF0000;
const entOk = greenLine === 0x00FF00;
const mAttach = mtext && Number((mtext.g['71']||[])[0]) === 5;
// MTEXT anchors on its insertion point (DXF 10) — must round-trip to (20,20),
// unlike justified TEXT whose alignment point was getting zeroed.
const mx = mtext ? Number((mtext.g['10']||[])[0]) : NaN;
const my = mtext ? Number((mtext.g['20']||[])[0]) : NaN;
const mPos = mx === 20 && my === 20;
const mtextOk = mAttach && mPos;
console.log(`layer "booths" 420 -> ${booths ? '0x'+(rgb((booths.g['420']||[])[0])||0).toString(16) : 'none'}  ${layerOk?'PASS':'FAIL'}`);
console.log(`entity green 420  -> ${greenLine!=null?'0x'+greenLine.toString(16):'none'}  ${entOk?'PASS':'FAIL'}`);
console.log(`MTEXT attach+pos  -> 71=${mtext ? (mtext.g['71']||['none'])[0] : 'no mtext'} 10=(${mx},${my})  ${mtextOk?'PASS':'FAIL'}`);
process.exit(layerOk && entOk && mtextOk ? 0 : 1);

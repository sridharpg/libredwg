/* Post-build check for the EXTENTS feature of the build_dwg superset.
 *
 * Run AFTER ./wasm/build.sh:
 *   node wasm/tests/verify-extents.mjs
 *
 * Builds a DWG carrying an EXTENTS record (the plan bounding box the
 * dwg-export-writer emits between LAYERS and ENTS), round-trips it back to DXF
 * with libredwg_dwg2dxf, and asserts the drawing extents ($EXTMIN/$EXTMAX) and
 * the active viewport view ($VIEWCTR/$VIEWSIZE) frame the bbox — i.e. a CAD
 * viewer will zoom-to-fit the plan instead of opening it tiny/off-screen.
 *
 * It also verifies the no-EXTENTS path still builds (backward compatibility).
 */
import { fileURLToPath, pathToFileURL } from 'node:url';
import { dirname, resolve } from 'node:path';
import assert from 'node:assert/strict';

const here = dirname(fileURLToPath(import.meta.url));
const wasmDir = resolve(here, '..', 'dist');
const moduleUrl = pathToFileURL(resolve(wasmDir, 'libredwg.mjs')).href;

const createLibreDWG = (await import(moduleUrl)).default;
const Module = await createLibreDWG({
	print: () => {},
	printErr: (m) => console.error('[libredwg]', m),
	locateFile: (p) => (String(p).endsWith('.wasm') ? resolve(wasmDir, 'libredwg.wasm') : resolve(wasmDir, p))
});

// Bounding box: x in [10,110], y in [20,70]  =>  w=100, h=50.
const minX = 10, minY = 20, maxX = 110, maxY = 70;
const cx = (minX + maxX) / 2; // 60
const cy = (minY + maxY) / 2; // 45
const view = Math.max(maxX - minX, maxY - minY) * 1.1; // max(100,50)*1.1 = 110

const desc = [
	'DWGADD1',
	'VER r2004',
	'LAYERS 1',
	'7 1:0',
	`EXTENTS ${minX} ${minY} ${maxX} ${maxY}`,
	'ENTS 1',
	`LINE 0 7 ${minX} ${minY} ${maxX} ${maxY}`,
	'END'
].join('\n');

Module.FS.writeFile('/ext.txt', desc);
let err = Module.ccall('build_dwg', 'number', ['string', 'string', 'string'], ['/ext.txt', '/ext.dwg', 'r2004']);
assert.equal(err, 0, `build_dwg (with EXTENTS) failed: 0x${(err >>> 0).toString(16)}`);

err = Module.ccall('libredwg_dwg2dxf', 'number', ['string', 'string', 'string', 'number', 'number'], ['/ext.dwg', '/ext.dxf', '', 0, 0]);
assert.equal(err, 0, `dwg2dxf failed: 0x${(err >>> 0).toString(16)}`);

const dxf = Module.FS.readFile('/ext.dxf', { encoding: 'utf8' });

// Pull a HEADER variable: $NAME is followed by group-code/value pairs.
function headerVar(name, codes) {
	const re = new RegExp(`\\$${name}\\b`);
	const m = re.exec(dxf);
	assert.ok(m, `header should contain $${name}`);
	const tail = dxf.slice(m.index);
	const out = {};
	for (const code of codes) {
		const cre = new RegExp(`\\n\\s*${code}\\s*\\n\\s*(-?[0-9.eE+]+)`);
		const cm = cre.exec(tail);
		assert.ok(cm, `$${name} should carry group ${code}`);
		out[code] = parseFloat(cm[1]);
	}
	return out;
}

const near = (a, b, eps = 1e-6) => Math.abs(a - b) <= eps * Math.max(1, Math.abs(b));

const extmin = headerVar('EXTMIN', [10, 20]);
const extmax = headerVar('EXTMAX', [10, 20]);
assert.ok(near(extmin[10], minX) && near(extmin[20], minY), `EXTMIN ${JSON.stringify(extmin)} != (${minX},${minY})`);
assert.ok(near(extmax[10], maxX) && near(extmax[20], maxY), `EXTMAX ${JSON.stringify(extmax)} != (${maxX},${maxY})`);

// The active-view source CAD readers restore is the *Active VPORT table record
// (libredwg's R2004 DXF writer omits the $VIEWCTR/$VIEWSIZE header vars). Group
// 12/22 = view center, group 40 = view height, group 41 = aspect ratio.
function vportField(code) {
	const start = dxf.indexOf('*Active');
	assert.ok(start >= 0, 'DXF should contain the *Active VPORT record');
	const rec = dxf.slice(start, start + 800);
	const cm = new RegExp(`\\n\\s*${code}\\s*\\n\\s*(-?[0-9.eE+]+)`).exec(rec);
	assert.ok(cm, `*Active VPORT should carry group ${code}`);
	return parseFloat(cm[1]);
}

const vctrX = vportField(12), vctrY = vportField(22), vheight = vportField(40);
assert.ok(near(vctrX, cx) && near(vctrY, cy), `VPORT view center (${vctrX},${vctrY}) != (${cx},${cy})`);
assert.ok(near(vheight, view), `VPORT view height ${vheight} != ${view}`);

console.log(`verify-extents OK: EXTMIN=(${extmin[10]},${extmin[20]}) EXTMAX=(${extmax[10]},${extmax[20]}) VPORT center=(${vctrX},${vctrY}) height=${vheight}`);

// Backward compatibility: a description WITHOUT EXTENTS must still build.
const noExt = ['DWGADD1', 'VER r2004', 'LAYERS 1', '7 1:0', 'ENTS 1', 'LINE 0 7 0 0 5 5', 'END'].join('\n');
Module.FS.writeFile('/noext.txt', noExt);
err = Module.ccall('build_dwg', 'number', ['string', 'string', 'string'], ['/noext.txt', '/noext.dwg', 'r2004']);
assert.equal(err, 0, `build_dwg (no EXTENTS) failed: 0x${(err >>> 0).toString(16)}`);
console.log('verify-extents OK: no-EXTENTS path still builds (backward compatible).');

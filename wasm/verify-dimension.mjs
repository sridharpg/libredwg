/* Post-build smoke test for the build_dwg superset, focused on DIMENSION support.
 *
 * Run AFTER ./wasm/build.sh:
 *   node wasm/verify-dimension.mjs
 * (any Node >= 18; needs ESM + top-level await). Loads the freshly built module
 * from wasm/dist/.
 *
 * It builds a DWG containing one aligned dimension via build_dwg, round-trips it
 * back to DXF with libredwg_dwg2dxf, and asserts a DIMENSION entity with the
 * override label survives — the same oracle used for the color/hatch fixes.
 */
import { fileURLToPath, pathToFileURL } from 'node:url';
import { dirname, resolve } from 'node:path';
import assert from 'node:assert/strict';

const here = dirname(fileURLToPath(import.meta.url));
const wasmDir = resolve(here, 'dist');
const moduleUrl = pathToFileURL(resolve(wasmDir, 'libredwg.mjs')).href;

const createLibreDWG = (await import(moduleUrl)).default;
const Module = await createLibreDWG({
	print: () => {},
	printErr: (m) => console.error('[libredwg]', m),
	locateFile: (p) => (String(p).endsWith('.wasm') ? resolve(wasmDir, 'libredwg.wasm') : resolve(wasmDir, p))
});

// DWGADD1 with a single aligned dimension. Mirrors what dwg-export-writer.js
// emits: DIM <li> <color> x1 y1 x2 y2 tx ty meas attach <text:lenstr>.
const desc = [
	'DWGADD1',
	'VER r2004',
	'LAYERS 1',
	'#00CED1 15:Expo_Dimensions',
	'ENTS 1',
	"DIM 0 #00CED1 1 -2 4 6 2.5 3 8.544 5 5:8'-7\"",
	'END'
].join('\n');

Module.FS.writeFile('/dim.txt', desc);

let err = Module.ccall('build_dwg', 'number', ['string', 'string', 'string'], ['/dim.txt', '/dim.dwg', 'r2004']);
assert.equal(err, 0, `build_dwg failed: 0x${(err >>> 0).toString(16)}`);

err = Module.ccall('libredwg_dwg2dxf', 'number', ['string', 'string', 'string', 'number', 'number'], ['/dim.dwg', '/dim.dxf', '', 0, 0]);
assert.equal(err, 0, `dwg2dxf failed: 0x${(err >>> 0).toString(16)}`);

const dxf = Module.FS.readFile('/dim.dxf', { encoding: 'utf8' });
assert.ok(/(^|\n)\s*DIMENSION\s*(\r?\n)/.test(dxf), 'round-tripped DXF should contain a DIMENSION entity');
assert.ok(/AcDbAlignedDimension/.test(dxf), 'dimension should decode as an aligned dimension (subclass marker)');
assert.ok(dxf.includes("8'-7\""), 'dimension override text should survive the round-trip');
assert.ok(/(^|\n)\s*2\s*\r?\nStandard\s*\r?\n/.test(dxf) || /Standard/.test(dxf), 'a Standard dimstyle should be referenced');

console.log('verify-dimension OK: DIMENSION + aligned subclass + override text round-tripped.');

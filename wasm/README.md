# Floor-planner LibreDWG WASM build

This folder is the **single source of truth** for the WASM module the floor-planner
editor ships at `webapps/editor/public/wasm/libredwg.{mjs,wasm}`. It lives inside a
fork of LibreDWG (this repo) so the C source, the patches, and the build harness
are versioned together. The floor-planner repo carries **no** C/WASM build files —
it only consumes the two built artifacts.

The module is a **superset** of stock LibreDWG: it adds `build_dwg`, a direct
proto→DWG model builder that bypasses LibreDWG's DXF text reader (which is O(n²)
in distinct blocks and rejects some valid DXF, e.g. MTEXT code 50).

## Layout
- `build_dwg.c` — the wrapper compiled into the module (the only `.c` we add).
- `build.sh` — self-locating build → `wasm/dist/{libredwg.mjs,libredwg.wasm}`.
- `verify-dimension.mjs` — post-build smoke test (Node).
- `libredwg-on2-fix.patch` — reference copy of the O(n²) fix (already applied to
  `src/` on this branch; keep it to re-apply after an upstream rebase).
- `embin/` — local emcc wrappers that force python ≥3.10 (git-ignored, per-machine).
- `dist/` — the built artifacts (committed; this is what floor-planner consumes).

## Source patches on this branch
Three tracked edits to upstream `src/` (committed, not just a patch file):
1. **`src/common.h` + `src/dwg.c` — O(n²)→linear** (`libredwg-on2-fix.patch`).
   `PUSH_HV` grew the block entity-handle vector by +1 per push; patched to
   geometric growth. `dwg_add_handleref`/`ordered_ref_add` did an O(n) sorted
   insert per ref; added a `libredwg_skip_ref_dedup` flag (default 0 = stock;
   `build_dwg` sets 1 — duplicate refs are valid for a fresh build). Without this
   `build_dwg` is quadratic and large plans hang/OOM. 150k entities → ~1.5s.
2. **`src/logging.h` — `DISABLE_LOGGING`** (size lever). Compile-time strips all
   trace/info log strings+code (`LOG_ERROR`/`LOG_WARN` kept unless
   `DISABLE_LOGGING_ALL`). `build.sh` defines it; no change to conversion output.

## Build
```sh
./wasm/build.sh            # -> wasm/dist/{libredwg.mjs,libredwg.wasm}
STRIP_STRINGS=1 ./wasm/build.sh   # also drop error/warn strings (smaller, no diagnostics)
```
`build.sh` self-locates (no per-machine path config — this replaced the old
`.env` + `configure-em.sh` + `link-em.sh` split). It links at `-Oz` +
`MinSizeRel` + `-DDISABLE_LOGGING`. Exports: `build_dwg`, `libredwg_dwg2dxf`,
`libredwg_dxf2dwg`, `libredwg_version` (+ `malloc`/`free`, FS/ccall runtime).

### Deploy to floor-planner
```sh
cp wasm/dist/libredwg.{mjs,wasm} <floor-planner>/webapps/editor/public/wasm/
```
Then verify the export in the editor before committing the new binary (a grammar
change in `build_dwg.c` requires a rebuild + redeploy or `build_dwg` returns
`0x14` on the new record).

## Toolchain
- Emscripten (`emcc`), CMake, `make`, perl (LibreDWG dynapi codegen).
- Homebrew's `emcc` wrapper runs under Xcode's python 3.9.6 and fails. The
  `embin/` wrappers force python3.12; `build.sh` prepends `embin/` to PATH if
  present. Regenerate after a fresh clone:
  ```sh
  PY=/opt/homebrew/opt/python@3.12/bin/python3.12
  LIBEXEC=$(dirname $(dirname $(readlink -f $(which emcc)))/libexec)/libexec
  mkdir -p wasm/embin
  for t in emcc em++ emar emranlib emconfigure emcmake emmake; do
    printf '#!/bin/bash\nexec %s %s/%s.py "$@"\n' "$PY" "$LIBEXEC" "$t" > wasm/embin/$t; chmod +x wasm/embin/$t
  done
  printf '#!/bin/bash\nexec %s "$@"\n' "$PY" > wasm/embin/python3 && chmod +x wasm/embin/python3
  ```
- Pinned to LibreDWG commit **`7678e833`** (`0.13.4.8252`) — the exact commit the
  original WASM was built from, so the DWG→DXF decoder is byte-identical.

## Output DWG version: R2004 (AC1018)
The export defaults to **R2004** (set in the floor-planner JS; the WASM accepts
any version at runtime, no rebuild). A strict consumer viewer crashed on
libredwg's R2000 (AC1015) output and showed R2013/R2018 as damaged, but opened
R2004 cleanly. R2004 also supports true colors (DXF 420). Note: libredwg's R2004
files carry a ~1.5 MB fixed section overhead.

## Valid-output gotchas handled in `build_dwg.c`
LibreDWG's `dwg_add_*` produces models its own decoder reads fine but strict CAD
readers (AutoCAD) crash on. Fix-ups applied (validate with `ezdxf`
`recover.readfile` — a clean export reports `0 0`):
- **Layer linetype** — `dwg_add_LAYER` leaves DXF 6 unset (dangling ref);
  `build_dwg` links every layer to `CONTINUOUS`.
- **TEXT width factor** — `dwg_add_TEXT` leaves DXF 41 at 0 (AUDIT "width not in
  range"); `build_dwg` sets `width_factor = 1.0`.
- **Duplicate layer "0"** — `dwg_new_Document` already makes layer "0"; `build_dwg`
  reuses it via `dwg_find_tablehandle` instead of adding a duplicate.

## License
LibreDWG is **GPL-3.0+**; shipping this WASM makes the consuming product subject
to the GPL. Obfuscating the binary neither protects IP (the source is public) nor
removes that obligation.

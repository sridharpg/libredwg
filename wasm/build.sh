#!/usr/bin/env bash
# Build the floor-planner LibreDWG WASM module (a superset of stock libredwg:
# adds build_dwg, the direct proto->DWG model builder — see build_dwg.c).
#
# Self-locating: run it from anywhere; it resolves the repo root from its own
# path, so there is no per-machine path to configure (this is why the old
# .env / configure-em.sh / link-em.sh split is gone).
#
# Usage:
#   ./wasm/build.sh          # build -> wasm/dist/{libredwg.mjs,libredwg.wasm}
#   STRIP_STRINGS=1 ./wasm/build.sh   # also drop LOG_ERROR/LOG_WARN strings (smaller, no diagnostics)
#
# Requires emscripten (emcc/emcmake/emmake) + cmake. If wasm/embin/ exists it is
# prepended to PATH — those are the python3.12-forcing wrappers (Homebrew's emcc
# otherwise grabs Xcode's python3.9 and fails). Regenerate per ../README "Toolchain".
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-wasm"
DIST="$ROOT/wasm/dist"

# emcc toolchain: prefer the in-repo python3.12 wrappers when present.
if [ -d "$ROOT/wasm/embin" ]; then
  export PATH="$ROOT/wasm/embin:$PATH"
fi
command -v emcc >/dev/null || { echo "ERROR: emcc not on PATH (need emscripten; see ../README Toolchain)" >&2; exit 1; }

# DISABLE_LOGGING strips all trace/info log strings+code (a large share of the
# binary); LOG_ERROR/LOG_WARN are kept unless STRIP_STRINGS=1.
LOG_FLAGS="-DDISABLE_LOGGING"
if [ "${STRIP_STRINGS:-0}" = "1" ]; then
  LOG_FLAGS="$LOG_FLAGS -DDISABLE_LOGGING_ALL"
fi

mkdir -p "$BUILD" "$DIST"
cd "$BUILD"

# Static lib-only build, size-optimized. WRITE support stays ON: build_dwg uses
# dwg_write_file, and the dxf2dwg fallback needs the DXF reader. JSON is dropped
# (we export no dwg2json). LTO off (matches the known-good behavior baseline).
emcmake cmake "$ROOT" \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DCMAKE_C_FLAGS="-Oz $LOG_FLAGS" \
  -DBUILD_SHARED_LIBS=OFF \
  -DLIBREDWG_LIBONLY=ON \
  -DLIBREDWG_DISABLE_JSON=ON \
  -DENABLE_LTO=OFF \
  -DDISABLE_WERROR=ON \
  -DBUILD_TESTING=OFF

emmake make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" redwg

# Link the build_dwg superset wrapper against the static lib.
emcc "$ROOT/wasm/build_dwg.c" "$BUILD"/libredwg.a \
  -I"$ROOT/include" -I"$ROOT/src" -I"$BUILD/src" \
  -Oz $LOG_FLAGS \
  -o "$DIST/libredwg.mjs" \
  -sMODULARIZE=1 \
  -sEXPORT_ES6=1 \
  -sEXPORT_NAME=createLibreDWG \
  -sENVIRONMENT=web,worker,node \
  -sALLOW_MEMORY_GROWTH=1 \
  -sINITIAL_MEMORY=64MB \
  -sSTACK_SIZE=8MB \
  -sFORCE_FILESYSTEM=1 \
  -sEXPORTED_FUNCTIONS=_build_dwg,_libredwg_dwg2dxf,_libredwg_dxf2dwg,_libredwg_version,_malloc,_free \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,FS,stringToUTF8,UTF8ToString,writeArrayToMemory,lengthBytesUTF8

echo "=== BUILD DONE ==="
ls -lh "$DIST"
printf 'gzipped wasm: '; gzip -9 -c "$DIST/libredwg.wasm" | wc -c

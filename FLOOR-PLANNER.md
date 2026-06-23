# Floor-planner fork of LibreDWG

This is a fork of [LibreDWG](https://github.com/LibreDWG/libredwg) pinned at
commit `7678e833`, carrying the WASM build the floor-planner editor ships.

- **What's added / changed:** see **[`wasm/README.md`](wasm/README.md)**.
- **Build the module:** `./wasm/build.sh` → `wasm/dist/{libredwg.mjs,libredwg.wasm}`.
- **Source patches on this branch (`floor-planner-wasm`):** `src/common.h` +
  `src/dwg.c` (O(n²)→linear bulk build) and `src/logging.h` (`DISABLE_LOGGING`
  size switch). Everything else under `src/` is stock upstream.

The floor-planner repo consumes only the two built artifacts from `wasm/dist/`;
it contains no C/WASM build files. LibreDWG is **GPL-3.0+**.

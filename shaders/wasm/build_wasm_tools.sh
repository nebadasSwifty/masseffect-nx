#!/usr/bin/env bash
# Builds the WebAssembly modules of the browser installer (everything except DXC, see link_dxc_wasm.sh):
#   hlsl.mjs  the container -> HLSL translator (shaders/tools/masseffect_hlsl.cpp + XenosRecomp)
#   pack.mjs  the package + index packer (shaders/tools/me_pack_shaders.cpp + the app's library code)
#   scan.mjs  the Unreal package scanner that finds containers in the disc's compressed packages
#             (shaders/tools/ue3_shader_scan.cpp, with its own LZO1X decoder)
# Run it from a shell where Emscripten is active (em++ on PATH: `brew install emscripten` or emsdk_env.sh).
#
#   XXHASH_DIR  folder with xxhash.h            (default: found by shaders/tools/env.sh)
#   FMT_DIR     folder that contains fmt/       (same)
#   OUT         output folder (default shaders/wasm/out)
#   usage: shaders/wasm/build_wasm_tools.sh [hlsl] [pack] [scan]     (default: all three)
#
# Both the translator and the packer are plain command line programs: the page writes the inputs into
# Emscripten's in-memory file system (FS) and calls callMain([...]) with the same arguments as on the
# command line, then reads the outputs back.
set -eu
. "$(dirname "${BASH_SOURCE[0]}")/../tools/env.sh"
command -v em++ >/dev/null || { echo "em++ not found: activate Emscripten first" >&2; exit 1; }
need_xxhash; need_fmt
OUT=${OUT:-$SHADERS/wasm/out}
APP=$ROOT/app/src/native/masseffect
mkdir -p "$OUT"
# stackSave/stackRestore: Emscripten's callMain puts argv on the stack and never pops it. The page calls main once
# per shader on the same instance, so without a restore around each call (js/workers/handlers.js runMain) the
# 4 MB stack ran out after ~14,500 calls and the next translation died with "memory access out of bounds".
COMMON=(-sEXPORT_ES6=1 -sMODULARIZE=1 -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 -sALLOW_MEMORY_GROWTH=1
        -sSTACK_SIZE=4MB -sEXPORTED_RUNTIME_METHODS=FS,callMain,stackSave,stackRestore -sENVIRONMENT=web,worker,node)
targets=${*:-hlsl pack scan}

for t in $targets; do
  echo "=== $t"
  case $t in
    hlsl)
      # -fexceptions: XenosRecomp rejects some shaders by throwing std::runtime_error, which masseffect_hlsl.cpp
      # catches and reports ("translation rejected: <reason>"). Without it Emscripten cannot catch C++
      # exceptions: the throw escapes callMain as a bare number (the exception's address) and the installer
      # reported "translator crashed (4228016)".
      em++ -std=c++20 -O1 -fexceptions -I"$SHADERS" -I"$XENOS" -I"$FMT_DIR" -I"$XXHASH_DIR" -include "$XENOS/pch_min.h" \
        -Wno-null-arithmetic -fms-extensions -DFMT_HEADER_ONLY -DXXH_INLINE_ALL -DMASSEFFECT_RECOMP \
        "$SHADERS/tools/masseffect_hlsl.cpp" "$XENOS/shader_recompiler.cpp" -o "$OUT/hlsl.mjs" \
        -fexceptions -sEXPORT_NAME=createHlslModule "${COMMON[@]}";;
    pack)
      # The full library (30k shaders, ~900 MB) needs the whole 4 GB address space of wasm32.
      em++ -std=c++20 -O2 -I"$XXHASH_DIR" "$SHADERS/tools/me_pack_shaders.cpp" "$APP/masseffect_shader_library.cpp" \
        -o "$OUT/pack.mjs" -sEXPORT_NAME=createPackModule -sMAXIMUM_MEMORY=4GB "${COMMON[@]}";;
    scan)
      em++ -std=c++20 -O2 "$SHADERS/tools/ue3_shader_scan.cpp" -o "$OUT/scan.mjs" \
        -sEXPORT_NAME=createScanModule "${COMMON[@]}";;
    *) echo "unknown target $t" >&2; exit 1;;
  esac
done
echo "done: $OUT"

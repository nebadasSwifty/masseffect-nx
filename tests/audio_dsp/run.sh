#!/usr/bin/env bash
# Bit-exactness test of the native audio DSP replacements (app/src/native/me_audio_dsp.h) against the recompiled
# originals, on random guest memory (host: macOS arm64 or Linux aarch64, the same ISA and FPCR semantics as the console).
#   tests/audio_dsp/run.sh [iterations] [seed]
# Needs app/generated/default (tools/codegen.sh) and the SDK third-party sources (tools/fetch_thirdparty.py).
# Env: GEN (generated folder), REXSDK_DIR (default <repo>/sdk), OUT (default <repo>/out/audio_dsp), CXX.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SDK=${REXSDK_DIR:-$ROOT/sdk}
GEN=${GEN:-$ROOT/app/generated/default}
OUT=${OUT:-$ROOT/out/audio_dsp}
CXX=${CXX:-clang++}
mkdir -p "$OUT"
python3 "$HERE/extract.py" "$GEN" "$OUT" 82AA53C0 82B4D580
extra=()
[[ "$(uname)" == Darwin ]] && extra=(-Wl,-undefined,dynamic_lookup)
"$CXX" -std=c++23 -O2 -ffp-contract=off -fno-strict-aliasing -Wno-asm-operand-widths \
  -I"$OUT" -I"$SDK/include" -I"$SDK/thirdparty/fmt/include" -I"$SDK/thirdparty/spdlog/include" \
  -I"$SDK/thirdparty/simde" -I"$GEN" \
  "$HERE/test_dsp.cpp" -o "$OUT/test_dsp" ${extra[@]+"${extra[@]}"}
"$OUT/test_dsp" "${1:-20000}" ${2:-}

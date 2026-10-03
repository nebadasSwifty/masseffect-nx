#!/usr/bin/env bash
# Builds the code generator `rexglue` for the machine you are on (macOS or Linux). The Switch build cannot run it:
# tools/codegen.sh needs it on the host to translate default.xex to C++.
#
#   tools/build_host.sh                 fetch the third-party sources if missing, configure, build
#   tools/build_host.sh --configure     only fetch and configure (a quick check of the toolchain)
#
# Result: sdk/out/host/rexglue (a link to the executable the SDK build puts in sdk/out/<platform>/).
# Environment:
#   CC, CXX    compilers (default: clang and clang++; the SDK is written for clang)
#   JOBS       parallel build jobs (default: number of CPUs)
#   BUILD_DIR  CMake build folder (default: sdk/out/build/host)
# Needs: cmake >= 3.25, ninja, clang, python3, git (git only to fetch the third-party sources).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$ROOT/sdk"
BUILD_DIR="${BUILD_DIR:-$SDK/out/build/host}"
CC="${CC:-clang}"
CXX="${CXX:-clang++}"
PYTHON="${PYTHON:-python3}"
if command -v nproc >/dev/null 2>&1; then DEFAULT_JOBS=$(nproc); else DEFAULT_JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 4); fi
JOBS="${JOBS:-$DEFAULT_JOBS}"
CONFIGURE_ONLY=0
[[ "${1:-}" == "--configure" ]] && CONFIGURE_ONLY=1

for tool in cmake ninja "$CC" "$CXX"; do
  command -v "$tool" >/dev/null 2>&1 || { echo "error: '$tool' not found in PATH" >&2; exit 1; }
done

# The repository only keeps the third-party files this port changed; the rest comes from the upstream release.
if [[ ! -f "$SDK/thirdparty/fmt/include/fmt/format.h" ]]; then
  echo "== fetching the SDK third-party sources (tools/fetch_thirdparty.py)"
  "$PYTHON" "$ROOT/tools/fetch_thirdparty.py"
fi

echo "== configure ($BUILD_DIR)"
cmake -S "$SDK" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX"
[[ $CONFIGURE_ONLY -eq 1 ]] && { echo "configured; run tools/build_host.sh to build"; exit 0; }

echo "== build the rexglue target"
cmake --build "$BUILD_DIR" --target rexglue -j "$JOBS"

# The SDK writes executables to sdk/out/<platform>/ (mac-arm64, linux-amd64, ...). Give them a fixed place.
built=""
for candidate in "$SDK"/out/*/rexglue "$SDK"/out/*/Release/rexglue; do
  [[ -x "$candidate" && "$candidate" != "$SDK/out/host/rexglue" ]] && { built="$candidate"; break; }
done
[[ -n "$built" ]] || { echo "error: the build finished but no rexglue executable was found under $SDK/out" >&2; exit 1; }
mkdir -p "$SDK/out/host"
ln -sfn "../${built#"$SDK/out/"}" "$SDK/out/host/rexglue"
echo "ok: $SDK/out/host/rexglue -> $built"

#!/usr/bin/env bash
# Builds the Switch executable (out/nx/masseffect-nx.nro) inside the official devkitpro/devkita64 Docker image
# (devkitA64 GCC + libnx). No local devkitPro install is needed, only Docker.
#
#   tools/build_nro.sh
#
# Prerequisites, in order (see README.md):
#   1. tools/fetch_thirdparty.py      SDK third-party sources (tools/build_host.sh runs it for you)
#   2. tools/codegen.sh               app/generated/ (the game translated to C++; needs your own disc)
#   3. mesa/build_mesa_docker.sh      the NVK Vulkan driver, laid out as <MESA_SDK>/opt/devkitpro/portlibs/switch/lib/libvulkan.a
#
# Environment (all optional):
#   MESA_SDK              folder produced by mesa/build_mesa_docker.sh (default: the first of out/mesa-sdk and
#                         ../mesa-sdk, next to this repository, that exists). The default driver is built at -O2.
#   MESA_OPT              O1 (or 1) = use the -O1 fallback driver instead: the first of out/mesa-sdk-o1 and ../mesa-sdk-o1
#                         (make it with OPT=1 mesa/build_mesa_opt.sh). Ignored when MESA_SDK is set.
#   SDK_DIR               SDK source tree to build against (default sdk/)
#   NRO_OUT               build folder, relative to the repository (default out/nx)
#   JOBS                  parallel compile jobs inside the container (default 6; every job needs about 1-2 GB)
#   UPSCALE_SHADERS       ON (default) = build the cas/fsr output effects (present_effect = "cas" / "fsr" in masseffect.toml).
#                         Default OFF. CMake keeps the value in its cache, so pass it on every build.
#   DEVKITA64_IMAGE       Docker image with devkitA64, cmake, ninja and glslangValidator. By default the script builds
#                         the small image "masseffect-nx-build" (devkitpro/devkita64 plus glslang-tools) once.
#   MASSEFFECT_LTO        ON/OFF   LTO on the game and app code (default OFF: measured no gain, needs ~16 GB of Docker memory)
#   MASSEFFECT_PGO        "", the generate mode or the use mode of app/CMakeLists.txt (profile-guided optimisation)
#   MASSEFFECT_GEN_OPT    extra optimisation flag for the generated game code, e.g. -Os or -O2
#   MASSEFFECT_FAST_MATH  ON/OFF (default ON)
#   MASSEFFECT_INDIRECT_DISPATCH          0/1/2 guest indirect-call dispatch (default 0 legacy; docs/cpu-cost-analysis.md
#                                         "Indirect call dispatch")
#   MASSEFFECT_INDIRECT_DISPATCH_VERIFY   ON/OFF compare it with the legacy dispatch (default OFF)
#
# The SPIR-V initializers of the native renderer's utility shaders (app/src/native/masseffect/shaders/*.comp|vert|frag)
# are generated during the build by glslangValidator inside the container; nothing else is needed on the host.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK_DIR="${SDK_DIR:-$ROOT/sdk}"
NRO_OUT="${NRO_OUT:-out/nx}"
JOBS="${JOBS:-6}"

command -v docker >/dev/null 2>&1 || { echo "error: docker not found in PATH" >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "error: the Docker daemon is not running" >&2; exit 1; }

# ---- inputs ---------------------------------------------------------------------------------------------------------
MESA_DIR_NAME=mesa-sdk
case "${MESA_OPT:-}" in ""|O2|o2|2) ;; O1|o1|1) MESA_DIR_NAME=mesa-sdk-o1 ;;
  *) echo "error: MESA_OPT must be O1 or O2 (got '$MESA_OPT'); set MESA_SDK for other drivers" >&2; exit 1 ;; esac
if [[ -z "${MESA_SDK:-}" ]]; then
  for candidate in "$ROOT/out/$MESA_DIR_NAME" "$ROOT/../$MESA_DIR_NAME"; do
    [[ -f "$candidate/opt/devkitpro/portlibs/switch/lib/libvulkan.a" ]] && { MESA_SDK="$(cd "$candidate" && pwd)"; break; }
  done
fi
MESA_SDK="${MESA_SDK:-}"
[[ -n "$MESA_SDK" && -f "$MESA_SDK/opt/devkitpro/portlibs/switch/lib/libvulkan.a" ]] || {
  echo "error: the NVK driver was not found. Build it with mesa/build_mesa_docker.sh and set MESA_SDK to its output folder" >&2
  echo "       (expected <MESA_SDK>/opt/devkitpro/portlibs/switch/lib/libvulkan.a)" >&2
  exit 1
}
MESA_SDK="$(cd "$MESA_SDK" && pwd)"
# Which driver is linked (SOURCE.txt of mesa/build_mesa_docker.sh and mesa/build_mesa_opt.sh names -Doptimization).
MESA_OPT_LINKED="$(grep -oE -- '-Doptimization=[0-9sg]+' "$MESA_SDK/SOURCE.txt" 2>/dev/null | head -1 || true)"
echo "== Mesa SDK: $MESA_SDK (${MESA_OPT_LINKED:-optimization level not recorded in SOURCE.txt})"
[[ -f "$ROOT/app/generated/rexglue.cmake" ]] || {
  echo "error: app/generated/rexglue.cmake not found. Run tools/codegen.sh first (it needs your game in assets/game_root)" >&2
  exit 1
}
[[ -f "$SDK_DIR/thirdparty/fmt/include/fmt/format.h" ]] || {
  echo "error: $SDK_DIR/thirdparty is incomplete. Run python3 tools/fetch_thirdparty.py" >&2
  exit 1
}

# ---- build image ----------------------------------------------------------------------------------------------------
if [[ -z "${DEVKITA64_IMAGE:-}" ]]; then
  DEVKITA64_IMAGE=masseffect-nx-build
  if ! docker image inspect "$DEVKITA64_IMAGE" >/dev/null 2>&1; then
    echo "== building the Docker image $DEVKITA64_IMAGE (devkitpro/devkita64 + glslang-tools), once"
    docker build -t "$DEVKITA64_IMAGE" - <<'DOCKERFILE'
FROM devkitpro/devkita64:latest
RUN apt-get update && apt-get install -y --no-install-recommends glslang-tools && rm -rf /var/lib/apt/lists/*
DOCKERFILE
  fi
fi

# ---- build ----------------------------------------------------------------------------------------------------------
# The repository (and the Mesa SDK, when it lives elsewhere) is mounted at the same absolute path inside the container,
# so paths in the generated files stay valid.
mounts=(-v "$ROOT:$ROOT")
case "$MESA_SDK/" in "$ROOT"/*) ;; *) mounts+=(-v "$MESA_SDK:$MESA_SDK:ro") ;; esac
# The SDK may be a link (a staged edition tree, tools/edition.sh): mount its real folder at its real path. The SDK build writes
# into <sdk>/out, so a real folder outside the tree is mounted writable (it is shared, edition-independent build output).
SDK_REAL="$(cd "$SDK_DIR" && pwd -P)"
case "$SDK_REAL/" in "$ROOT"/*) ;; *) mounts+=(-v "$SDK_REAL:$SDK_REAL") ;; esac
user=()
[[ "$(uname -s)" == Linux ]] && user=(--user "$(id -u):$(id -g)" -e HOME=/tmp)

echo "== configure and build ($NRO_OUT, -j$JOBS)"
docker run --rm "${user[@]+"${user[@]}"}" "${mounts[@]}" -w "$ROOT" \
  -e JOBS="$JOBS" -e ROOT="$ROOT" -e SDK_DIR="$SDK_DIR" -e MESA_SDK="$MESA_SDK" -e NRO_OUT="$NRO_OUT" \
  -e LTO="${MASSEFFECT_LTO:-OFF}" -e PGO="${MASSEFFECT_PGO:-}" -e GEN_OPT="${MASSEFFECT_GEN_OPT:-}" \
  -e APP_VERSION="${MASSEFFECT_VERSION:-1.0.0}" -e UPSCALE_SHADERS="${UPSCALE_SHADERS:-ON}" \
  -e FAST_MATH="${MASSEFFECT_FAST_MATH:-ON}" \
  -e INDIRECT_DISPATCH="${MASSEFFECT_INDIRECT_DISPATCH:-0}" -e INDIRECT_DISPATCH_VERIFY="${MASSEFFECT_INDIRECT_DISPATCH_VERIFY:-OFF}" \
  "$DEVKITA64_IMAGE" bash -euo pipefail -c '
    # An older SDK can link successfully while silently omitting the project NVK patches.
    # Check the actual archive, rather than trusting its folder name or SOURCE.txt.
    /opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm --defined-only \
      "$MESA_SDK/opt/devkitpro/portlibs/switch/lib/libvulkan.a" > /tmp/masseffect-nvk-symbols.txt
    if ! grep -Eq " [BD] nvk_switch_draw$" /tmp/masseffect-nvk-symbols.txt; then
      echo "error: Mesa SDK lacks the Mass Effect NVK patches. Rebuild with mesa/build_mesa_docker.sh" >&2
      exit 1
    fi
    cmake -S app -B "$NRO_OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/switch/cmake/switch-devkitA64.cmake" \
      -DDEVKITPRO=/opt/devkitpro -DREXSDK_DIR="$SDK_DIR" \
      -DMASSEFFECT_VERSION="$APP_VERSION" -DMASSEFFECT_LTO="$LTO" -DMASSEFFECT_PGO="$PGO" -DMASSEFFECT_GEN_OPT="$GEN_OPT" -DMASSEFFECT_FAST_MATH="$FAST_MATH" \
      -DMASSEFFECT_INDIRECT_DISPATCH="$INDIRECT_DISPATCH" -DMASSEFFECT_INDIRECT_DISPATCH_VERIFY="$INDIRECT_DISPATCH_VERIFY" \
      -DREXGLUE_SWITCH_NVK_SDK="$MESA_SDK/opt/devkitpro/portlibs/switch" \
      -DREXGLUE_UPSCALE_SHADERS="${UPSCALE_SHADERS:-ON}"
    cmake --build "$NRO_OUT" -j "$JOBS"
    # crt0 must be first in .text or the NRO does not boot
    /opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm "$NRO_OUT/masseffect" | grep "^0000000000000000 T _start$" >/dev/null
    echo "OK: _start at 0"
  '

# The CMake target is called masseffect; the app looks for masseffect-nx.nro next to its configuration (SD card:
# switch/masseffect-nx/). Keep the name the app expects, and the unstripped ELF for profile symbolization.
cp -f "$ROOT/$NRO_OUT/masseffect.nro" "$ROOT/$NRO_OUT/masseffect-nx.nro"
ls -la "$ROOT/$NRO_OUT/masseffect-nx.nro"
echo "ELF with symbols (for tools/function_order.py): $ROOT/$NRO_OUT/masseffect"

#!/bin/bash
# Build the NVK Vulkan driver for the Nintendo Switch on macOS (arm64 or x86_64) or Linux, in Docker, and lay it out
# as the SDK folder that tools/build_nro.sh expects:
#
#     <OUT>/opt/devkitpro/portlibs/switch/lib/libvulkan.a
#
# Steps: clone danfromtico/mesa-switch at the pinned commit, apply mesa-switch-masseffect.patch, run the upstream
# build-switch.sh (it builds the devkitpro-mesa-rust image from Docker.rust, then Mesa's host tools, then the Switch
# archives), and merge libnak_rs.a (the Rust runtime of the shader compiler) into libvulkan.a.
#
#   ./build_mesa_docker.sh                       # full build, result in ../../mesa-sdk (next to the repo)
#   OUT=/path/to/mesa-sdk ./build_mesa_docker.sh
#   SRC=/path/to/mesa-switch-src ./build_mesa_docker.sh
#   ./build_mesa_docker.sh --incremental         # after editing SRC: only ninja + merge (needs a previous full build)
#   APPLY_ONLY=1 ./build_mesa_docker.sh          # only clone + apply the patch (no Docker needed)
#
# Needs: git, docker. First full build: about an hour or more and ~15 GB of disk. The Docker.rust image installs
# the latest Rust nightly; our reference build used rust 1.101.0-nightly (LLVM 23.1.1). If a newer nightly breaks the
# NAK build, install that nightly instead (see README.md).
set -euo pipefail

PIN=d4a00ea0ab3f59afb967cc5d779e4263d237bd77
UPSTREAM=${UPSTREAM:-https://github.com/danfromtico/mesa-switch.git}
HERE=$(cd "$(dirname "$0")" && pwd)
PATCH=$HERE/mesa-switch-masseffect.patch
SRC=${SRC:-$HERE/../../mesa-switch-src}
OUT=${OUT:-$HERE/../../mesa-sdk}
IMAGE=devkitpro-mesa-rust
CONTAINER=mesa-switch-masseffect-build
INCREMENTAL=0
[ "${1:-}" = "--incremental" ] && INCREMENTAL=1

mkdir -p "$(dirname "$SRC")" "$OUT"
SRC=$(cd "$SRC" 2>/dev/null && pwd || echo "$(cd "$(dirname "$SRC")" && pwd)/$(basename "$SRC")")
OUT=$(cd "$OUT" && pwd)

if [ "$INCREMENTAL" = 0 ]; then
  if [ ! -d "$SRC/.git" ]; then
    echo "== clone $UPSTREAM at $PIN"
    git clone --no-checkout "$UPSTREAM" "$SRC"
  fi
  cd "$SRC"
  if git apply --reverse --check "$PATCH" 2>/dev/null; then
    echo "== patch already applied"
  else
    git checkout -q "$PIN"
    echo "== apply $(basename "$PATCH")"
    git apply "$PATCH"
  fi
  [ "${APPLY_ONLY:-0}" = 1 ] && { echo "patched source: $SRC"; exit 0; }
  echo "== full build (build-switch.sh)"
  ./build-switch.sh
fi

cd "$SRC"
if [ "$INCREMENTAL" = 1 ]; then
  echo "== incremental ninja"
  docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
  docker run --rm --name "$CONTAINER" -v "$SRC:/project" -w /project "$IMAGE" bash -c '
    export PATH="/usr/local/libexec:/project/builddir-native/src/compiler/clc:/project/builddir-native/src/compiler/spirv:$PATH"
    ninja -C /project/builddir-switch \
      src/nouveau/vulkan/libnvk.a src/nouveau/vulkan/libvulkan.a src/nouveau/compiler/libnak_rs.a \
      src/nouveau/compiler/libnak.a'
fi

echo "== merge libnak_rs.a into libvulkan.a"
LIBDIR=$OUT/opt/devkitpro/portlibs/switch/lib
mkdir -p "$LIBDIR"
docker run --rm -v "$SRC:/project" -w /project/builddir-switch "$IMAGE" bash -c '
  AR=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar
  printf "CREATE /project/builddir-switch/libvulkan_merged.a\nADDLIB src/nouveau/vulkan/libvulkan.a\nADDLIB src/nouveau/compiler/libnak_rs.a\nSAVE\nEND\n" | $AR -M'
mv "$SRC/builddir-switch/libvulkan_merged.a" "$LIBDIR/libvulkan.a"
cat > "$OUT/SOURCE.txt" <<EOT
mesa-switch $PIN + masseffect-nx mesa/mesa-switch-masseffect.patch
built with build-switch.sh (Docker), libvulkan.a = builddir-switch libvulkan.a + libnak_rs.a merged with ar -M
EOT
ls -la "$LIBDIR/libvulkan.a"
echo "Use it with:  MESA_SDK=$OUT tools/build_nro.sh"
echo "(-DREXGLUE_SWITCH_NVK_SDK=$OUT/opt/devkitpro/portlibs/switch). Delete the NRO build dir so it relinks."

#!/bin/bash
# Windows (MSYS2 MINGW64) variant. NOT tested for this project's patch: it follows the build of the earlier Windows
# port that mesa-switch's own build-unified.sh was written for. The macOS/Linux Docker script, build_mesa_docker.sh,
# is the tested path.
#
# First do the full build once with mesa-switch's build-unified.sh (see its README), in a source tree prepared like:
#     git clone https://github.com/danfromtico/mesa-switch.git && cd mesa-switch
#     git checkout d4a00ea0ab3f59afb967cc5d779e4263d237bd77
#     git apply /path/to/masseffect-nx/mesa/mesa-switch-masseffect.patch
# Then this script rebuilds incrementally (ninja on builddir-unified) and installs only what changed:
#
#   MESA=/c/src/mesa-switch DEVKITPRO=/c/devkitPro ./build_mesa_msys2.sh
#
# builddir-unified was configured with the GNU Rust toolchain (its proc macros are GNU DLLs). If rustup defaults to
# MSVC, the build fails; RUSTUP_TOOLCHAIN pins the GNU one.
set -u
MESA=${MESA:?set MESA to the patched mesa-switch source tree}
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export RUSTUP_TOOLCHAIN=${RUSTUP_TOOLCHAIN:-stable-x86_64-pc-windows-gnu}
INSTALL=${INSTALL:-$MESA/mesa-unified-install}
cd "$MESA" || exit 1
echo "== ninja"
ninja -C builddir-unified -j2 || exit $?
echo "== meson install"
meson install -C builddir-unified --destdir "$INSTALL" --only-changed 2>&1 | tail -3
echo "== libraries"
find "$INSTALL" \( -name "libnvk.a" -o -name "libvulkan.a" -o -name "libnak*.a" \) -exec ls -la {} \;
echo "Merge libnak_rs.a into libvulkan.a if the install does not already do it (see build_mesa_docker.sh)."

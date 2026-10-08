#!/bin/bash
# Variant build of the patched Mesa with another -Doptimization, in its own build dir (builddir-switch-o<OPT>), so the
# other build dirs and out/mesa-sdk are not touched. Needs a previous full build of SRC (builddir-native host
# tools) and the devkitpro-mesa-rust image (mesa/build_mesa_docker.sh). About 1 minute on Apple silicon.
# The default driver is -O2 (mesa/build_mesa_docker.sh, MESA_OPT=2); this script makes the others quickly:
#   OPT=1 mesa/build_mesa_opt.sh        -> out/mesa-sdk-o1 ; the -O1 fallback, used by MESA_OPT=1 tools/build_nro.sh
#   OPT=3 mesa/build_mesa_opt.sh        -> out/mesa-sdk-o3 ; experiments, link with MESA_SDK=out/mesa-sdk-o3
#   OPT=2 OUT=out/mesa-sdk mesa/build_mesa_opt.sh   -> refresh the default -O2 driver from builddir-switch-o2
# See docs/nvk-per-draw.md.
set -euo pipefail
OPT=${OPT:-2}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC:-$HERE/../out/mesa-switch-corrected}
OUT=${OUT:-$HERE/../out/mesa-sdk-o$OPT}
BD=builddir-switch-o$OPT
C=mesa-switch-o$OPT-build
JOBS=${JOBS:-6}
start=$(date +%s)
docker rm -f $C >/dev/null 2>&1 || true
docker run -d --name $C -v "$SRC:/project" --workdir /project devkitpro-mesa-rust sleep infinity >/dev/null
run() { docker exec $C bash -c "$1"; }
run 'mkdir -p /usr/local/libexec
cp /project/bindgen-switch-wrapper.sh /usr/local/libexec/bindgen
cp /project/rustc-switch-wrapper.sh /usr/local/libexec/rustc
cp /project/bindgen-atomic-shim.h /usr/local/libexec/bindgen-atomic-shim.h
chmod +x /usr/local/libexec/bindgen /usr/local/libexec/rustc
M="$(command -v meson)"; mkdir -p /usr/local/bin; [ "$M" = /usr/local/bin/meson ] || ln -sf "$M" /usr/local/bin/meson
cp /project/src/gallium/winsys/nouveau/drm/nouveau.h /opt/devkitpro/portlibs/switch/include/
cp /project/src/nouveau/headers/nv_device_info.h /opt/devkitpro/portlibs/switch/include/
[ -d /usr/lib/llvm-15/lib/clang/15/include ] || ln -sf /usr/lib/llvm-15/lib/clang/15.0.6/include /usr/lib/llvm-15/lib/clang/15/include'
run "export PATH=/usr/local/libexec:/project/builddir-native/src/compiler/clc:/project/builddir-native/src/compiler/spirv:\$PATH
cd /project && { [ -f $BD/build.ninja ] || meson setup $BD --cross-file switch_cross_file.txt --buildtype=release \
 -Doptimization=$OPT -Db_lto=false -Db_ndebug=true -Dvulkan-drivers=nouveau -Dgallium-drivers=nouveau -Dshader-cache=true \
 -Dgallium-rusticl=false -Dplatforms=switch -Dglx=disabled -Degl=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
 -Dllvm=disabled -Dshared-glapi=disabled -Dshared-llvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system -Dcpp_rtti=false; }"
echo "configured after $(( $(date +%s) - start )) s"
run "export PATH=/usr/local/libexec:/project/builddir-native/src/compiler/clc:/project/builddir-native/src/compiler/spirv:\$PATH
ninja -j$JOBS -C /project/$BD src/nouveau/vulkan/libnvk.a src/nouveau/vulkan/libvulkan.a src/nouveau/compiler/libnak_rs.a src/nouveau/compiler/libnak.a"
echo "built after $(( $(date +%s) - start )) s"
mkdir -p "$OUT/opt/devkitpro/portlibs/switch/lib"
run "cd /project/$BD && printf 'CREATE /project/$BD/libvulkan_merged.a\nADDLIB src/nouveau/vulkan/libvulkan.a\nADDLIB src/nouveau/compiler/libnak_rs.a\nSAVE\nEND\n' | /opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar -M"
mv "$SRC/$BD/libvulkan_merged.a" "$OUT/opt/devkitpro/portlibs/switch/lib/libvulkan.a"
echo "mesa-switch d4a00ea0ab3 + mesa-switch-masseffect.patch, -Doptimization=$OPT ($BD), merged libvulkan.a + libnak_rs.a" > "$OUT/SOURCE.txt"
docker stop $C >/dev/null; docker rm $C >/dev/null
ls -la "$OUT/opt/devkitpro/portlibs/switch/lib/libvulkan.a"
echo "total $(( $(date +%s) - start )) s"

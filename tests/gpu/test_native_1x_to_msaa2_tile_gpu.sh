#!/bin/bash
# Exact production utility shader, headless proof only. No main build/game.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
sdk="${VULKAN_SDK:?set VULKAN_SDK to the Vulkan SDK root (the folder with bin/ and lib/)}"
result="${1:-$repo/out/tests/1x-to-msaa2-production-tile-01}"
if [[ -e "$result" ]]; then echo "FAIL result directory already exists: $result" >&2; exit 1; fi
mkdir -p "$result"
shader="$repo/app/src/native/masseffect/shaders/me_edram_depth_1x_to_depth_msaa2.frag"
"$sdk/bin/glslangValidator" -V --target-env vulkan1.2 "$shader" -o "$result/transfer.frag.spv"
"$sdk/bin/glslangValidator" -V --target-env vulkan1.2 -x "$shader" -o "$result/production.inc"
cmp "$result/production.inc" "$repo/app/src/native/masseffect/me_edram_depth_1x_to_depth_msaa2.inc"
"$sdk/bin/spirv-val" --target-env vulkan1.2 "$result/transfer.frag.spv"
for pair in "test_native_msaa_tile.vert:tile.vert" "test_native_msaa2_to_1x_tile_init.frag:init.frag" \
  "test_native_msaa2_to_1x_tile_extract.comp:extract.comp"; do
  source="${pair%%:*}"; output="${pair##*:}"
  "$sdk/bin/glslangValidator" -V --target-env vulkan1.2 "$repo/tests/gpu/$source" -o "$result/$output.spv"
  "$sdk/bin/spirv-val" --target-env vulkan1.2 "$result/$output.spv"
done
python3 "$repo/tests/gpu/make_native_1x_to_msaa2_tile_mutants.py" "$shader" "$result"
for mode in swap collapse skip; do
  "$sdk/bin/glslangValidator" -V --target-env vulkan1.2 "$result/negative-$mode.frag" -o "$result/negative-$mode.frag.spv"
  "$sdk/bin/spirv-val" --target-env vulkan1.2 "$result/negative-$mode.frag.spv"
done
clang++ -std=c++20 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers \
  -I"$sdk/include" -I"$repo/app/src/native" -I"$repo/app/src/native/masseffect" "$repo/tests/gpu/test_native_1x_to_msaa2_tile_gpu.cpp" \
  -L"$sdk/lib" -lvulkan -Wl,-rpath,"$sdk/lib" -o "$result/production-tile-proof"
export VK_ICD_FILENAMES="$sdk/share/vulkan/icd.d/MoltenVK_icd.json"
export VK_LAYER_PATH="$sdk/share/vulkan/explicit_layer.d"
for mode in half unhalf unorm; do
  if [[ "$mode" == half ]]; then
    "$result/production-tile-proof" "$result" >"$result/proof-$mode.log" 2>&1
  else
    "$result/production-tile-proof" "$result" "$mode" >"$result/proof-$mode.log" 2>&1
  fi
  grep -Eq 'PASS physical1x-to-2x production72 D32/S8 exact' "$result/proof-$mode.log"
  grep -Eq 'productionSpvBytes=.* exactEmbeddedBytes=true pushABI=72 depthBinding=0 stencilBinding=1' "$result/proof-$mode.log"
  grep -Eq 'D32BitMismatches=0 S8Mismatches=0 sourceMismatches=0 outsideMismatches=0 staleObservers=0 fenceComplete=true' "$result/proof-$mode.log"
  grep -Eq 'validationErrors=0 explicitLayer=true debugCallback=true cleanupComplete=true' "$result/proof-$mode.log"
done
for mode in swap collapse skip unknown-format mixed-format half-mismatch grid collapsed 64bpp bad-mask zero-pitch dim-mismatch wrong-tile; do
  set +e
  "$result/production-tile-proof" "$result" "$mode" >"$result/negative-$mode.log" 2>&1
  status=$?
  set -e
  [[ "$status" == 1 ]]
  grep -Eq 'FAIL exact tile transfer comparison failed' "$result/negative-$mode.log"
  case "$mode" in
    swap) grep -Eq 'D32BitMismatches=960 S8Mismatches=960' "$result/negative-$mode.log";;
    collapse) grep -Eq 'D32BitMismatches=480 S8Mismatches=480' "$result/negative-$mode.log";;
    skip) grep -Eq 'D32BitMismatches=8 S8Mismatches=8' "$result/negative-$mode.log";;
    wrong-tile) grep -Eq 'D32BitMismatches=960 S8Mismatches=960' "$result/negative-$mode.log";;
    *) grep -Eq 'D32BitMismatches=960 S8Mismatches=960' "$result/negative-$mode.log";
       grep -Eq 'guardRejectedROIUnchangedSamples=960 / 960' "$result/negative-$mode.log";;
  esac
  grep -Eq 'sourceMismatches=0 outsideMismatches=0 staleObservers=0 fenceComplete=true' "$result/negative-$mode.log"
  grep -Eq 'validationErrors=0 explicitLayer=true debugCallback=true cleanupComplete=true' "$result/negative-$mode.log"
  if grep -E -q 'VALIDATION ERROR|VUID-' "$result/negative-$mode.log"; then exit 1; fi
done
echo "PASS exact production72 shader: physical1x-to-2x three formats + thirteen actual-comparison negatives: $result"



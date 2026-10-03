#!/bin/bash
# Isolated headless proof; never builds or launches the main application.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
sdk="${VULKAN_SDK:?set VULKAN_SDK to the Vulkan SDK root (the folder with bin/ and lib/)}"
result="${1:-$repo/out/tests/msaa2-gpu-tile-repro-01}"
if [[ -e "$result" ]]; then echo "FAIL result directory already exists: $result" >&2; exit 1; fi
mkdir -p "$result"
clang++ -std=c++20 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers \
  -I"$sdk/include" -I"$repo/app/src/native" -I"$repo/app/src/native/masseffect" "$repo/tests/gpu/test_native_msaa_tile_gpu.cpp" \
  -L"$sdk/lib" -lvulkan -Wl,-rpath,"$sdk/lib" -o "$result/tile-proof"
for pair in "test_native_msaa_tile.vert:tile.vert" "test_native_msaa_tile_init.frag:init.frag" \
  "test_native_msaa_tile_transfer.frag:transfer.frag" "test_native_msaa_tile_extract.comp:extract.comp"; do
  source="${pair%%:*}"; output="${pair##*:}"
  "$sdk/bin/glslangValidator" -V --target-env vulkan1.2 "$repo/tests/gpu/$source" -o "$result/$output.spv"
  "$sdk/bin/spirv-val" --target-env vulkan1.2 "$result/$output.spv"
done
export VK_ICD_FILENAMES="$sdk/share/vulkan/icd.d/MoltenVK_icd.json"
export VK_LAYER_PATH="$sdk/share/vulkan/explicit_layer.d"
"$result/tile-proof" "$result" >"$result/proof.log" 2>&1
grep -Eq 'PASS true2x D32/S8 exact' "$result/proof.log"
grep -Eq 'validationErrors=0 explicitLayer=true debugCallback=true cleanupComplete=true' "$result/proof.log"
for mode in swap collapse skip; do
  set +e
  "$result/tile-proof" "$result" "$mode" >"$result/negative-$mode.log" 2>&1
  status=$?
  set -e
  [[ "$status" == 1 ]]
  grep -Eq 'FAIL exact tile transfer comparison failed' "$result/negative-$mode.log"
  grep -Eq 'validationErrors=0 explicitLayer=true debugCallback=true cleanupComplete=true' "$result/negative-$mode.log"
  grep -Eq 'sourceMismatches=0 staleObservers=0 fenceComplete=true' "$result/negative-$mode.log"
  case "$mode" in
    swap) grep -Eq 'D32BitMismatches=960 S8Mismatches=960' "$result/negative-$mode.log";;
    collapse) grep -Eq 'D32BitMismatches=480 S8Mismatches=480' "$result/negative-$mode.log";;
    skip) grep -Eq 'D32BitMismatches=8 S8Mismatches=8' "$result/negative-$mode.log";;
  esac
  if grep -E -q 'VALIDATION ERROR|VUID-' "$result/negative-$mode.log"; then exit 1; fi
done
echo "PASS tile positive + all three actual-comparison negatives: $result"


#!/bin/bash
# Isolated raw-coordinate proof; no main build or game changes.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
sdk="${VULKAN_SDK:?set VULKAN_SDK to the Vulkan SDK root (the folder with bin/ and lib/)}"
result="${1:?fresh output directory required}"
if [[ -e "$result" ]]; then echo 'FAIL existing result directory'; exit 1; fi
mkdir -p "$result"
clang++ -std=c++20 -O2 -Wall -Wextra -Werror -I"$repo/app/src/native" -I"$repo/app/src/native/masseffect" "$repo/tests/cpu/test_native_fragcoord_xy_spirv.cpp" -o "$result/transform"
"$result/transform" "$result/fixtures" >"$result/cpu.log"
for file in "$result"/fixtures/*.spv; do "$sdk/bin/spirv-val" --target-env vulkan1.2 "$file"; done
for stage in vert frag; do
 "$sdk/bin/glslangValidator" -V --target-env vulkan1.2 "$repo/tests/gpu/test_native_fragcoord_xy_gpu.$stage" -o "$result/input.$stage.spv"
 "$sdk/bin/spirv-val" --target-env vulkan1.2 "$result/input.$stage.spv"
done
for mode in 1 2 3; do
 "$result/transform" --transform "$mode" "$result/input.frag.spv" "$result/mode$mode.frag.spv"
 "$sdk/bin/spirv-val" --target-env vulkan1.2 "$result/mode$mode.frag.spv"
done
clang++ -std=c++20 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -I"$sdk/include" -I"$repo/app/src/native" -I"$repo/app/src/native/masseffect" \
 "$repo/tests/gpu/test_native_fragcoord_xy_gpu.cpp" -L"$sdk/lib" -lvulkan \
 -Wl,-rpath,"$sdk/lib" -o "$result/gpu"
export VK_ICD_FILENAMES="$sdk/share/vulkan/icd.d/MoltenVK_icd.json"
export VK_LAYER_PATH="$sdk/share/vulkan/explicit_layer.d"
export DYLD_LIBRARY_PATH="$sdk/lib"
for mode in 1 2 3; do
 "$result/gpu" "$result/input.vert.spv" "$result/mode$mode.frag.spv" "$mode" >"$result/mode$mode.log" 2>&1
done
# Actual wrong modules: grid scale instead of phase2, and opposite phase Y sign.
for mutant in wrong-scale wrong-sign; do
 selected=1; [[ "$mutant" == wrong-sign ]] && selected=2
 if "$result/gpu" "$result/input.vert.spv" "$result/mode$selected.frag.spv" 3 >"$result/$mutant.log" 2>&1; then
  echo 'FAIL negative unexpectedly passed'; exit 1
 fi
 grep -Eq '^FAIL inverse XY mismatch$' "$result/$mutant.log"
 grep -Eq 'XYmismatches=64/64 ZWmismatches=0 staleObservers=0 fenceComplete=true validationErrors=0 cleanupComplete=true' "$result/$mutant.log"
done
if grep -E 'VUID-|validation ERROR' "$result"/*.log; then exit 1; fi
echo "PASS three inverse XY GPU profiles + two wrong-module negatives: $result"

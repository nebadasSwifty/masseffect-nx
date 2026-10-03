#!/bin/bash
# Isolated headless GPU proof, never builds/launches game or shared renderer.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
sdk="${VULKAN_SDK:?set VULKAN_SDK to the Vulkan SDK root (the folder with bin/ and lib/)}"
result="${1:-$repo/out/tests/msaa2-guest-resolve-repro-01}"
if [[ -e "$result" ]]; then echo "FAIL result directory exists: $result" >&2; exit 1; fi
mkdir -p "$result"
clang++ -std=c++20 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers \
 -I"$sdk/include" -I"$repo/app/src/native" -I"$repo/app/src/native/masseffect" "$repo/tests/gpu/test_native_msaa_resolve_gpu.cpp" \
 -L"$sdk/lib" -lvulkan -Wl,-rpath,"$sdk/lib" -o "$result/resolve-proof"
for pair in "tests/gpu/test_native_msaa_tile.vert:tile.vert" "tests/gpu/test_native_msaa_tile_init.frag:init.frag" \
 "tests/gpu/test_native_msaa_tile_extract.comp:extract.comp" "tests/gpu/test_native_msaa_resolve_extract.comp:resolve-extract.comp" \
 "app/src/native/masseffect/shaders/me_depth_resolve_guestspace_msaa2.comp:resolve.comp"; do
 source="${pair%%:*}"; output="${pair##*:}"
 "$sdk/bin/glslangValidator" -V --target-env vulkan1.2 "$repo/$source" -o "$result/$output.spv"
 "$sdk/bin/spirv-val" --target-env vulkan1.2 "$result/$output.spv"
done
export VK_ICD_FILENAMES="$sdk/share/vulkan/icd.d/MoltenVK_icd.json"
export VK_LAYER_PATH="$sdk/share/vulkan/explicit_layer.d"
for raw in 0 1 2 3 4 5 6 7; do
 "$result/resolve-proof" "$result" "$raw" >"$result/select-$raw.log" 2>&1
 grep -Eq 'PASS native2x sample-select' "$result/select-$raw.log"
 grep -Eq 'R32BitMismatches=0 sourceD32S8Mismatches=0 staleObservers=0 changedPixels=651 outsidePixels=3189 fenceComplete=true' "$result/select-$raw.log"
 grep -Eq 'validationErrors=0 explicitLayer=true debugCallback=true cleanupComplete=true' "$result/select-$raw.log"
done
for mode in wrong-host unsanitized; do
 raw=0; [[ "$mode" == unsanitized ]] && raw=4
 set +e
 "$result/resolve-proof" "$result" "$raw" "$mode" >"$result/negative-$mode.log" 2>&1
 status=$?
 set -e
 [[ "$status" == 1 ]]
 grep -Eq 'FAIL exact MSAA guest resolve comparison failed' "$result/negative-$mode.log"
 grep -Eq 'R32BitMismatches=651 sourceD32S8Mismatches=0 staleObservers=0' "$result/negative-$mode.log"
 grep -Eq 'validationErrors=0 explicitLayer=true debugCallback=true cleanupComplete=true' "$result/negative-$mode.log"
 if grep -E -q 'VALIDATION ERROR|VUID-' "$result/negative-$mode.log"; then exit 1; fi
done
set +e
"$result/resolve-proof" "$result" 8 >"$result/negative-raw-field.log" 2>&1
status=$?
set -e
[[ "$status" == 1 ]]
grep -Eq 'FAIL RB_COPY_CONTROL sample selector outside3bitfield' "$result/negative-raw-field.log"
echo "PASS ALL8SDKselectors + wronghost/unsanitizedactualcomparison + CPUinvalidfield: $result"


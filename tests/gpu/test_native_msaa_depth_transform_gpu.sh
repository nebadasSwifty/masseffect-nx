#!/bin/bash
# Validate the actual dormant production transform, not a hand-written lookalike.
# Reuse the frozen plane observer unchanged. Does not launch/build the game.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
sdk="${VULKAN_SDK:?set VULKAN_SDK to the Vulkan SDK root (the folder with bin/ and lib/)}"
result="${1:?usage: bash tests/gpu/test_native_msaa_depth_transform_gpu.sh NEW_RESULT_DIRECTORY}"
if [[ -e "$result" ]]; then echo "FAIL result directory already exists: $result" >&2; exit 1; fi
mkdir -p "$result"
clang++ -std=c++20 -O2 -Wall -Wextra -Werror \
  -I"$repo/app/src/native" -I"$repo/app/src/native/masseffect" "$repo/tests/cpu/test_native_depth_quantize_spirv.cpp" -o "$result/transform-cpu-proof"
clang++ -std=c++20 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers \
  -I"$sdk/include" -I"$repo/app/src/native" -I"$repo/app/src/native/masseffect" "$repo/tests/gpu/test_native_msaa_depth_gpu.cpp" \
  -L"$sdk/lib" -lvulkan -Wl,-rpath,"$sdk/lib" -o "$result/plane-proof"
for shader in plane.vert plane.frag extract.comp quant.frag; do
  "$sdk/bin/glslangValidator" -V --target-env vulkan1.2 \
    "$repo/tests/gpu/test_native_msaa_depth_$shader" -o "$result/$shader.spv"
done
"$sdk/bin/glslangValidator" -V --target-env vulkan1.2 \
  "$repo/tests/gpu/test_native_msaa_depth_transform.frag" -o "$result/original-no-depth.spv"
"$result/transform-cpu-proof" "$result" "$result/original-no-depth.spv" >"$result/cpu.log" 2>&1
# Mechanical artifact copy: the unchanged observer calls this candidate derivative.frag.spv.
cp "$result/real-native2x-2.spv" "$result/derivative.frag.spv"
for shader in "$result"/*.spv; do "$sdk/bin/spirv-val" --target-env vulkan1.2 "$shader"; done
export VK_ICD_FILENAMES="$sdk/share/vulkan/icd.d/MoltenVK_icd.json"
export VK_LAYER_PATH="$sdk/share/vulkan/explicit_layer.d"
export DYLD_LIBRARY_PATH="$sdk/lib"
"$result/plane-proof" "$result" derivative >"$result/proof.log" 2>&1
grep -Eq '^validationErrors=0 explicitRequiredLayer=true$' "$result/proof.log"
[[ "$(grep -Ec '^PASS plane=.*quant=1 .*sampleInvocations=16,16 fenceComplete=true$' "$result/proof.log")" == 5 ]]
set +e
"$result/plane-proof" "$result" >"$result/negative-naive.log" 2>&1
status=$?
set -e
[[ "$status" == 1 ]]
grep -Eq '^FAIL sample plane mismatch$' "$result/negative-naive.log"
if grep -E -n 'VUID-|validation ERROR:' "$result/proof.log" "$result/negative-naive.log"; then exit 1; fi
echo "PASS actual native2x production-transform candidate + naive negative: $result"
echo 'Bounded plane/sample proof only; no material interpolation, game-camera or Switch acceptance.'

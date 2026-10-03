#!/bin/bash
# Independent headless GPU fixture. Never builds or runs the game.
set -euo pipefail
cd "$(dirname "$0")/../.."
test_output=${1:?usage: tests/gpu/test_native_msaa_stencil_gpu.sh NEW_OUTPUT_DIRECTORY}
test_sdk=${VULKAN_SDK:?set VULKAN_SDK}
if [ -e "$test_output" ]; then echo "Output already exists: $test_output" >&2; exit 1; fi
mkdir -p "$test_output"
"$test_sdk/bin/glslangValidator" -V --target-env vulkan1.2 tests/gpu/test_native_msaa_depth_plane.vert -o "$test_output/plane.vert.spv"
"$test_sdk/bin/glslangValidator" -V --target-env vulkan1.2 tests/gpu/test_native_msaa_depth_plane.frag -o "$test_output/plane.frag.spv"
"$test_sdk/bin/glslangValidator" -V --target-env vulkan1.2 tests/gpu/test_native_msaa_stencil_extract.comp -o "$test_output/extract.comp.spv"
"$test_sdk/bin/glslangValidator" -V --target-env vulkan1.2 tests/gpu/test_native_msaa_stencil_extract_no_depth.comp -o "$test_output/extract-no-depth.comp.spv"
for module in "$test_output"/*.spv; do "$test_sdk/bin/spirv-val" --target-env vulkan1.2 "$module"; done
clang++ -std=c++20 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers \
  -I"$test_sdk/include" -I"$PWD/app/src/native" -I"$PWD/app/src/native/masseffect" tests/gpu/test_native_msaa_stencil_gpu.cpp \
  -L"$test_sdk/lib" -lvulkan -Wl,-rpath,"$test_sdk/lib" \
  -o "$test_output/test_native_msaa_stencil_gpu"
VK_ICD_FILENAMES="$test_sdk/share/vulkan/icd.d/MoltenVK_icd.json" \
VK_LAYER_PATH="$test_sdk/share/vulkan/explicit_layer.d" \
  "$test_output/test_native_msaa_stencil_gpu" "$test_output" > "$test_output/proof.log" 2>&1
cat "$test_output/proof.log"
set +e
VK_ICD_FILENAMES="$test_sdk/share/vulkan/icd.d/MoltenVK_icd.json" \
VK_LAYER_PATH="$test_sdk/share/vulkan/explicit_layer.d" \
  "$test_output/test_native_msaa_stencil_gpu" "$test_output" negative-no-depth > "$test_output/negative-no-depth.log" 2>&1
negative_status=$?
set -e
if [ "$negative_status" -ne 1 ]; then echo "negative fixture did not fail correctly" >&2; exit 1; fi
grep -Eq 'actualBits=7FCABCDE.*all32S8Exact=true callbackErrors=0 fenceComplete=true' "$test_output/negative-no-depth.log"
if grep -E -q 'VALIDATION ERROR|VUID' "$test_output/negative-no-depth.log"; then echo "negative fixture validation failure" >&2; exit 1; fi
cat "$test_output/negative-no-depth.log"
echo 'PASS negative observer freshness: skipped D32 stores detected by poison, all S8 fresh, zero validation errors'

#!/bin/bash
# Optional macOS/MoltenVK GPU regression. No game process or renderer changes.
set -euo pipefail
cd "$(dirname "$0")/../.."
gpu_sdk_root=${1:?usage: tests/gpu/test_native_msaa_depth_gpu.sh /path/to/VulkanSDK/macOS}
gpu_icd="$gpu_sdk_root/share/vulkan/icd.d/MoltenVK_icd.json"
gpu_layers="$gpu_sdk_root/share/vulkan/explicit_layer.d"
for gpu_required in "$gpu_sdk_root/bin/glslangValidator" "$gpu_sdk_root/bin/spirv-val" \
 "$gpu_icd" \
    "$gpu_layers/VkLayer_khronos_validation.json"; do
  [[ -e "$gpu_required" ]] || { echo "Missing GPU test dependency: $gpu_required" >&2; exit 2; }
done
gpu_artifacts=$(mktemp -d "${TMPDIR:-/tmp}/me-msaa-depth-proof.XXXXXX")
echo "GPU proof artifacts (retained): $gpu_artifacts"
gpu_compiler=${CXX:-clang++}
"$gpu_compiler" -std=c++20 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers \
  -I"$gpu_sdk_root/include" -I"$PWD/app/src/native" -I"$PWD/app/src/native/masseffect" tests/gpu/test_native_msaa_depth_gpu.cpp \
  -L"$gpu_sdk_root/lib" -lvulkan -Wl,-rpath,"$gpu_sdk_root/lib" \
  -o "$gpu_artifacts/test_native_msaa_depth_gpu"
for gpu_shader in plane.vert plane.frag extract.comp quant.frag derivative.frag; do
  "$gpu_sdk_root/bin/glslangValidator" -V --target-env vulkan1.2 \
    "tests/gpu/test_native_msaa_depth_$gpu_shader" -o "$gpu_artifacts/$gpu_shader.spv"
  "$gpu_sdk_root/bin/spirv-val" --target-env vulkan1.2 "$gpu_artifacts/$gpu_shader.spv"
done
gpu_run() {
  env VK_ICD_FILENAMES="$gpu_icd" VK_LAYER_PATH="$gpu_layers" \
    DYLD_LIBRARY_PATH="$gpu_sdk_root/lib" \
    "$gpu_artifacts/test_native_msaa_depth_gpu" "$gpu_artifacts" "$@"
}
gpu_run plain >"$gpu_artifacts/plain.log" 2>&1
gpu_run derivative >"$gpu_artifacts/derivative.log" 2>&1
# The naive quantizer must fail for the observed center-depth assumption, not
# for a missing dependency, an unsupported feature or an unrelated error.
if gpu_run >"$gpu_artifacts/negative.log" 2>&1; then
  echo 'Negative control unexpectedly passed; investigate before accepting this device' >&2
  exit 1
fi
grep -Eq '^FAIL sample plane mismatch$' "$gpu_artifacts/negative.log" || {
  echo 'Negative control did not fail for the expected reason' >&2
  exit 1
}
if grep -E -n 'VUID-|validation ERROR:' "$gpu_artifacts/plain.log" \
    "$gpu_artifacts/derivative.log" "$gpu_artifacts/negative.log"; then
  echo 'GPU proof contains validation diagnostics; rejected' >&2
  exit 1
fi
for gpu_log in plain derivative; do
  grep -Eq '^validationErrors=0 explicitRequiredLayer=true$' "$gpu_artifacts/$gpu_log.log"
done
tail -n 12 "$gpu_artifacts/derivative.log"
echo 'Bounded depth/sample proof passed; naive negative control failed as expected.'
echo 'Not game-camera, full FLOAT24, stencil/ownership or Switch acceptance.'

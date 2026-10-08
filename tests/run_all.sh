#!/usr/bin/env bash
# Builds and runs the standalone tests of the port: no console, no game files, no generated code.
#
#   tests/run_all.sh                 CPU contract tests (C++) and the Python tests
#   tests/run_all.sh --gpu           also the headless Vulkan proofs (needs VULKAN_SDK, MoltenVK on macOS)
#   tests/run_all.sh [--gpu] NAME... only the tests whose name contains one of the NAMEs
#
# Environment:
#   CXX          C++ compiler (default: clang++, else g++)
#   PYTHON       Python interpreter (default: python3)
#   VULKAN_SDK   root of the LunarG Vulkan SDK (the folder with bin/ and lib/); only for --gpu
#   SDK_THIRDPARTY  folder with the SDK third-party sources (default: sdk/thirdparty)
#   OUT          build and result folder (default: <repo>/out/tests)
#
# A test is "skipped" when a dependency is missing (the SDK third-party sources for the tests that include SDK
# headers, NEON on a non-ARM host); skipped tests do not fail the run. Exit status 1 if any test fails.
# These tests check the logic of the app headers; they say nothing about speed or image quality on the console.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="${OUT:-$ROOT/out/tests}"
PYTHON="${PYTHON:-python3}"
if [[ -z "${CXX:-}" ]]; then
  if command -v clang++ >/dev/null 2>&1; then CXX=clang++; else CXX=g++; fi
fi

RUN_GPU=0
FILTERS=()
for arg in "$@"; do
  case "$arg" in
    --gpu) RUN_GPU=1 ;;
    -h|--help) sed -n "2,18p" "$0"; exit 0 ;;
    *) FILTERS+=("$arg") ;;
  esac
done
mkdir -p "$OUT/cpu" "$OUT/gpu"

APP="$ROOT/app/src"
SDK="$ROOT/sdk"
INC=(-I"$APP/native" -I"$APP/native/masseffect")
# Header-only third-party code needed by some tests; fetched by tools/fetch_thirdparty.py into sdk/thirdparty.
THIRD="${SDK_THIRDPARTY:-$SDK/thirdparty}"
XXHASH="$THIRD/xxHash"

passed=0; failed=0; skipped=0
failures=()

selected() {
  [[ ${#FILTERS[@]} -eq 0 ]] && return 0
  local f
  for f in "${FILTERS[@]}"; do [[ "$1" == *"$f"* ]] && return 0; done
  return 1
}
report() {  # status name [detail]
  case "$1" in
    PASS) passed=$((passed + 1)); printf '  PASS  %s\n' "$2" ;;
    SKIP) skipped=$((skipped + 1)); printf '  SKIP  %s (%s)\n' "$2" "$3" ;;
    FAIL) failed=$((failed + 1)); failures+=("$2"); printf '  FAIL  %s\n' "$2"
          [[ -n "${3:-}" ]] && sed 's/^/        /' "$3" | tail -n 15 ;;
  esac
}

# ---------------------------------------------------------------------------------------------------------------
# CPU contract tests: one executable per tests/cpu/test_*.cpp, built against the app headers only.
# ---------------------------------------------------------------------------------------------------------------
echo "== CPU contract tests (C++, $CXX)"
for src in "$HERE"/cpu/test_*.cpp; do
  name="$(basename "$src" .cpp)"
  selected "$name" || continue
  extra_inc=(); extra_src=(); std=c++20; strict=(-Wall -Wextra -Werror); run_args=()
  case "$name" in
    test_native_crc_fingerprint|test_native_frame_coherence)
      # Benchmark plus equality checks of the texture fingerprint against xxHash.
      [[ -f "$XXHASH/xxhash.h" ]] || { report SKIP "$name" "run tools/fetch_thirdparty.py for xxHash"; continue; }
      extra_inc=(-I"$XXHASH") ;;
    test_native_draw_extent_estimator)
      # Links the SDK memory subsystem (platform sources and libc headers of the Switch build): only built there.
      report SKIP "$name" "needs the SDK memory subsystem; not buildable standalone"; continue ;;
    test_native_ps_no_kill)
      # These use the SDK's Xenos shader microcode interpreter, compiled from the SDK sources.
      std=c++23; strict=(-w)
      extra_inc=(-I"$SDK/include" -I"$SDK/src/core")
      extra_src=("$SDK/src/graphics/register_file.cpp" "$SDK/src/graphics/format/ucode.cpp"
                 "$SDK/src/graphics/pipeline/shader/interpreter.cpp" "$SDK/src/ui/graphics_util.cpp")
      for third in fmt/include spdlog/include simde; do
        [[ -d "$THIRD/$third" ]] && extra_inc+=(-I"$THIRD/$third")
      done
      [[ -d "$THIRD/fmt/include" ]] || { report SKIP "$name" "run tools/fetch_thirdparty.py"; continue; } ;;
    test_xma_convert)
      # NEON audio conversion against the scalar loop it replaces (copies of the SDK code).
      std=c++23; strict=(-w)
      case "$(uname -m)" in arm64|aarch64) ;; *) report SKIP "$name" "needs NEON (ARM host)"; continue ;; esac ;;
    test_audio_output)
      # NEON output stage of the Switch audio system (SDK header) against the scalar code it replaced. No FP
      # contraction, like the SDK build (clang's default would fuse the scalar reference's multiply-adds).
      std=c++23; strict=(-Wall -Werror -ffp-contract=off)
      extra_inc=(-I"$SDK/include") ;;
    test_native_copy_vertices)
      case "$(uname -m)" in arm64|aarch64) ;; *) report SKIP "$name" "needs NEON (ARM host)"; continue ;; esac ;;
    test_native_fragcoord_xy_spirv)
      # Writes SPIR-V fixtures into a fresh folder given as the only argument.
      rm -rf "$OUT/cpu/$name.fixtures"; run_args=("$OUT/cpu/$name.fixtures") ;;
  esac
  log="$OUT/cpu/$name.log"
  if ! "$CXX" -std=$std -O2 "${strict[@]}" "${INC[@]}" ${extra_inc[@]+"${extra_inc[@]}"} "$src" \
        ${extra_src[@]+"${extra_src[@]}"} -pthread -o "$OUT/cpu/$name" >"$log" 2>&1; then
    report FAIL "$name (build)" "$log"; continue
  fi
  if "$OUT/cpu/$name" ${run_args[@]+"${run_args[@]}"} >>"$log" 2>&1; then report PASS "$name"
  else report FAIL "$name" "$log"; fi
done

# ---------------------------------------------------------------------------------------------------------------
# Python tests (pure Python; a test that needs a third-party module exits with 77 when it is missing = skipped).
# ---------------------------------------------------------------------------------------------------------------
echo "== Python tests"
for py in "$HERE"/tools/test_*.py; do
  name="$(basename "$py" .py)"
  selected "$name" || continue
  log="$OUT/cpu/$name.log"
  (cd "$HERE/tools" && PYTHONPATH="$ROOT/tools:$HERE/tools" "$PYTHON" "$py") >"$log" 2>&1
  rc=$?
  if [[ $rc -eq 0 ]]; then report PASS "$name"
  elif [[ $rc -eq 77 ]]; then report SKIP "$name" "$(tail -1 "$log")"
  else report FAIL "$name" "$log"; fi
done

# ---------------------------------------------------------------------------------------------------------------
# Headless Vulkan proofs of the EDRAM tile transfer, resolve and depth shaders (exact comparison against a CPU
# model, plus negative controls that must fail). Optional: they need a Vulkan implementation.
# ---------------------------------------------------------------------------------------------------------------
if [[ $RUN_GPU -eq 1 ]]; then
  echo "== GPU proofs (headless Vulkan)"
  if [[ -z "${VULKAN_SDK:-}" ]]; then
    echo "  VULKAN_SDK is not set: set it to the LunarG SDK root (folder with bin/glslangValidator, lib/, share/vulkan)." >&2
    failed=$((failed + 1)); failures+=("gpu (VULKAN_SDK not set)")
  else
    for script in "$HERE"/gpu/test_*.sh; do
      name="$(basename "$script" .sh)"
      selected "$name" || continue
      log="$OUT/gpu/$name.log"
      case "$name" in
        test_native_msaa_depth_gpu) args=("$VULKAN_SDK") ;;                  # takes the SDK root
        *) rm -rf "$OUT/gpu/$name"; args=("$OUT/gpu/$name") ;;               # takes a fresh result folder
      esac
      if bash "$script" "${args[@]}" >"$log" 2>&1; then report PASS "$name"; else report FAIL "$name" "$log"; fi
    done
  fi
fi

echo
echo "passed $passed, failed $failed, skipped $skipped (logs in $OUT)"
if [[ $failed -ne 0 ]]; then printf 'failed: %s\n' "${failures[*]}"; exit 1; fi
exit 0

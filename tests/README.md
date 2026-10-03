# Tests

Standalone tests of the port. None needs a console, game files or generated code, except the two harnesses marked
below. They check the logic of the app headers and the shaders; they say nothing about speed or image quality on the
console (that is measured with [../tools/console-test](../tools/console-test/README.md)).

    tests/run_all.sh                run the CPU and Python tests
    tests/run_all.sh --gpu          also the headless Vulkan proofs (VULKAN_SDK must point to the LunarG SDK)
    tests/run_all.sh edram resolve  only tests whose name contains one of the words

`run_all.sh` builds every test into `out/tests/` with `clang++` (or `g++`, `CXX=...`), runs it and prints one line per
test; a test whose dependency is missing is reported as skipped. The tests that include SDK headers or xxHash need the
third-party sources: run `python3 tools/fetch_thirdparty.py` first (or set `SDK_THIRDPARTY` to a folder that has them).

| Folder | What it has |
|---|---|
| `cpu/` | `test_*.cpp`: C++ tests of the headers in `app/src/native` (EDRAM layout and ownership, depth and resolve conversions, MSAA depth, draw extent, PM4 runs, record and object tables, shader identity and candidate policy, deferred recording queue stress test, texture fingerprint, vertex copy and audio conversion on ARM). `test_native_draw_extent_estimator.cpp` links the SDK memory subsystem and builds only inside the full SDK build, so the runner skips it. |
| `gpu/` | Headless Vulkan proofs with a CPU model and negative controls (a wrong shader must fail for the expected reason) of the EDRAM tile transfer, MSAA guest resolve, depth plane and quantizer, stencil and the fragment coordinate transform. `test_*_gpu.sh` builds the proof, compiles the shaders with `glslangValidator`, validates them with `spirv-val` and runs them with the Vulkan validation layer on. Needs the LunarG Vulkan SDK (MoltenVK on macOS). Tested on macOS arm64 with SDK 1.4.363. |
| `tools/` | Python tests of the scripts in `../tools` and of shader address mapping: the gap resolver, the ISO extractor (against synthetic disc images) and the compute/fragment conversion mapping. |
| `hot_fuzz/` | Differential fuzzer of the native replacements of hot game functions (`app/src/native/hot`) against the recompiled originals, on random registers and memory. Needs `app/generated/default` (run `tools/codegen.sh`) and the third-party sources; macOS arm64 or Linux aarch64. `python3 tests/hot_fuzz/build.py [--iters N] [case]` runs one case, `run_all.py` runs all of them, `liveness.py` checks that no caller reads a register the replacement does not reproduce, `autonative.py` derives a replacement mechanically from a generated leaf function. |
| `audio_dsp/` | Bit-exactness test of the native audio DSP (`app/src/native/me_audio_dsp.h`) against the recompiled originals. Same requirements as `hot_fuzz/`. `tests/audio_dsp/run.sh [iterations]`. |

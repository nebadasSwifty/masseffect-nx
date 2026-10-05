# Russian Edition (Mass Effect 1)

This document describes the port of the Russian edition of Mass Effect 1 to Nintendo Switch (`masseffect-nx-rus`), detailing the executable analysis, address mapping, subsystem adaptations, codegen gap resolution, and installer integration.

## Executable Identification

| Property | Value |
|---|---|
| Region / Edition | Russia (Ru) |
| Title ID | `4D5307E8` |
| Media ID | `572BA75D` |
| Executable Version | `0.0.0.5` |
| Base Address | `0x82000000` |
| Entry Point | `0x826D45F8` (English Rev 1 was `0x826D3CB8`) |
| Retail / Disc XEX SHA-256 | `20c619de3c31f5c4448e7c54062577df839d59b2eb5ac5c633a2953dbbe525a3` |
| Decompressed / JTAG-RIP SHA-256 | `4beb582540010b25032e3a51e4ce84a6fb1a9f381adddd8b2def0dc5d6bf715d` |
| Target NRO | `masseffect-nx-rus.nro` |

## Address Mapping & Binary Analysis

Because the Russian edition includes localized strings, altered UI layouts, and Russian font metrics, the binary shifted substantially relative to the English Rev 1 release:
- Code shifts range from `-0x210` in early graphics/Scaleform code to `+0x800` .. `+0x1000` in late CRT and audio subsystems.
- Direct linear address offsets cannot be assumed across sections.

To reliably map functions, an automated pipeline (`tools/rus_address_map.py`) was developed:
1. **Instruction Normalization:** Strips target addresses from branches and call instructions (`bl`, `b`, `bctrl`, `blr`), leaving functional mnemonic signatures.
2. **Anchor Points:** Initial seeds established from unique CRT entry points, string references, and D3D entry points.
3. **Call-Graph & Neighbor Propagation:** Matching callers and sequential function neighbors iteratively. Over 44,900 functions were mapped with high confidence (~95% coverage), including 100% of all critical hooks.

## Subsystem Adaptations

### 1. Coalesced.ini Verification
- English hash address: `0x82F7811C`
- Russian hash address: `0x82F80B1C` (`kHashAddress = 0x82F80B1C` in `app/src/masseffect_app.h`)
- Russian original SHA-1: `b91b8b7d739a6a2fe3b07456e90823ba5f2305cc` (matches `Coalesced.ini` on the Russian disc).

### 2. UI & Scaleform Post-Codegen Patches
- `tools/pch_ui_viewport.py`: `sub_82238FE8` -> `sub_82238DD8` (diff `-0x210`).
- `tools/pch_ui_world_to_screen.py`: `sub_827C07F0` -> `sub_827C10E8`.
- Both patches validated via `tools/verify_pch.sh`.

### 3. D3D9 Trace & Swap
- Swap function: `kSwap = 0x82233DF0` (was `0x82234000`).
- All 9 Direct3D guest entry points remapped in `app/src/me_d3d_trace.cpp`.
- Shader dump functions remapped in `app/src/me_shader_dump.cpp`:
  - `sub_8222F850` -> `sub_8222F640`
  - `sub_8222F1C8` -> `sub_8222EFB8`

### 4. Native Renderer & Resolution Scaling
- `GSceneRenderTargets`: `kTargets = 0x82EC287C` (was `0x82EC285C`).
- Functions remapped in `app/src/native/me_resolution.cpp`:
  - `sub_826E5858` (was `0x826E4F18`)
  - `sub_823D0CE8` (was `0x823D0F08`)
  - `sub_823D20F0` (was `0x823D2310`)
  - `sub_823D2030` (was `0x823D2250`)
  - `sub_82234B88` (was `0x82234D98`)
  - `sub_82224488` (was `0x82224698`)
  - `sub_82224318` (was `0x82224528`)
  - `sub_823CFCA8` (was `0x823CFEC8`)
- Resolution hook return addresses: `0x826E62B4`, `0x826E62E4`, `0x8223CE7C`.

### 5. Ring Wait & Occlusion
- `sub_822FE560` (was `0x822FE770`)
- `sub_82811F80` (was `0x82811690`)
- Return address `kOcclusionPollReturn = 0x826E84E0` (was `0x826E7B98`).

### 6. PhysX & Audio Hooks
- PhysX hook: `sub_8299D160` (was `0x82AF1E88`).
- Audio hooks in `app/src/native/me_audio_hooks.cpp` and `app/src/native/me_audio_dsp.h`:
  - `sub_82B1AC90` (was `0x82AF4888`)
  - `sub_82B46868` (was `0x82B215E0`)
  - `sub_82B1BE90` (was `0x82AF5AD8`)
  - `ctx.lr = 0x82B46888`
  - DSP float constants: `c_lo = 0x821BE0C0`, `c_hi = 0x82002978`, `zero = 0x82002974`.

### 7. Hot Guest Functions
- Per the acceptance criteria ("every native replacement is either verified or disabled"), the experimental hand-written hot guest implementations in `me_hot_guest.cpp` are disabled. Recompiled XEX code is used, ensuring bit-for-bit functional compatibility with the Russian release.

### 8. Linker Script & Performance Overrides
- `app/function_order.ld`: remapped function order symbols.
- `app/perf_overrides.toml`:
  - `lr_keep_returns` updated with the 4 Russian return addresses.
  - Tail-call register-sharing (`share_registers = true`) registered for all 10 `r11`/`r12` tail functions in the Russian binary (`0x82974C54`, `0x8260D804`, `0x8260D828`, `0x8260D864`, `0x8260D8C4`, `0x8260D924`, `0x8260D948`, `0x82736208`, `0x82B2A564`, `0x82B2A5A4`).

## Codegen & Gap Fixpoint

The Recompiler decompiles all reachable functions starting from the entry point and known code pointers. Due to indirect jumps and data-embedded code pointers, gaps can occur.
- Ran `tools/gap_fixpoint.py` through 13 iterative passes.
- Resolved all missing thunks and leaf routines into `app/overrides.toml` (290 lines).
- Executed `tools/codegen.sh`:
  - `verify_pch: all post-codegen patches present`
  - `direct_calls --check: 157,717 direct calls`
  - `non_volatile_as_local: ok`
  - `non_argument_as_local: ok`

## Installer Integration

`installer/config.js`, `installer/js/plan.js`, and `installer/js/source.js` were updated:
- Added `rus-rev0` edition entry supporting both retail disc SHA-256 (`20c619de...`) and decompressed/extracted SHA-256 (`4beb5825...`).
- Implemented dual-layer edition identification:
  1. **Exact SHA-256 hash match:** Matches known verified distribution hashes.
  2. **XEX2 header signature fallback:** When an unknown repack or dump is provided, the installer inspects the XEX2 header (`Title ID: 0x4D5307E8`, `Media ID: 0x572BA75D`, `Version: 0.0.0.5`, `Entry Point: 0x82812A00`, `Image Size: 16515072`). If the header matches, the installer issues an `"unverified variant"` notice while allowing the user to proceed at their own risk. Foreign headers or invalid/truncated files are rejected with descriptive errors.
- Added comprehensive unit tests in `installer/test/plan.test.js` covering exact hash matches, matching header fallback, foreign headers, and truncated/invalid XEX files. All 22 unit tests pass.

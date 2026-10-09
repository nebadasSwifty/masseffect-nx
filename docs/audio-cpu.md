# Audio CPU cost

What the game's audio costs on the CPU of the Switch, which parts are now native code, how they are verified, and what is left.
Words you do not know are in the [glossary](glossary.md). The general method is in [cpu-cost-analysis.md](cpu-cost-analysis.md).

## Where the time goes

Source: the Russian edition tour profile `run/me1/tour_20261009_004721/rex_profile.sym` (mass-effect-recomp tree), clocks
1785 / 768 MHz, `masseffect_audio_dsp_native = 1`. The guest audio thread is `XThreadEC071FA0` (priority 0x3B). It used 8-10 % of
a core in quiet places and 32-34 % in audio-heavy ones (Citadel spacewalk, combat). Self time, in % of a core:

| Function (RU / English address) | What it is | Tour average | Heaviest 10 s block |
|---|---|---|---|
| `sub_82B22740` / `sub_82AA2A00` | Reverb (XAudio effect, 256-sample blocks, two input chains): calls 5 delay-line / comb helpers per chain, then two per-sample loops over a ~310 KB state with ~150 scalar FP operations per sample, then a VMX mix-out loop | 3.9 | 0.9 (2.7 % of the thread) |
| `sub_82B2D4F0` / `sub_82AAFB90` | Voice resampler, mono: s16 -> float, linear interpolation, 32.32 position, volume ramp | 1.3-3.4 | 4.2 (12.2 % of the thread) |
| `me::audio_dsp::detail::ChainPhase` | Our native smoother (`sub_82B46868` / `sub_82B4D580`): the serial one-pole recurrence | 1.3-2.3 | 3.8 (11.1 %) |
| `vld1q_u8` / `vst1q_u8` | Not functions: the profile symbolizer names the inlined innermost frame. These are the simde vector loads/stores inside the generated VMX code (mostly the resamplers) | 2.0-3.4 | 4.5 (13 %) |
| `sub_829730C0` / `sub_82AC4A50` | CRT memset (the mixer clears its buffers) | 0.4-0.9 | 2.1 (6.1 %) |
| `sub_82B2D780` / `sub_82AAFE20` | Voice resampler, stereo (interleaved L/R -> two planes) | 0.7 | 1.8 (5.3 %) |
| `sub_82B1ADB8` / `sub_82AA54E8` | Mixer voice loop (calls the native ramped mix `sub_82AA53C0`) | 0.9-1.3 | 1.6 (4.5 %) |
| `sub_82B21100`, `sub_82B218D0`, `sub_82B21C38`, `sub_82B21E78`, `sub_82B22150` / `sub_82AA13C0`, `sub_82AA1B90`, `sub_82AA1EF8`, `sub_82AA2138`, `sub_82AA2410` | The reverb's delay-line, comb and all-pass helpers (leaf loops of 42-424 instructions) | 0.3-0.6 each | 0.3-0.6 each |
| `sub_82B32FC8`, `sub_82B26F68`, `sub_82B29E10` / `sub_82AB5668`, `sub_82AADC28`, `sub_82AA7B20` | Voice setup / format paths | 0.3-0.8 each | about 1 each |
| `ff_wma_run_level_decode` (FFmpeg) | XMA (WMA Pro) bitstream decoding, host side | | |

The host side (the `?` bucket of host threads): `SwitchAudioSystem::FillAndAppend` 3.0, `SwitchAudioDriver::MixFrameInto` 1.8,
`GetStereoFold` 1.4 % of a core on average. These come from few samples (about 40 in the whole run); treat them as an upper bound.

## What is native now

All guest replacements are exact: the same operations in the same order with the same FPCR state as the recompiled code, checked
against it on random inputs on the host (macOS arm64, same ISA and FPCR semantics as the console), and checked again on the console
by the guard of the hot hooks. Each one has its own cvar, **default off**.

| Change | Cvar | Kind | Host speed-up | Verified by |
|---|---|---|---|---|
| Resampler mono `sub_82AAFB90` (RU `sub_82B2D4F0`), `app/src/native/hot/n_82AAFB90.h` | `masseffect_hot_audio_resampler` | hot hook (guarded) | x2.6 (538 -> 203 ns per call) | `tests/hot_fuzz` case_82AAFB90, EN and RU, 200000 iterations, 0 failures |
| Resampler stereo `sub_82AAFE20` (RU `sub_82B2D780`), `n_82AAFE20.h` (shares the mono code) | `masseffect_hot_audio_resampler_stereo` | hot hook (guarded) | x2.2 (758 -> 338 ns) | case_82AAFE20, EN and RU, 100000 iterations, 0 failures |
| CRT memset `sub_82AC4A50` (RU `sub_829730C0`), `n_82AC4A50.h` | `masseffect_hot_crt_memset` | hot hook (guarded) | x6.5 (127 -> 20 ns) | case_82AC4A50, EN and RU, 100000 iterations, 0 failures |
| `ChainPhase` of the native smoother: two channels per float64x2 lane pair, up to three pairs interleaved (`app/src/native/me_audio_dsp.h`) | part of `masseffect_audio_dsp_native` (mask bit 1) | inside an existing native | x3.4 (3.19 -> 0.95 us per 6-channel call; the recompiled original is 20.6 us) | `tests/audio_dsp/run.sh`, 30000 iterations, 1-8 channels, 0 failures; console: `masseffect_audio_dsp_native = 2` |
| Host output stage: 5.1 fold and accumulate in one NEON pass (`sequential_6_BE_fold_add_interleaved_2_LE`), NEON float -> s16 quantizer with the same peak / saturation counters (`mix_to_s16`), `sdk/include/rex/audio/conversion.h`, used by `sdk/src/audio/switch/switch_audio_system.cpp` | none (always on, same results) | host code | not measured (removes a 2 KB temporary and a second add loop per frame, and the per-sample lrint / clamp / isfinite loop) | `tests/cpu/test_audio_output.cpp` (4000 random frames incl. NaN / inf / denormals: fold outputs bit-equal, NaN-vs-NaN aside; PCM, peak and counters identical) |

### Console guard finding (2026-10-09) and its fix

First console run (RU, spacewalk, guard period 4096): `[hot] DIFFERENCE sub_82B2D780: memory 0x40063084 (+4 of range
0x40063080+1040) native=0x00 original=0xb7` right after start, inputs `r5 = 0x40062c80`, `r6 = 0x100`; mono and memset guard OK.
**Cause: the guard, not the native code.** For 256 outputs the stereo `Writes()` declared the left plane as
`[dst & ~15, +32 * blocks + 16)` and the right plane as `[(dst + 1024) & ~15, ...)`: the extra 16 bytes made the two ranges overlap.
`Check()` copied the native result of range 0, rolled range 0 back, and only then copied range 1: the overlapping bytes of range 1
were already the pre-call values (here 0, the buffer cleared by memset), and the comparison with the original reported them.
Fixes: (1) `Check()` copies every range before rolling any back (shared helpers `SaveRanges` / `RestoreRanges` /
`FirstRangeDiff` in `me_hot_common.h`; later the same day replaced by the private-copy guard, which merges overlapping ranges
and never rolls guest memory back: [hot-guard.md](hot-guard.md)), so overlapping declared ranges are safe for every hook; (2) the resampler declares the exact
extents (`32 * blocks` bytes per plane; one merged range when the planes overlap, more than 32 blocks). (3) The fuzzer now replays
the guard's algorithm on every iteration of every case with the same helpers (`tests/hot_fuzz/main.cpp`, "guard replay"); with the
old copy order it reproduces the console report on 78 % of the stereo inputs, with the fix 0 failures (EN and RU, 100000 iterations),
and the generators include the console inputs (256 outputs into an aligned buffer, `r9 = r5`, `r4 = 0x6500`, `r10 = 0x64FF`). The
native stereo resampler was never wrong; after the guard switched it off, the run continued on the original. Rerun the console
check with the new build.

### Resampler details

`r3` source, `r5` destination, `r6` output samples, `r7` voice state (`+0` source start, `+4` length, `+8` consumed (out), `+13` byte
divisor, `+20` destination start, `+24` capacity, `+28` written (out), `+36` gain (out: the target), `+40` gain target, `+44` rate,
`+48` position fraction (in and out)). Per block of 8 outputs: positions `pos + k * rate` (64-bit, 32.32), samples at
`src + 2 * (pos >> 32)` (stereo: `4 *`), `out = (s1 - s0) * (frac * g) + s0 * g` with `frac = (u32)pos >> 1 * 2^-31` and the
ramped gain `g` (two vectors of 4, `+ step * 8` per block). The full contract, the corner cases that are reproduced (the `lvlx`
halfword split at offset 15 of a 16-byte block for odd sources, `fctidz` saturation, the do-while with a count <= 0, the
`__floatundidf` helper's `r3` / `r5` / `f1`) and the declines are in the header comments. The English and Russian functions are
instruction-for-instruction the same except the five `.rdata` addresses, which the RU overlay copy of `n_82AAFB90.h` changes
(`editions/ru/overlay/app/src/native/hot/n_82AAFB90.h`; `n_82AAFE20.h` has no address of its own and needs no copy).

Fuzz notes (the generator is in `tests/hot_fuzz/cases/case_82AAFB90.inc`): two inputs may not both be NaNs with different payloads
(an IEEE add / mul returns the first operand's payload and the compiler may commute the recompiled code's operands; real audio
carries no NaN), and the simulated source reads are kept off the callee's own stack frame (the original spills there; a real
source buffer never aliases the callee's new frame). With these two rules there are 0 failures in 200000 iterations.

The Russian cases are in `editions/ru/tests/hot_fuzz/cases/` (RU original names, same generators). To run them, copy
`tests/hot_fuzz` and `app/src/native` into a scratch tree, apply `editions/ru/overlay/app/src/native` on top, replace the cases
with the RU ones and run `GEN=out/edition-ru/app/generated/default python3 tests/hot_fuzz/build.py <case> <RU name>` there
(the case file filter matches the file name, the run filter the registered name).

### Smoother `ChainPhase`

`y = (float)fma(a, y, x[j])` per channel and sample, kept as double between steps (as `fmadds` does). The recurrence is serial within
a channel, but channels are independent: pairs of channels run in the two lanes of a float64x2 (`FMLA`, `FCVTN`, `FCVTL`), up to three
pairs interleaved to hide the latency, an odd channel uses the old scalar loop. Vector and scalar FP round identically under the same
FPCR (FZ = 0 here), so the result is bit-identical. The function keeps its signature and section name (`app/function_order.ld`).

## Does the set of hooked functions change?

Yes. Run **`tools/edition.sh ru all`** and **`tools/edition.sh en all`** (codegen, then the NRO): `tools/direct_calls.py` treats
every 82xxxxxx address in the app sources as hooked, so the direct calls to the new hooks must be regenerated.

- English: new `sub_82AAFB90`, `sub_82AAFE20` (`sub_82AC4A50` was already in the hooked set through a comment in `n_82654030.h`).
- Russian: new `sub_82B2D4F0`, `sub_82B2D780`, `sub_829730C0`. The RU overlay headers also name the English addresses in comments and
  namespaces, as all overlay headers do (those RU functions only lose direct-call inlining).
- The `ChainPhase` and host output changes need no codegen (ordinary rebuild).

## What to check on the console

1. Build both editions as above. Production settings stay as they are (all new cvars default off; the host output stage and
   `ChainPhase` change are always on but exact).
2. Validate run (RU, audio-heavy route: Citadel spacewalk, a combat): `masseffect_audio_dsp_native = 2`,
   `masseffect_hot_audio_resampler = true`, `masseffect_hot_audio_resampler_stereo = true`, `masseffect_hot_crt_memset = true`,
   `masseffect_hot_guard_period = 1` (every call checked). Expect in the log `[hot] sub_82B2D4F0: guard OK ...`,
   `[hot] sub_82B2D780: guard OK ...`, `[hot] sub_829730C0: guard OK ...`, no `[hot] DIFFERENCE`, and in the profiler's system line
   `native audio DSP calls N, 0 mismatches`. Listen: no clicks, no changed reverb or pitch.
3. A/B (guard period back to the default 4096, `masseffect_audio_dsp_native = 1`), same route, 2+ runs each, against the same toml
   without the three new cvars: the audio thread's CPU % and its self top (`sub_82B2D4F0`, `sub_82B2D780`, `sub_829730C0`,
   `ChainPhase` should drop out of the top or shrink), the `audio:` line (`requests without data`, saturated samples, peak: unchanged
   behaviour), fps in the heavy windows.
4. memset is called from everywhere (1204 call sites): also look at the main and render threads' self time of `sub_829730C0` in a
   non-audio route, and at the cold start time.

### Expected gain

From the heaviest block (audio thread at ~34 % of a core): resampler mono 4.2 -> ~1.6, stereo 1.8 -> ~0.8, memset 2.1 -> ~0.3,
`ChainPhase` 3.8 -> ~1.1 % of a core, part of the `vld1q/vst1q` share with them: about **8-10 % of a core less** on the audio thread
in audio-heavy scenes (34 % -> ~25 %). On the tour average about 3-4 % of a core. The host speed-ups above are from the Apple
host (clang); the console (GCC -O3, Cortex-A57) usually shows similar or larger ratios for this kind of code, but this is not measured
yet. The output-stage change saves at most the 1-3 % the host functions showed.

## What is left

- **Reverb `sub_82AA2A00` (RU `sub_82B22740`)**, 3.9 % of a core on average, the largest single item. 1457 PPC instructions; the two
  per-sample loops (about 130 and 520 instructions per sample, 256 samples per call) are a feedback-delay network with many delay
  lines whose read / write indices and filter states live in the 310 KB object and are stored every sample. GCC already narrows the
  float math (`devkitA64 -O3` output: 3825 instructions, most FP ops in single precision, 190 `fcvt`); what costs is the PPC
  register pressure: about 40 loads of pointers and spilled values from the guest stack frame per sample (each with a byte swap) and
  the stores of the per-sample state. A native version would keep those in host registers (an estimated x2-x3 on the self time,
  ~2-2.5 % of a core). It needs a semantic rewrite of ~650 loop instructions with exact operation order, or a mechanical tool that
  promotes the frame spill slots (offsets below 400) of a generated function to locals and narrows its volatile registers with
  spills around the calls; the fuzz harness then needs a generator for the 310 KB state. Not started.
- The reverb's leaf helpers `sub_82AA13C0`, `sub_82AA1B90`, `sub_82AA1EF8`, `sub_82AA2138`, `sub_82AA2410` (0.3-0.6 % each): small
  scalar delay-line loops; `tests/hot_fuzz/autonative.py` can derive register-narrowed clones mechanically.
- `sub_82AA54E8` (mixer voice loop, 0.9-1.3 %), `sub_82AB5668` (leaf, 249 instructions).
- XMA decoding: FFmpeg's WMA Pro decoder already uses its NEON MDCT and float DSP (`sdk/thirdparty/CMakeLists.txt`);
  `ff_wma_run_level_decode` is bitstream / VLC work with no obvious waste on our side. The SDK's `XmaContext::ConvertFrame` is already
  NEON.

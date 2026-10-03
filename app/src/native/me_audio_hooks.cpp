// Mass Effect - host-runtime overhead switches that live in the app, and the native audio DSP hooks.
//
// 1) masseffect_vblank_sleep_exact: the native GPU "VSync" thread (me_native_system.cpp VblankLoop) slept 1 ms per
//    loop to deliver 60 interrupts a second: ~1000 wake-ups/s at priority 0x2C on cores 0-1. It now sleeps to the
//    next vblank.
// 2) masseffect_audio_dsp_native: NEON replacements of the XAudio mixer's hottest recompiled kernels
//    (me_audio_dsp.h; bit-exact against the recompiled code on the host, tests/audio_dsp).
//      0 = recompiled code (default), 1 = native, 2 = validate: runs the native code on a copy of the destination,
//      then the recompiled code, compares the results byte by byte and keeps the recompiled result (the game is
//      never affected); mismatches are counted in the profiler's "sistema" line and the first one is logged.
//    masseffect_audio_dsp_mask selects the functions (bit 0 = sub_82AA53C0, bit 1 = sub_82B4D580).
//
// The direct calls to sub_82AA53C0 inside sub_82AA54E8 are rewritten to call the hook by tools/direct_calls.py
// (any 82xxxxxx address in the sources counts as hooked; keep the addresses in this file).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/sys_counters.h>

#include "me_audio_dsp.h"

REXCVAR_DEFINE_BOOL(masseffect_vblank_sleep_exact, true, "Mass Effect",
                    "Native GPU vblank thread: sleep to the next vblank instead of waking every 1 ms (60 useful "
                    "wake-ups of ~1000 per second). false = the original 1 ms loop")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(masseffect_audio_dsp_native, 0, "Mass Effect",
                     "XAudio mixer DSP kernels in native NEON code (sub_82AA53C0 ramped mix, sub_82B4D580 "
                     "smoother): 0 = recompiled (default), 1 = native (bit-exact on the host test), 2 = validate "
                     "(native vs recompiled, byte compare; the recompiled result is kept)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_audio_dsp_mask, 3, "Mass Effect",
                     "Which audio DSP kernels masseffect_audio_dsp_native applies to: bit 0 = sub_82AA53C0, bit 1 = "
                     "sub_82B4D580")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82AA53C0);
REX_EXTERN(__imp__sub_82B4D580);

namespace {

#if ME_AUDIO_DSP_NEON

using me::audio_dsp::GP;

std::atomic<uint64_t> g_validated{0};
std::atomic<uint64_t> g_mismatches{0};
std::atomic<bool> g_first_logged{false};
std::atomic<int64_t> g_last_report_ms{0};

int64_t NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void MaybeReport(int mode) {
  const int64_t now = NowMs();
  int64_t last = g_last_report_ms.load(std::memory_order_relaxed);
  if (last == 0) {
    g_last_report_ms.compare_exchange_strong(last, now, std::memory_order_relaxed);
    return;
  }
  if (now - last < 10000 || !g_last_report_ms.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
    return;
  }
  REXLOG_INFO("[audio] native DSP (mode {}): {} calls validated, {} mismatches", mode,
              g_validated.exchange(0, std::memory_order_relaxed), g_mismatches.load(std::memory_order_relaxed));
}

// Two NaNs with different payloads count as equal (the compiler may commute the operands of an FP op).
bool SameRegion(const uint8_t* a, const uint8_t* b, size_t bytes, size_t* first_diff) {
  for (size_t o = 0; o + 4 <= bytes; o += 4) {
    if (std::memcmp(a + o, b + o, 4) == 0) continue;
    uint32_t x, y;
    std::memcpy(&x, a + o, 4);
    std::memcpy(&y, b + o, 4);
    x = __builtin_bswap32(x);
    y = __builtin_bswap32(y);
    const bool nan_x = (x & 0x7F800000) == 0x7F800000 && (x & 0x007FFFFF);
    const bool nan_y = (y & 0x7F800000) == 0x7F800000 && (y & 0x007FFFFF);
    if (nan_x && nan_y) continue;
    if (first_diff) *first_diff = o;
    return false;
  }
  for (size_t o = bytes & ~size_t(3); o < bytes; ++o) {
    if (a[o] != b[o]) {
      if (first_diff) *first_diff = o;
      return false;
    }
  }
  return true;
}

void Mismatch(const char* name, uint32_t address, size_t offset, const uint8_t* recompiled, const uint8_t* native) {
  g_mismatches.fetch_add(1, std::memory_order_relaxed);
  REX_SYS_COUNT(rex::syscount::kAudioMismatch, 1);
  if (!g_first_logged.exchange(true, std::memory_order_relaxed)) {
    uint32_t r = 0, n = 0;
    std::memcpy(&r, recompiled + (offset & ~size_t(3)), 4);
    std::memcpy(&n, native + (offset & ~size_t(3)), 4);
    REXLOG_WARN("[audio] native DSP mismatch in {}: region 0x{:08X}+{} recompiled {:08X} native {:08X}", name,
                address, offset, __builtin_bswap32(r), __builtin_bswap32(n));
  }
}

// Runs `native` and `recompiled` on the same input and keeps the recompiled result. `region` = [address, address+bytes).
template <typename Native, typename Recompiled>
void Validate(const char* name, PPCContext& ctx, uint8_t* base, uint32_t address, uint32_t bytes, Native native,
              Recompiled recompiled, int mode) {
  thread_local std::vector<uint8_t> before, from_native;
  uint8_t* p = GP(base, address);
  before.assign(p, p + bytes);
  PPCContext copy = ctx;
  native(copy, base);
  from_native.assign(p, p + bytes);
  std::memcpy(p, before.data(), bytes);
  recompiled(ctx, base);
  size_t diff = 0;
  g_validated.fetch_add(1, std::memory_order_relaxed);
  if (!SameRegion(p, from_native.data(), bytes, &diff)) {
    Mismatch(name, address, diff, p, from_native.data());
  }
  MaybeReport(mode);
}

#endif  // ME_AUDIO_DSP_NEON

}  // namespace

// ---- sub_82AA53C0: ramped gain mix (leaf) ---------------------------------------------------------------
REX_HOOK_RAW(sub_82AA53C0) {
#if ME_AUDIO_DSP_NEON
  static const int mode = REXCVAR_GET(masseffect_audio_dsp_native);
  static const bool enabled = (REXCVAR_GET(masseffect_audio_dsp_mask) & 1) != 0;
  if (mode != 0 && enabled) {
    REX_SYS_COUNT(rex::syscount::kAudioNative, 1);
    if (mode == 2) {
      const uint32_t vectors = ctx.r7.u32 >> 2;
      if (vectors) {
        const uint32_t bytes = (((vectors - 1) >> 2) + 1) * 64 + 16;
        const uint32_t dst = ctx.r3.u32 & ~0xFu;
        Validate("sub_82AA53C0", ctx, base, dst, bytes, me::audio_dsp::Native82AA53C0, __imp__sub_82AA53C0, mode);
        return;
      }
    } else {
      me::audio_dsp::Native82AA53C0(ctx, base);
      return;
    }
  }
#endif
  __imp__sub_82AA53C0(ctx, base);
}

// ---- sub_82B4D580: smoother + clamp over the mixer's channel buffers -------------------------------------
REX_HOOK_RAW(sub_82B4D580) {
#if ME_AUDIO_DSP_NEON
  static const int mode = REXCVAR_GET(masseffect_audio_dsp_native);
  static const bool enabled = (REXCVAR_GET(masseffect_audio_dsp_mask) & 2) != 0;
  if (mode != 0 && enabled) {
    REX_SYS_COUNT(rex::syscount::kAudioNative, 1);
    if (mode == 2) {
      // The regions it can change: the channel buffer and the per-channel states in the object. Asks the
      // frame query (sub_82AA65E8) like the function does, on a scratch frame below the stack pointer.
      const uint32_t self = ctx.r3.u32;
      PPCContext probe = ctx;
      const uint32_t frame = ctx.r1.u32 - 224 - 64;
      probe.r1.u32 = frame;
      probe.r3.u64 = ctx.r4.u64;
      probe.r4.s64 = int64_t(frame) + 80;
      __imp__sub_82AA65E8(probe, base);
      const uint32_t channels = GP(base, frame + 81)[0];
      uint32_t buffer;
      std::memcpy(&buffer, GP(base, frame + 88), 4);
      buffer = __builtin_bswap32(buffer);
      const uint32_t buffer_bytes = (channels ? channels : 1) * 1024;
      // Validate one region at a time: the object states first (small), then the buffer.
      thread_local std::vector<uint8_t> before_buf, native_buf, before_state, native_state;
      uint8_t* pb = GP(base, buffer);
      uint8_t* ps = GP(base, self + 20);
      const uint32_t state_bytes = 4 * (channels ? channels : 1);
      before_buf.assign(pb, pb + buffer_bytes);
      before_state.assign(ps, ps + state_bytes);
      PPCContext copy = ctx;
      me::audio_dsp::Native82B4D580(copy, base);
      native_buf.assign(pb, pb + buffer_bytes);
      native_state.assign(ps, ps + state_bytes);
      std::memcpy(pb, before_buf.data(), buffer_bytes);
      std::memcpy(ps, before_state.data(), state_bytes);
      __imp__sub_82B4D580(ctx, base);
      size_t diff = 0;
      g_validated.fetch_add(1, std::memory_order_relaxed);
      if (!SameRegion(pb, native_buf.data(), buffer_bytes, &diff)) {
        Mismatch("sub_82B4D580 buffer", buffer, diff, pb, native_buf.data());
      } else if (!SameRegion(ps, native_state.data(), state_bytes, &diff)) {
        Mismatch("sub_82B4D580 state", self + 20, diff, ps, native_state.data());
      }
      MaybeReport(mode);
      return;
    }
    me::audio_dsp::Native82B4D580(ctx, base);
    return;
  }
#endif
  __imp__sub_82B4D580(ctx, base);
}

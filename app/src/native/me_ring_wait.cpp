// Mass Effect - the game's D3D wait for the GPU without spinning (native renderer).
//
// WHY (Switch stack profile)
//   sub_8222FA98 is the D3D check "has the GPU read pointer moved since the last iteration":
//   32 db16cyc delays, then it compares the word the GPU writes the
//   read pointer back to ([[device+10768]]) with the value seen last time ([r3+8]), and after 5000 ticks
//   without change declares the GPU hung. Its callers loop on it. On the console it was ~22 % of the busy
//   game thread (plus ~6 % in the caller), on a 3-core machine where the ring thread needs that core.
//
// WHAT IT DOES
//   If the read pointer is unchanged, it waits at most masseffect_wait_ring_max_us for the ring thread
//   to write it back again (NotifyRingProgress), then calls the original, which decides as before. The game
//   sees the same values, only later. With bit 0x04 of [device+10813] (GPU already declared hung) or
//   without the native renderer, it does not wait.

#include "me_native_system.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

REXCVAR_DEFINE_BOOL(masseffect_wait_blocking_ring, false, "Mass Effect",
                    "Native renderer: the game's D3D sleeps while waiting for the ring thread to advance "
                    "instead of spinning in sub_8222FA98")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_notify_no_lock, false, "Mass Effect",
                    "Ring thread: NotifyRingProgress takes the progress mutex only when the game's D3D wait is "
                    "registered as sleeping (sequentially consistent handshake on both sides) instead of on every call "
                    "(one per fence write and per ring segment). Same wake-ups")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_wait_ring_max_us, 2000, "Mass Effect",
                     "Native renderer: longest sleep per iteration of the game's D3D GPU wait, in microseconds")
    .range(100, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace me::native {
namespace {

std::mutex g_progress_mutex;
std::condition_variable g_progress_cv;
std::atomic<uint32_t> g_progress{0};
std::atomic<int> g_waiting{0};

}  // namespace

// The ring thread bumps the counter after writing the read pointer back, and checks for waiters under the
// lock (an unlocked Dekker handshake lost wake-ups on ARM).
void NotifyRingProgress() {
  static const bool no_lock = REXCVAR_GET(masseffect_notify_no_lock);
  if (no_lock) {
    // Dekker handshake, sequentially consistent on both sides: the waiter registers in g_waiting and then checks
    // g_progress (hook below); this side bumps g_progress and then reads g_waiting. One of the two sees the other's
    // write. If a waiter is registered, the lock/unlock below waits for it to be inside the condition variable (it holds
    // the mutex from registering until wait_for releases it), so the notify cannot be lost.
    g_progress.fetch_add(1, std::memory_order_seq_cst);
    if (g_waiting.load(std::memory_order_seq_cst) <= 0) return;
    { std::lock_guard<std::mutex> lock(g_progress_mutex); }
    g_progress_cv.notify_all();
    return;
  }
  g_progress.fetch_add(1, std::memory_order_acq_rel);
  bool notify;
  {
    std::lock_guard<std::mutex> lock(g_progress_mutex);
    notify = g_waiting.load(std::memory_order_acquire) > 0;
  }
  if (notify) g_progress_cv.notify_all();
}

}  // namespace me::native

namespace {

constexpr uint32_t kOffReadPointerWord = 10768;  // device: pointer to the word the GPU writes RPTR to
constexpr uint32_t kOffState = 10813;            // device: bit 0x04 = GPU considered hung

// Guest addresses map 1:1 onto base on the Switch (no 0xE0000000 physical offset).
uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v = 0;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

uint8_t Load8(const uint8_t* base, uint32_t address) {
  return base[address];
}

std::atomic<uint64_t> g_iterations{0}, g_sleeps{0}, g_advanced{0}, g_ns_asleep{0};
std::atomic<int64_t> g_next_report_ms{0};

void Report() {
  using namespace std::chrono;
  const int64_t now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  int64_t next = g_next_report_ms.load(std::memory_order_relaxed);
  if (now < next) return;
  if (!g_next_report_ms.compare_exchange_strong(next, now + 10000)) return;
  if (!next) return;  // first call only arms the timer
  const uint64_t sleeps = g_sleeps.exchange(0), advanced = g_advanced.exchange(0);
  REXLOG_INFO("[ring_wait] last 10 s: {} D3D iterations, {} sleeps ({} ended by ring progress), {:.1f} ms asleep",
              g_iterations.exchange(0), sleeps, advanced, double(g_ns_asleep.exchange(0)) / 1e6);
}

}  // namespace

REX_EXTERN(__imp__sub_8222FA98);
REX_HOOK_RAW(sub_8222FA98) {
  static const bool active = me::native::Enabled() && REXCVAR_GET(masseffect_wait_blocking_ring);
  if (active) {
    const uint32_t wait = ctx.r3.u32;
    const uint32_t device = wait ? Load32(base, wait) : 0;
    if (device && !(Load8(base, device + kOffState) & 0x04)) {
      const uint32_t word = Load32(base, device + kOffReadPointerWord);
      if (word) {
        g_iterations.fetch_add(1, std::memory_order_relaxed);
        const uint32_t seen = me::native::g_progress.load(std::memory_order_acquire);
        if (Load32(base, word) == Load32(base, wait + 8)) {
          const auto before = std::chrono::steady_clock::now();
          g_sleeps.fetch_add(1, std::memory_order_relaxed);
          bool advanced;
          {
            std::unique_lock<std::mutex> lock(me::native::g_progress_mutex);
            me::native::g_waiting.fetch_add(1, std::memory_order_seq_cst);
            advanced = me::native::g_progress_cv.wait_for(
                lock, std::chrono::microseconds(REXCVAR_GET(masseffect_wait_ring_max_us)),
                [seen] { return me::native::g_progress.load(std::memory_order_seq_cst) != seen; });
            me::native::g_waiting.fetch_sub(1, std::memory_order_acq_rel);
          }
          if (advanced) g_advanced.fetch_add(1, std::memory_order_relaxed);
          g_ns_asleep.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - before).count()),
                                std::memory_order_relaxed);
          Report();
        }
      }
    }
  }
  __imp__sub_8222FA98(ctx, base);
}

// Mass Effect - the D3D Swap throttle (docs/edram-shader-imul.md, part 2).
// sub_8222C768(device, fence, reason) waits until the fence word the ring writes back ([[device+10768]], updated by
// the native ring parser at parse time, C25) has reached `fence`. Its kick counter is [device+10780] (+2 per kick).
// Reason 3 comes only from Swap (82234000 at 0x82234684, and the segment function 8222BD48 called from it): it waits
// for [device+14564], the fence of the PREVIOUS Swap, then stores this Swap's fence there. So D3D already allows one
// frame in flight; the wait is the render thread waiting for the ring thread to finish parsing frame N-1.
//   masseffect_swap_wait_stats: per Swap wait, the time spent and the ring lag at entry (kicks not yet written back),
//     logged every 10 s.
//   masseffect_swap_frames_in_flight = 2: the Swap waits for the fence of Swap N-2 instead of N-1. Ring and segment
//     memory stay protected by their own waits (reasons 1 and 2: read pointer and segment write-backs) and every
//     resource keeps its own fence (reasons 4-15), so the guest can never overwrite unread ring data; the guest's
//     command order and the game/render thread fences (FRenderCommandFence, masseffect_frame_lag) are untouched. Only
//     guest data that is reused per frame WITHOUT a fence, relying on Swap's implicit "frame N-1 is done", could be
//     overwritten one frame early: default off, check the image on the console.
REXCVAR_DEFINE_BOOL(masseffect_swap_wait_stats, false, "Mass Effect",
                    "Diagnostics: time the D3D Swap throttle (sub_8222C768 with reason 3) and the ring lag at entry, "
                    "logged every 10 s")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_swap_frames_in_flight, 1, "Mass Effect",
                     "Native renderer: frames the ring may be behind when the guest D3D Swap returns. 1 = the game's own "
                     "(wait for the previous Swap's fence); 2 = wait for the fence of the Swap before it. Experimental")
    .range(1, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
constexpr uint32_t kOffKickCounter = 10780;  // device: fence value of the next kick (+2 per kick)
constexpr uint32_t kOffSwapCounter = 16184;  // device: +1 at the start of every Swap
constexpr uint32_t kReasonSwap = 3;

struct SwapWaitStats {
  uint64_t calls = 0, waited = 0, ns = 0, max_ns = 0, kicks_behind = 0, max_kicks_behind = 0, relaxed = 0;
  std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
SwapWaitStats g_swap_stats;  // render thread only (the only caller of reason 3)

void RecordSwapWait(uint64_t ns, uint32_t kicks_behind, bool must_wait, bool relaxed) {
  SwapWaitStats& s = g_swap_stats;
  ++s.calls;
  if (must_wait) ++s.waited;
  if (relaxed) ++s.relaxed;
  s.ns += ns;
  if (ns > s.max_ns) s.max_ns = ns;
  s.kicks_behind += kicks_behind;
  if (kicks_behind > s.max_kicks_behind) s.max_kicks_behind = kicks_behind;
  const auto now = std::chrono::steady_clock::now();
  if (now - s.last < std::chrono::seconds(10)) return;
  REXLOG_INFO("[swap_wait] last 10 s: {} Swap waits, {} had to wait, {} relaxed to N-2, {:.1f} ms total "
              "({:.2f} ms/Swap, max {:.2f} ms), ring lag at entry {:.1f} kicks avg, {} max",
              s.calls, s.waited, s.relaxed, s.ns / 1.0e6, s.calls ? s.ns / 1.0e6 / double(s.calls) : 0.0,
              s.max_ns / 1.0e6, s.calls ? double(s.kicks_behind) / double(s.calls) : 0.0, s.max_kicks_behind);
  s = SwapWaitStats{};
  s.last = now;
}
}  // namespace

#ifndef MASSEFFECT_D3D_TRACE_ALL  // that build wraps sub_8222C768 itself (me_d3d_trace_all.inc)
REX_EXTERN(__imp__sub_8222C768);
REX_HOOK_RAW(sub_8222C768) {
  static const bool stats = REXCVAR_GET(masseffect_swap_wait_stats);
  static const bool relax = me::native::Enabled() && REXCVAR_GET(masseffect_swap_frames_in_flight) > 1;
  if (ctx.r5.u32 != kReasonSwap || (!stats && !relax)) {
    __imp__sub_8222C768(ctx, base);
    return;
  }
  const uint32_t device = ctx.r3.u32;
  const uint32_t word = Load32(base, device + kOffReadPointerWord);
  const uint32_t counter = Load32(base, device + kOffKickCounter);
  const uint32_t done = word ? Load32(base, word) : counter;
  uint32_t target = ctx.r4.u32;
  bool relaxed = false;
  if (relax) {
    // The target of the previous Swap's wait = the fence of Swap N-2, used only if that wait was in the Swap right
    // before this one (a Swap whose [device+14564] was reset skips the wait). Same unsigned distance test as the
    // guest loop: older = further from the counter; never newer than the guest's own target, so the wait can only
    // end earlier, never later.
    static uint32_t last_target = 0, last_swap = 0, older_target = 0, older_swap = 0;
    const uint32_t swap = Load32(base, device + kOffSwapCounter);
    if (target != last_target) {
      older_target = last_target;
      older_swap = last_swap;
      last_target = target;
      last_swap = swap;
    }
    if (older_target && swap - older_swap == 1u && counter - older_target > counter - target &&
        !(Load8(base, device + kOffState) & 0x04)) {
      target = older_target;
      ctx.r4.u64 = target;
      relaxed = true;
    }
  }
  const bool must_wait = counter - target < counter - done;
  const auto t0 = std::chrono::steady_clock::now();
  __imp__sub_8222C768(ctx, base);
  if (stats) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0);
    RecordSwapWait(uint64_t(ns.count()), (counter - done) / 2u, must_wait, relaxed);
  }
}
#endif

// Mass Effect - the game thread's wait for the render thread (UE3 render command fence) without spinning.
// sub_822FE760(counter, n) is `while (*counter > n) appSleep(0);` (sub_82811750 with 0 = a yield): ~27 % of
// the game thread in the Switch profile, on a 3-core machine where the ring and render threads need that
// core. Native: the same condition, a few yields first, then short real sleeps (masseffect_wait_game_us).
REXCVAR_DEFINE_INT32(masseffect_wait_game_us, 100, "Mass Effect",
                     "Game thread waiting for the render thread (sub_822FE760): sleep per check after a few yields, "
                     "in microseconds; 0 = the game's own yield loop")
    .range(0, 2000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(masseffect_frame_lag, 1, "Mass Effect",
                     "Frames the UE3 render thread may lag behind the game thread: the game's per-frame render "
                     "fence wait (the only FRenderCommandFence::Wait with n = 1) waits for n = this value instead. "
                     "1 = the game's own; 2 crashed the game at start on the Switch (2026-10-07), do not use")
    .range(1, 3)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(masseffect_fence_stats, false, "Mass Effect",
                    "Diagnostics: count the game thread's render fence waits (sub_822FE760) per fence and argument, with "
                    "the time spent waiting, and log the table every 10 s")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
// Waits are grouped by the fence object (r3) and n (r4): the generated code does not keep the caller's address.
// n = 1 lets the render thread lag one frame; n = 0 is a full flush.
struct FenceEntry {
  uint32_t fence = 0, n = 0;
  uint64_t calls = 0, waited = 0, ns = 0, max_ns = 0;
};
std::mutex g_fence_mutex;
FenceEntry g_fence_entries[24];
std::chrono::steady_clock::time_point g_fence_last_report = std::chrono::steady_clock::now();

void RecordFenceWait(uint32_t fence, uint32_t n, bool must_wait, uint64_t ns) {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(g_fence_mutex);
  for (FenceEntry& e : g_fence_entries) {
    if (e.calls && (e.fence != fence || e.n != n)) continue;
    if (!e.calls) { e.fence = fence; e.n = n; }
    ++e.calls;
    if (must_wait) ++e.waited;
    e.ns += ns;
    if (ns > e.max_ns) e.max_ns = ns;
    break;
  }
  if (now - g_fence_last_report < std::chrono::seconds(10)) return;
  g_fence_last_report = now;
  std::string text;
  for (FenceEntry& e : g_fence_entries) {
    if (!e.calls) continue;
    text += fmt::format(" [fence {:08X} n={}: {} calls, {} waited, {:.1f} ms total, max {:.2f} ms]", e.fence, e.n,
                        e.calls, e.waited, e.ns / 1.0e6, e.max_ns / 1.0e6);
    e = FenceEntry{};
  }
  REXLOG_INFO("[native] render fence waits, 10 s:{}", text);
}
}  // namespace

REX_EXTERN(__imp__sub_822FE760);
REX_HOOK_RAW(sub_822FE760) {
  static const int32_t us = REXCVAR_GET(masseffect_wait_game_us);
  // Read on every call: the first waits run before the configuration is loaded.
  const uint32_t lag = uint32_t(REXCVAR_GET(masseffect_frame_lag));
  const uint32_t counter = ctx.r3.u32;
  uint32_t n = ctx.r4.u32;
  if (n == 1 && lag > 1) {
    n = lag;
    ctx.r4.u64 = n;
  }
  // Read on every call: the first waits run before the configuration is loaded.
  const bool stats = REXCVAR_GET(masseffect_fence_stats);
  const bool must_wait = stats && Load32(base, counter) > n;
  const auto t0 = stats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  if (us <= 0) {
    __imp__sub_822FE760(ctx, base);
  } else {
    for (uint32_t i = 0; Load32(base, counter) > n; ++i) {
      if (i < 4) std::this_thread::yield();
      else std::this_thread::sleep_for(std::chrono::microseconds(us));
    }
  }
  if (stats) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0);
    RecordFenceWait(counter, n, must_wait, uint64_t(ns.count()));
  }
}

// Mass Effect - the render thread's occlusion-query poll without 100 us sleeps.
// sub_826E7C98 loops `while (GetData(query) == S_FALSE) Sleep(0);` (up to 100000 times); Sleep(0) is
// sub_82811750, which XThread::Delay turns into a 100 us sleep: ~9 % of the render thread in the Switch profile. The
// query result only changes when the ring thread has parsed past the query, so here (only for that call
// site) the thread waits for ring progress instead, at most masseffect_wait_occlusion_us.
REXCVAR_DEFINE_INT32(masseffect_wait_occlusion_us, 0, "Mass Effect",
                     "Occlusion-query poll (sub_826E7C98): wait for ring progress up to this many microseconds "
                     "instead of Sleep(0); 0 = the game's own Sleep(0)")
    .range(0, 5000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_diag_occlusion_poll, false, "Mass Effect",
                    "Diagnostic (docs/image-defects-feros.md 3.8): count the Sleep(0) iterations of the render thread's "
                    "occlusion-query poll (a query whose result is not written yet). A burst of more than 5000 in "
                    "100 ms is logged (the poll gives up after 100000 and the game then treats the query as not "
                    "answered); a total every 10 s. No behavior change");

namespace {
// masseffect_diag_occlusion_poll state (render thread only).
struct OcclusionPollDiag {
  uint64_t window_calls = 0, total_calls = 0, bursts = 0;
  std::chrono::steady_clock::time_point window{}, report{};
};
OcclusionPollDiag g_occlusion_poll;
void NoteOcclusionPoll() {
  auto& d = g_occlusion_poll;
  const auto now = std::chrono::steady_clock::now();
  if (d.window == std::chrono::steady_clock::time_point{}) d.window = d.report = now;
  ++d.window_calls;
  ++d.total_calls;
  if (now - d.window >= std::chrono::milliseconds(100)) {
    if (d.window_calls > 5000) {
      ++d.bursts;
      REXLOG_WARN("[ring_wait] occlusion poll burst: {} iterations in {:.1f} ms (a query result the ring has not "
                  "written; the game gives up after 100000)",
                  d.window_calls, std::chrono::duration<double, std::milli>(now - d.window).count());
    }
    d.window = now;
    d.window_calls = 0;
  }
  if (now - d.report >= std::chrono::seconds(10)) {
    REXLOG_INFO("[ring_wait] occlusion poll (10 s): {} iterations, {} bursts over 5000 per 100 ms", d.total_calls,
                d.bursts);
    d.report = now;
    d.total_calls = d.bursts = 0;
  }
}
}  // namespace

REX_EXTERN(__imp__sub_82811750);
REX_HOOK_RAW(sub_82811750) {
  constexpr uint32_t kOcclusionPollReturn = 0x826E7CE8;  // bl 0x82811750 in sub_826E7C98
  static const int32_t us = me::native::Enabled() ? REXCVAR_GET(masseffect_wait_occlusion_us) : 0;
  static const bool poll_diag = REXCVAR_GET(masseffect_diag_occlusion_poll);
  if (poll_diag && ctx.lr == kOcclusionPollReturn) NoteOcclusionPoll();
  if (us > 0 && ctx.lr == kOcclusionPollReturn) {
    const uint32_t seen = me::native::g_progress.load(std::memory_order_acquire);
    std::unique_lock<std::mutex> lock(me::native::g_progress_mutex);
    me::native::g_waiting.fetch_add(1, std::memory_order_seq_cst);  // seq_cst: pairs with the lock-free notifier
    me::native::g_progress_cv.wait_for(lock, std::chrono::microseconds(us), [seen] {
      return me::native::g_progress.load(std::memory_order_seq_cst) != seen;
    });
    me::native::g_waiting.fetch_sub(1, std::memory_order_acq_rel);
    ctx.r3.u64 = 0;  // SleepEx(0) result
    return;
  }
  __imp__sub_82811750(ctx, base);
}

// Guest interrupt-disable brackets (atomic RMW sequences) without the host global lock; see
// tools/pch_no_global_lock.py. Read in MassEffectApp::OnPreSetup, before any guest code runs.
REXCVAR_DEFINE_BOOL(masseffect_lockfree_atomics, false, "Mass Effect",
                    "Guest atomic sequences (mfmsr/mtmsrd brackets around lwarx/stwcx.) skip the host global lock; "
                    "stwcx. is already a compare-and-swap")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
extern "C" bool g_me_lockfree_atomics = false;

// Mass Effect - the game's present threshold for late frames (docs/frame-handshake.md, P1).
// UE3 (bLockToVsync) presents with D3DRS_PRESENTIMMEDIATETHRESHOLD = 10: a frame that misses its VBlank by more than
// 10 % of a 60 Hz scan waits for the next VBlank (a 35 ms frame is shown at 50 ms). The swap callback sub_82233940 gets
// (interval << 8) | threshold in r3 and releases a late frame at once when the scan position is within the threshold.
// 100 releases every late frame at once; on-time frames still wait for their VBlank (the 30 fps cap stays). Our output is
// presented immediately (no tearing). -1 = the game's value.
REXCVAR_DEFINE_INT32(masseffect_present_immediate_threshold, -1, "Mass Effect",
                     "Present threshold for late frames (0..100, % of a 60 Hz scan): 100 = a late frame is shown at "
                     "once instead of at the next VBlank; -1 = the game's (10)")
    .range(-1, 100);

REX_EXTERN(__imp__sub_82233940);
REX_HOOK_RAW(sub_82233940) {
  // Read on every call: the first presents can run before the configuration is loaded.
  const int32_t threshold = REXCVAR_GET(masseffect_present_immediate_threshold);
  if (threshold >= 0) {
    static uint32_t logged = 0;
    if (logged < 4) {
      ++logged;
      REXLOG_INFO("[present] swap callback param {:08X} -> threshold {}", ctx.r3.u32, threshold);
    }
    ctx.r3.u64 = (ctx.r3.u32 & ~0xFFu) | uint32_t(threshold);
  }
  __imp__sub_82233940(ctx, base);
}

// Mass Effect - D3D's occlusion GetData answered at once in query mode 0 (docs/image-defects-feros.md 3.8.2).
// sub_82229158(query, out, size, flags) is D3D's GetData. Occlusion branch (type 9 at [query+4]): S_FALSE while the
// ring has not written back the fence of the query's END ([query+20] against [[device+10768]], with a kick if the
// fence is the current segment) or while end words 0..3 of every tile still hold the sentinel; otherwise S_OK with
// end ZPass - begin ZPass. Its only caller is UE3's blocking poll sub_826E7C98 (the 8 reads of sub_82392F28: light and
// primitive visibility; a 0 culls). In mode 0 the ring answers 1000 for every query, so the answer is known when the
// game asks: give it now (S_OK, 1000) without waiting for the ring and without reading guest memory, which also
// makes the game immune to an end structure the ring zeroed (an END taken for a BEGIN, see
// masseffect_native_query_pair_by_address). The ring still writes 1000 at parse time: the same value, never read.
// Other modes, other query types, a GPU declared hung and the D3D-trace build call the original.
REXCVAR_DEFINE_BOOL(masseffect_query_getdata_visible, true, "Mass Effect",
                    "Native renderer, occlusion query mode 0 only: D3D's GetData for an occlusion query answers "
                    "'finished, 1000 samples' at once instead of waiting for the ring to parse the query's END "
                    "(mode 0 writes 1000 there anyway). Counts the calls that would have answered 'not ready' or "
                    "another count, logged every 10 s")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(int32_t, masseffect_native_query_mode);  // me_native_system.cpp

namespace {
constexpr uint32_t kQueryTypeOcclusion = 9;
constexpr uint32_t kQueryVisibleSamples = 1000;  // what mode 0 writes at the END packet
constexpr uint32_t kSentinel = 0xFFFFFEEDu;      // -275, stored at Issue(BEGIN)

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

uint32_t Load32LE(const uint8_t* base, uint32_t address) {
  uint32_t v = 0;
  std::memcpy(&v, base + address, sizeof(v));
  return v;  // lwbrx: the count words are little endian
}

// What the original would answer, read without side effects (no kick): returns false for S_FALSE, else the count.
bool OriginalOcclusionAnswer(const uint8_t* base, uint32_t query, uint32_t device, uint32_t& count) {
  const uint32_t fence = Load32(base, query + 20);
  if (fence) {
    const uint32_t word = Load32(base, device + kOffReadPointerWord);
    const uint32_t counter = Load32(base, device + kOffKickCounter);
    const uint32_t done = word ? Load32(base, word) : counter;
    if (counter - fence < counter - done) return false;  // the ring has not written the END's fence back yet
  }
  count = 0;
  const uint32_t tiles = Load32(base, query + 144);
  for (uint32_t i = 0; i < tiles && i < 16; ++i) {
    const uint32_t e = Load32(base, query + 24 + 4 * i);
    // The address D3D reads it through (uncached physical view), exactly as the guest code computes it.
    const uint32_t a = (e & 0x1FFFFFFFu) + ((((e >> 20) & 0xFFFu) + 512u) & 0x1000u) + 0xC0000000u;
    if (Load32(base, a) == kSentinel && Load32(base, a + 4) == kSentinel && Load32(base, a + 8) == kSentinel &&
        Load32(base, a + 12) == kSentinel)
      return false;  // END not parsed by the ring yet
    count += Load32LE(base, a + 16) + Load32LE(base, a + 20) - Load32LE(base, a + 48) - Load32LE(base, a + 52);
  }
  return true;
}

struct GetDataStats {
  std::atomic<uint64_t> answered{0}, not_ready{0}, other{0}, zero{0};
  std::atomic<int64_t> next_ms{0};
};
GetDataStats g_getdata;

void ReportGetData() {
  using namespace std::chrono;
  const int64_t now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  int64_t next = g_getdata.next_ms.load(std::memory_order_relaxed);
  if (now < next) return;
  if (!g_getdata.next_ms.compare_exchange_strong(next, now + 10000)) return;
  if (!next) return;  // the first call only arms the timer
  REXLOG_INFO("[ring_wait] occlusion GetData (10 s, mode 0): {} answered 1000 at once; the original would have "
              "answered {} 'not ready' (ring behind), {} with another count, {} of them 0 (culled)",
              g_getdata.answered.exchange(0), g_getdata.not_ready.exchange(0), g_getdata.other.exchange(0),
              g_getdata.zero.exchange(0));
}
}  // namespace

#ifndef MASSEFFECT_D3D_TRACE_ALL  // that build wraps sub_82229158 itself (me_d3d_trace_all.inc)
REX_EXTERN(__imp__sub_82229158);
REX_HOOK_RAW(sub_82229158) {
  static const bool active = me::native::Enabled() && REXCVAR_GET(masseffect_query_getdata_visible) &&
                             REXCVAR_GET(masseffect_native_query_mode) == 0;
  if (active) {
    const uint32_t query = ctx.r3.u32, out = ctx.r4.u32;
    const uint32_t device = query ? Load32(base, query) : 0;
    if (device && out && Load32(base, query + 4) == kQueryTypeOcclusion && !(Load8(base, device + kOffState) & 0x04)) {
      uint32_t count = 0;
      if (!OriginalOcclusionAnswer(base, query, device, count)) {
        g_getdata.not_ready.fetch_add(1, std::memory_order_relaxed);
      } else if (count != kQueryVisibleSamples) {
        g_getdata.other.fetch_add(1, std::memory_order_relaxed);
        if (!count) g_getdata.zero.fetch_add(1, std::memory_order_relaxed);
      }
      g_getdata.answered.fetch_add(1, std::memory_order_relaxed);
      Store32(base, out, kQueryVisibleSamples);
      ctx.r3.u64 = 0;  // S_OK
      ReportGetData();
      return;
    }
  }
  __imp__sub_82229158(ctx, base);
}
#endif

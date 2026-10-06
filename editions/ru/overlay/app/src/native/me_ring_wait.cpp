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

// Mass Effect - the game thread's wait for the render thread (UE3 render command fence) without spinning.
// sub_822FE560(counter, n) is `while (*counter > n) appSleep(0);` (sub_82811F80 with 0 = a yield): ~27 % of
// the game thread in the Switch profile, on a 3-core machine where the ring and render threads need that
// core. Native: the same condition, a few yields first, then short real sleeps (masseffect_wait_game_us).
REXCVAR_DEFINE_INT32(masseffect_wait_game_us, 100, "Mass Effect",
                     "Game thread waiting for the render thread (sub_822FE560): sleep per check after a few yields, "
                     "in microseconds; 0 = the game's own yield loop")
    .range(0, 2000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_822FE560);
REX_HOOK_RAW(sub_822FE560) {
  static const int32_t us = REXCVAR_GET(masseffect_wait_game_us);
  if (us <= 0) {
    __imp__sub_822FE560(ctx, base);
    return;
  }
  const uint32_t counter = ctx.r3.u32, n = ctx.r4.u32;
  for (uint32_t i = 0; Load32(base, counter) > n; ++i) {
    if (i < 4) std::this_thread::yield();
    else std::this_thread::sleep_for(std::chrono::microseconds(us));
  }
}

// Mass Effect - the render thread's occlusion-query poll without 100 us sleeps.
// sub_826E7C98 loops `while (GetData(query) == S_FALSE) Sleep(0);` (up to 100000 times); Sleep(0) is
// sub_82811F80, which XThread::Delay turns into a 100 us sleep: ~9 % of the render thread in the Switch profile. The
// query result only changes when the ring thread has parsed past the query, so here (only for that call
// site) the thread waits for ring progress instead, at most masseffect_wait_occlusion_us.
REXCVAR_DEFINE_INT32(masseffect_wait_occlusion_us, 0, "Mass Effect",
                     "Occlusion-query poll (sub_826E7C98): wait for ring progress up to this many microseconds "
                     "instead of Sleep(0); 0 = the game's own Sleep(0)")
    .range(0, 5000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82811F80);
REX_HOOK_RAW(sub_82811F80) {
  constexpr uint32_t kOcclusionPollReturn = 0x826E84E0;  // bl 0x82811750 in sub_826E7C98
  static const int32_t us = me::native::Enabled() ? REXCVAR_GET(masseffect_wait_occlusion_us) : 0;
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
  __imp__sub_82811F80(ctx, base);
}

// Guest interrupt-disable brackets (atomic RMW sequences) without the host global lock; see
// tools/pch_no_global_lock.py. Read in MassEffectApp::OnPreSetup, before any guest code runs.
REXCVAR_DEFINE_BOOL(masseffect_lockfree_atomics, false, "Mass Effect",
                    "Guest atomic sequences (mfmsr/mtmsrd brackets around lwarx/stwcx.) skip the host global lock; "
                    "stwcx. is already a compare-and-swap")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
extern "C" bool g_me_lockfree_atomics = false;

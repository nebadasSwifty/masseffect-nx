// Hooks on the game's Direct3D entry points (XDK Direct3D at 0x8221F000-0x8223FFFF, called by Unreal Engine 3's
// RHI). Each generated sub_X is a weak symbol over __imp__sub_X, so a strong definition here wraps it: the
// original always runs. The wrappers
//   - hand the four draw functions to the native renderer (NoteDrawCall),
//   - give the shader dump the Direct3D-owned shader containers (MeShaderDumpContainer),
//   - apply the internal-resolution changes to device creation and the back/front buffers (MeResolutionBefore),
//   - optionally time the entry points (masseffect_d3d_stopwatch).
// -DMASSEFFECT_D3D_TRACE_ALL wraps every Direct3D function listed in me_d3d_trace_all.inc instead of the nine
// default ones (for the timer).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include <switch.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "native/me_native_system.h"

extern "C" void MeShaderDumpContainer(const uint8_t* base, uint32_t container);
// me_resolution.cpp: internal-resolution changes for the D3D functions this file hooks.
void MeResolutionBefore(uint32_t address, PPCContext& ctx, uint8_t* base);

REXCVAR_DEFINE_BOOL(masseffect_d3d_stopwatch, false, "Mass Effect",
                    "Time the hooked Direct3D entry points (1 call in 16) and report calls and ms per frame every 10 s")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

constexpr uint32_t kSwap = 0x82233DF0;

#define X(a) 0x##a,
constexpr uint32_t kAddresses[] = {
#ifdef MASSEFFECT_D3D_TRACE_ALL
#include "me_d3d_trace_all.inc"
#else
  X(82227550)
  X(82227A30)
  X(82227F68)
  X(82228358)
  X(8221E088)
  X(82234B88)
  X(82224488)
  X(82224318)
  X(82233DF0)
#endif
};
#undef X
constexpr size_t kCount = sizeof(kAddresses) / sizeof(kAddresses[0]);

bool g_cron_active = false;  // masseffect_d3d_stopwatch
std::chrono::steady_clock::time_point g_cron_report = std::chrono::steady_clock::now();

void Init() {
  g_cron_active = REXCVAR_GET(masseffect_d3d_stopwatch);
  g_cron_report = std::chrono::steady_clock::now();
}

// A function-local static instead of std::call_once: on this toolchain call_once takes a global lock
// (pthread_once has no fast path), and this runs on every hooked D3D call.
inline void EnsureInit() {
  // Not a function-local static: its guard is a load-acquire on every hooked D3D call.
  static std::atomic<bool> done{false};
  if (done.load(std::memory_order_relaxed)) [[likely]] return;
  static std::mutex m;
  std::lock_guard<std::mutex> lock(m);
  if (!done.load(std::memory_order_relaxed)) {
    Init();
    done.store(true, std::memory_order_release);
  }
}

constexpr bool IsDraw(uint32_t a) {
  return a == 0x82228358 || a == 0x82227A30 || a == 0x82227550 || a == 0x82227F68;
}

constexpr size_t IndexOf(uint32_t address) {
  for (size_t i = 0; i < kCount; ++i) {
    if (kAddresses[i] == address) {
      return i;
    }
  }
  return kCount;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// masseffect_d3d_stopwatch: inclusive time of the hooked D3D entry points (1 call in 16 is timed with the
// counter-timer register), reported every 10 s at Swap: calls per frame and estimated ms per frame per function.
// Measurement for the D3D hot-path work (which functions are worth replacing natively).
struct Cron {
  std::atomic<uint64_t> calls{0}, ticks{0}, samples{0};
};
Cron g_cron[kCount];
std::atomic<uint64_t> g_cron_frames{0};

inline uint64_t CronTicks() {
  return armGetSystemTick();  // mrs cntpct_el0 (cntvct_el0 faulted on Horizon)
}
inline double CronHz() {
  return double(armGetSystemTickFreq());
}

void CronReport() {
  const auto now = std::chrono::steady_clock::now();
  g_cron_frames.fetch_add(1, std::memory_order_relaxed);
  if (now - g_cron_report < std::chrono::seconds(10)) return;
  g_cron_report = now;
  const uint64_t frames = std::max<uint64_t>(1, g_cron_frames.exchange(0));
  struct Row { double ms; double calls; uint32_t dir; double us; };
  std::vector<Row> rows;
  const double hz = CronHz();
  for (size_t k = 0; k < kCount; ++k) {
    const uint64_t n = g_cron[k].calls.exchange(0), t = g_cron[k].ticks.exchange(0),
                   m = g_cron[k].samples.exchange(0);
    if (!n || !m) continue;
    const double us = double(t) / double(m) / hz * 1e6;  // per call
    rows.push_back({us * double(n) / 1000.0 / double(frames), double(n) / double(frames), kAddresses[k], us});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ms > b.ms; });
  std::string text;
  for (size_t i = 0; i < rows.size() && i < 14; ++i) {
    char b[96];
    std::snprintf(b, sizeof(b), " %08X %.2fms (%.0f x %.2fus)", rows[i].dir, rows[i].ms, rows[i].calls,
                  rows[i].us);
    text += b;
  }
  REXLOG_INFO("[native] ME D3D timer ({} frames, inclusive, per frame):{}", frames, text);
}

#define X(a)                                                     \
  REX_EXTERN(__imp__sub_##a);                                    \
  extern "C" REX_FUNC(sub_##a) {                                 \
    static_assert(IndexOf(0x##a) < kCount);                      \
    EnsureInit();                                                \
    if constexpr (IsDraw(0x##a)) {                               \
      me::native::NoteDrawCall(base, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32); \
    }                                                            \
    if constexpr (0x##a == 0x8221E088) MeShaderDumpContainer(base, ctx.r4.u32); \
    if constexpr (0x##a == 0x82234B88 || 0x##a == 0x82224488 || 0x##a == 0x82224318) \
      MeResolutionBefore(0x##a, ctx, base);                      \
    uint64_t cron_t0 = 0;                                        \
    bool cron_measure = false;                                     \
    if (g_cron_active) {                                         \
      const uint64_t n_ = g_cron[IndexOf(0x##a)].calls.fetch_add(1, std::memory_order_relaxed); \
      if ((n_ & 15) == 0) { cron_measure = true; cron_t0 = CronTicks(); } \
    }                                                            \
    __imp__sub_##a(ctx, base);                                   \
    if (cron_measure) {                                            \
      g_cron[IndexOf(0x##a)].ticks.fetch_add(CronTicks() - cron_t0, std::memory_order_relaxed); \
      g_cron[IndexOf(0x##a)].samples.fetch_add(1, std::memory_order_relaxed); \
    }                                                            \
    if constexpr (0x##a == kSwap) { if (g_cron_active) CronReport(); } \
  }
#ifdef MASSEFFECT_D3D_TRACE_ALL
#include "me_d3d_trace_all.inc"
#else
  X(82227550)
  X(82227A30)
  X(82227F68)
  X(82228358)
  X(8221E088)
  X(82234B88)
  X(82224488)
  X(82224318)
  X(82233DF0)
#endif
#undef X

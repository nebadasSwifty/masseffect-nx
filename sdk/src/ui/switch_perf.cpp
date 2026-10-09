/**
 * @file        switch_perf.cpp
 * @brief       Performance measurement on the console, without a debugger
 *
 * The first boot that reached the title screen ran at 5-10 fps, with two cores at 97% and the
 * GPU at 99.7% according to the Horizon OC monitor. That does not tell what to optimize: it takes
 * knowing which thread spends the CPU and in which function. Horizon has no perf and no debugger
 * on a retail console, so it is measured from inside. Every 10 s a report is appended to
 * <NRO folder>/logs/rex/rex_profile.log:
 *
 *  - Game FPS: guest presents (IssueSwap).
 *  - Faults per second by type: each one costs an exception, or two with the resume through the
 *    kernel.
 *  - Exact CPU of each thread: the kernel keeps each thread's CPU time (svcGetInfo with
 *    InfoType_ThreadTickCount, 13.0.0+), with its priority, its preferred core and its core mask.
 *  - Where it is spent: sampling at 1 kHz. A high-priority thread pauses a busy thread
 *    (svcSetThreadActivity), reads its pc, its lr and its stack through the x29 chain
 *    (svcGetThreadContext3) and resumes it. The stacks also tell what a thread stopped in the
 *    kernel is waiting for. Addresses come out as image+0x...; they are translated with addr2line
 *    on the ELF.
 *
 * Threads: by wrapping libnx's threadCreate and threadClose (--wrap in rexglue_switch.cmake). Every
 * thread goes through there, pthread ones included, so Mesa's and the SDK's show up too. The name
 * comes from pthread_setname_np (switch_libc_supplement.c).
 *
 * libnx calls: also with --wrap, the calls that cost IPC or walk memory are counted, along with the
 * time the calling thread spends inside: fences (zero-timeout queries and waits), submission
 * kickoffs, new NvMaps (with and without CPU cache), GPU addresses and mappings, armDCacheClean, the
 * window queue and svcSleepThread. Every fence query is at least one ioctl even when the GPU has
 * already finished (see docs/platform-notes.md), so the count per second is needed before changing
 * anything.
 *
 * Careful with the pause: while a thread is paused nothing is done that could take a lock (no
 * malloc, no stdio): if the paused thread held it, this one would wait forever. Between pause and
 * resume there are only system calls. If pausing fails (for example because the SDK had already
 * suspended the thread) it is left alone: resuming it would break that suspension.
 *
 * It goes in the executable (rexglue_switch_startup): the SDK calls RexSwitchPerfCount from
 * libraries, and the wrappers have to be defined before libnx.a enters the link.
 */

#include <malloc.h>
#include <unistd.h>

// libnx: the end of the heap that newlib's sbrk hands out (see HeapState).
extern "C" char* fake_heap_end;
#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <switch.h>

#include "rex/ui/switch_apm.h"
#include "rex/ui/switch_saltynx.h"
#include "rex/ui/switch_sysclk.h"
#include "rex/ui/switch_thread_snapshot.h"

extern "C" {
void _start(void);
/* rexglue log folder, ending in '/' (switch_crash_hooks.c). */
const char* RexSwitchLogDir(void);
/*
 * From the guest memory core (guest_memory_switch.cpp): how much backing is
 * committed and how much is mapped in total, counting the 360 mirrors.
 */
size_t RexGmCommittedBytes(void);
size_t RexGmMappedBytes(void);
/* 0 = untested, 1 = permissions (watched pages readable), 2 = unmapping. */
int RexGmProtectionMode(void);
size_t RexGmLayoutSummary(char* buf, size_t cap);
Result __real_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem,
                           size_t stack_sz, int prio, int cpuid);
Result __real_threadClose(Thread* t);
Result __real_nvFenceWait(NvFence* f, s32 timeout_us);
Result __real_nvGpuChannelKickoff(NvGpuChannel* c);
Result __real_nvMapCreate(NvMap* m, void* cpu_addr, u32 size, u32 align, NvKind kind, bool is_cpu_cacheable);
Result __real_nvAddressSpaceAllocFixed(NvAddressSpace* a, bool sparse, u64 size, iova_t iova);
Result __real_nvioctlNvhostAsGpu_MapBufferEx(u32 fd, u32 flags, u32 kind, u32 nvmap_handle, u32 page_size,
                                             u64 buffer_offset, u64 mapping_size, u64 input_offset, u64* offset);
void __real_armDCacheClean(void* addr, size_t size);
Result __real_nwindowQueueBuffer(NWindow* nw, s32 slot, const NvMultiFence* fence);
Result __real_bqDequeueBuffer(Binder* b, bool async, u32 width, u32 height, s32 format, u32 usage, s32* buf,
                              NvMultiFence* fence);
Result __real_nwindowCancelBuffer(NWindow* nw, s32 slot, const NvMultiFence* fence);
Result __real_nwindowReleaseBuffers(NWindow* nw);
Result __real_nwindowConfigureBuffer(NWindow* nw, s32 slot, NvGraphicBuffer* buf);
Result __real_bqRequestBuffer(Binder* b, s32 bufferIdx, BqGraphicBuffer* buf);
Result __real_bqCancelBuffer(Binder* b, s32 buf, const NvMultiFence* fence);
void __real_svcSleepThread(s64 nano);
}

namespace {

constexpr size_t kMaxThreads = 160;
constexpr u64 kSampleIntervalNs = 1000000ULL;  // 1 ms
constexpr u64 kSecondNs = 1000000000ULL;      // tick for the console overlays

/* internal resolution published to the overlays. The game sets it when choosing the video mode. */
std::atomic<uint32_t> g_resolution{(1280u << 16) | 720u};

/*
 * A free-text label the game sets (the location tour: "<map> <stop>/<stops>", app/src/native/me_tour.cpp) and the
 * number of report blocks written, so each block can be attributed to a place. Two buffers: the writer fills the
 * one the reader is not using and then publishes it; the reader retries once if a write raced its copy.
 */
constexpr size_t kLabelBytes = 96;
char g_labels[2][kLabelBytes]{};
std::atomic<uint32_t> g_label_seq{0};
std::atomic<uint32_t> g_report_index{0};

void ReadLabel(char out[kLabelBytes]) {
  for (int tries = 0; tries < 2; ++tries) {
    const uint32_t seq = g_label_seq.load(std::memory_order_acquire);
    std::memcpy(out, g_labels[seq & 1], kLabelBytes);
    out[kLabelBytes - 1] = 0;
    if (g_label_seq.load(std::memory_order_acquire) == seq) {
      return;
    }
  }
}
constexpr u64 kPassiveIntervalNs = 100000000ULL;  // 100 ms, without pausing threads
// Next to the rexglue logs, in <NRO folder>/logs/rex/.
std::string StackFlagPath() {
  return std::string(RexSwitchLogDir()) + "stacks_profile.flag";
}
constexpr u64 kReportSeconds = 10;
/*
 * Let the game start first. This only delays stack sampling and the first report, which read the
 * SD and pause threads. The console overlays (FPS and resolution) are published from second zero:
 * see the warm-up loop in ProfilerMain.
 */
constexpr u64 kStartDelayNs = 8000000000ULL;
constexpr size_t kMaxSamples = 1 << 15;
constexpr size_t kFrames = 10;
// 0-32 renderer/audio counters (see RexSwitchPerfAdd); 40-55 host-runtime overhead (include/rex/sys_counters.h);
// 56-58 XMA stuck-voice diagnostics (xma_context.cpp, audio_xma_diag).
constexpr unsigned kCounterCount = 64;
// In <NRO folder>/logs/rex/ (switch_crash_hooks.c computes it at startup).
std::string ReportPath() {
  return std::string(RexSwitchLogDir()) + "rex_profile.log";
}

struct Slot {
  std::atomic<u32> handle{0};
  char name[32]{};
  // Guest threads: the guest start routine and its argument (XThread creation parameters), for the report.
  std::atomic<u32> guest_entry{0};
  std::atomic<u32> guest_context{0};
  // Render-thread role marks (one per guest D3D Swap, RexSwitchPerfNoteThreadRole), written by the thread itself.
  std::atomic<u32> swaps{0};
  // Only the profiler thread touches these:
  u32 seen = 0;
  u64 last_ticks = 0;
  bool busy = true;
  double cpu = 0.0;
  u32 swaps_last = 0;
  u32 swaps_interval = 0;
};
Slot g_slots[kMaxThreads];
// The game thread (the one that runs UGameEngine::Tick, RexSwitchPerfNoteThreadRole), 0 = not seen yet.
std::atomic<u32> g_game_thread{0};
// Exclusive core for the hottest guest thread (the game's main thread): -1 = off. See ApplyExclusiveCore.
std::atomic<int> g_exclusive_core{-1};
std::atomic<bool> g_light_core{false};
std::atomic<bool> g_core_render{false};

struct Sample {
  u64 pc;
  u64 lr;
  u64 frames[kFrames];  // return addresses along the x29 chain
  u16 slot;
  u64 tick;  // when it was taken (armGetSystemTick), to separate those from long frames
};

/*
 * Long frame windows. The renderer calls RexSwitchPerfHitch with the interval of every frame over
 * 45 ms; the report separates the samples taken inside those intervals and says what each thread was
 * doing right then (section "during the hitches"). Only works with stacks_profile.flag.
 */
constexpr size_t kMaxHitches = 256;
struct WindowHitch {
  u64 start;
  u64 end;
};
WindowHitch g_hitches[kMaxHitches];
std::atomic<u32> g_num_hitches{0};
Sample g_samples[kMaxSamples];
size_t g_sample_count = 0;

/*
 * Held while a thread is paused by the sampler or by RexSwitchSnapshotThreads, so one of them never resumes a
 * thread the other has just paused. The sampler only tries it (it skips that sample); the snapshot waits.
 */
std::atomic<bool> g_pause_busy{false};

std::atomic<u64> g_counters[kCounterCount];

/*
 * Wrapped libnx calls (see the file header): how many, how much time inside and, where it makes
 * sense, how many bytes. They are constant-initialized, so they are valid even if someone calls
 * before the constructors.
 */
enum : unsigned {
  kFenceQuery,     // nvFenceWait with wait 0
  kFenceWait,       // nvFenceWait with wait
  kKickoff,           // nvGpuChannelKickoff
  kNvMapCached,     // nvMapCreate with CPU cache
  kNvMapNoCache,     // nvMapCreate without CPU cache
  kReserveAddress,  // nvAddressSpaceAllocFixed
  kMappingGpu,          // nvioctlNvhostAsGpu_MapBufferEx
  kCacheClean,        // armDCacheClean
  kQueueBuffer,       // nwindowQueueBuffer
  kDequeueBuffer,     // bqDequeueBuffer
  kSleep0,            // svcSleepThread(0)
  kSleepYield,        // svcSleepThread(-1 o -2)
  kSleepShort,        // up to 1 ms
  kSleepLong,        // over 1 ms
  kCallsCount,
};
struct Calls {
  std::atomic<u64> n{0};
  std::atomic<u64> ticks{0};
  std::atomic<u64> bytes{0};
};
Calls g_calls[kCallsCount];

inline void Note(unsigned id, u64 from, u64 bytes = 0) {
  const u64 ticks = armGetSystemTick() - from;
  Calls& l = g_calls[id];
  l.n.fetch_add(1, std::memory_order_relaxed);
  l.ticks.fetch_add(ticks, std::memory_order_relaxed);
  if (bytes) {
    l.bytes.fetch_add(bytes, std::memory_order_relaxed);
  }
}

/*
 * Automatic A/B tests. Each report (10 s) removes one part of the GPU work: those commands (draw or
 * dispatch) stop being recorded, but everything else stays the same, so the state does not break.
 * The image looks wrong while the mode lasts; that is expected. If with "nothing" it is just as
 * slow, the cost is not in the work but in something fixed (submissions, driver).
 */
struct Mode {
  u32 mask;
  const char* name;
};
constexpr Mode kModes[] = {
    {0, "normal"},
    {1, "no draws"},
    {2, "no transfers"},
    {4, "no resolves"},
    {8, "no texture loads"},
    {16, "no presentation effect"},
    {32, "no tile dispatch"},
    {63, "nothing"},
};
constexpr size_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);

/*
 * A/B tests, started by hand
 *
 * They used to be enabled with this constant and rotate on their own from startup. That is no good:
 * the game takes a while to reach a race, and on the way it spent its time in modes with drawing
 * turned off. Once they were left on by mistake and the game did not even reach the title screen.
 *
 * Now the player starts the rotation with L+R+ZL from inside the race, which is the only place where
 * the measurement is useful. From then on it removes one part of the GPU work per report, goes once
 * through the eight modes and returns to "normal".
 *
 * How to read it. If with "nothing" the game is just as slow, the cost is not in the GPU work but
 * in something fixed: submissions, the driver, or the guest itself. If FPS shoots up, the mode where
 * it rises most points to the culprit.
 */
std::atomic<bool> g_ab_active{false};
std::atomic<u32> g_skip_mask{0};

extern "C" void RexSwitchPerfToggleAb(void) {
  const bool new_value = !g_ab_active.load(std::memory_order_relaxed);
  g_ab_active.store(new_value, std::memory_order_relaxed);
  if (!new_value) {
    g_skip_mask.store(0, std::memory_order_relaxed);
  }
}

/*
 * Horizon limits how much memory a process may map (LimitableResource_Memory). Our
 * guest memory model maps each chunk several times (the shadow and once per 360
 * view), so that limit is the real ceiling, not free memory. When it runs out,
 * even committing 4 KB fails with 2001-0103 (resource exhausted).
 */
// The process "used" size above includes the whole heap that libnx reserves at start, so it is always close to the
// total. The real state of the host heap: bytes malloc hands out, free bytes inside what malloc already took from the
// heap, and the heap that malloc has not taken yet (fake_heap_end - sbrk(0)).
void HeapState(u64* in_use_mb, u64* free_in_arena_mb, u64* untouched_mb) {
  const struct mallinfo mi = mallinfo();
  *in_use_mb = static_cast<u64>(mi.uordblks) >> 20;
  *free_in_arena_mb = static_cast<u64>(mi.fordblks) >> 20;
  char* top = static_cast<char*>(sbrk(0));
  *untouched_mb = (fake_heap_end && top && fake_heap_end > top) ? static_cast<u64>(fake_heap_end - top) >> 20 : 0;
}

void LimitOfMapping(u64* used_mb, u64* cap_mb, u64* process_mb, u64* total_mb) {
  *used_mb = *cap_mb = *process_mb = *total_mb = 0;
  u64 v = 0;
  if (R_SUCCEEDED(svcGetInfo(&v, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0))) {
    *process_mb = v >> 20;
  }
  if (R_SUCCEEDED(svcGetInfo(&v, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0))) {
    *total_mb = v >> 20;
  }
  Handle reslimit = INVALID_HANDLE;
  if (R_FAILED(svcGetInfo(&v, InfoType_ResourceLimit, CUR_PROCESS_HANDLE, 0))) {
    return;
  }
  reslimit = static_cast<Handle>(v);
  s64 actual = 0, cap = 0;
  if (R_SUCCEEDED(svcGetResourceLimitCurrentValue(&actual, reslimit, LimitableResource_Memory))) {
    *used_mb = static_cast<u64>(actual) >> 20;
  }
  if (R_SUCCEEDED(svcGetResourceLimitLimitValue(&cap, reslimit, LimitableResource_Memory))) {
    *cap_mb = static_cast<u64>(cap) >> 20;
  }
  svcCloseHandle(reslimit);
}

u64 g_base = 0, g_text_lo = 0, g_text_hi = 0;
Thread g_thread;

void Register(Handle h) {
  if (!h) {
    return;
  }
  for (auto& s : g_slots) {
    u32 expected = 0;
    if (s.handle.compare_exchange_strong(expected, h)) {
      s.name[0] = 0;
      s.guest_entry.store(0, std::memory_order_relaxed);
      s.guest_context.store(0, std::memory_order_relaxed);
      return;
    }
  }
}

void Unregister(Handle h) {
  if (!h) {
    return;
  }
  for (auto& s : g_slots) {
    u32 expected = h;
    if (s.handle.compare_exchange_strong(expected, 0)) {
      return;
    }
  }
}

bool InText(u64 addr) { return addr >= g_text_lo && addr < g_text_hi; }

void PrintAddr(FILE* f, u64 addr) {
  if (InText(addr)) {
    std::fprintf(f, "image+0x%" PRIx64, addr - g_base);
  } else {
    std::fprintf(f, "0x%016" PRIx64, addr);
  }
}

/*
 * Is the pc right after a system call? Then the thread is waiting in the kernel,
 * not using CPU.
 */
bool AfterSvc(u64 pc) {
  if (!InText(pc) || pc < g_text_lo + 4) {
    return false;
  }
  // A thread blocked in the kernel reports either the svc itself or the instruction after it as its pc
  // (on this firmware the svc itself: every thread showed 0 % in the kernel with only the pc - 4 check).
  auto is_svc = [](u32 insn) { return (insn & 0xFFE0001Fu) == 0xD4000001u; };
  return is_svc(*reinterpret_cast<const u32*>(pc - 4)) ||
         (InText(pc + 4) && is_svc(*reinterpret_cast<const u32*>(pc)));
}

struct Count {
  u64 addr;
  u32 n;
};

std::vector<Count> Histogram(std::vector<u64>& addrs) {
  std::sort(addrs.begin(), addrs.end());
  std::vector<Count> out;
  for (u64 a : addrs) {
    if (out.empty() || out.back().addr != a) {
      out.push_back({a, 1});
    } else {
      ++out.back().n;
    }
  }
  std::sort(out.begin(), out.end(), [](const Count& x, const Count& y) { return x.n > y.n; });
  return out;
}

/*
 * Exclusive core (RexSwitchPerfExclusiveCore): once the game's main thread uses more than 60 % of a core, it is
 * pinned to core K alone and every other registered thread loses core K from its mask (threads pinned to K alone are
 * left alone). Re-applied at every report so that threads created later move off K too. Same work, different
 * placement: the main thread no longer shares its core with the ring thread and the C20 worker, which run at a higher
 * priority (0x2C < 0x3B).
 *
 * The threads are chosen by what they run, not by their CPU share: the main thread is the one that runs
 * UGameEngine::Tick and the render thread the one that calls the guest D3D Swap (both reported by the game hooks
 * through RexSwitchPerfNoteThreadRole). The CPU ranking picked a guest timer thread that spun at 92 % of a core in one
 * boot and raised it above the ring thread instead of the render thread (docs/kernel-waits.md). Without the role
 * marks (a build without those hooks) the main thread falls back to the old rule, the hottest "XThread..." thread, and
 * no thread is raised.
 */
const Slot* FindSlot(const std::vector<size_t>& order, u32 handle) {
  if (!handle) return nullptr;
  for (size_t i : order) {
    if (g_slots[i].handle.load() == handle) return &g_slots[i];
  }
  return nullptr;
}

void ApplyExclusiveCore(FILE* f, const std::vector<size_t>& order) {
  static u32 pinned = 0;
  const int k = g_exclusive_core.load(std::memory_order_relaxed);
  if (k < 0 || k > 2) return;
  const u64 bit = 1ull << k;
  if (!pinned) {
    const u32 game = g_game_thread.load(std::memory_order_relaxed);
    const Slot* main_slot = nullptr;
    const char* why = "game thread: runs UGameEngine::Tick";
    if (game) {
      main_slot = FindSlot(order, game);
    } else {
      // Fallback (no role marks): the hottest guest thread.
      why = "hottest guest thread; game thread not identified";
      for (size_t i : order) {
        if (std::strncmp(g_slots[i].name, "XThread", 7) == 0) {
          main_slot = &g_slots[i];
          break;
        }
      }
    }
    if (main_slot && main_slot->cpu >= 60.0 && R_SUCCEEDED(svcSetThreadCoreMask(main_slot->handle.load(), k, bit))) {
      pinned = main_slot->handle.load();
      std::fprintf(f, "-- exclusive core %d for \"%s\" (%s; CPU %.1f%%, guest entry 0x%08X)\n", k, main_slot->name, why,
                   main_slot->cpu, main_slot->guest_entry.load(std::memory_order_relaxed));
    }
    if (!pinned) return;
  }
  // 20 + K: also raise UE3's render thread (the main thread's critical path since C45) above the ring thread and the
  // C20 worker (0x2C), which preempted it on cores 0-1. The render thread = the thread with the most guest D3D Swap
  // calls in the last interval (at least kMinSwaps). If another thread takes that role later, the previous one gets
  // its old priority back.
  static u32 raised = 0;
  static s32 raised_old_priority = -1;
  static bool reported_unknown = false;
  if (g_core_render.load(std::memory_order_relaxed)) {
    constexpr u32 kMinSwaps = 10;
    const Slot* render = nullptr;
    for (size_t i : order) {
      const Slot& s = g_slots[i];
      if (s.handle.load() == pinned || s.swaps_interval < kMinSwaps) continue;
      if (!render || s.swaps_interval > render->swaps_interval) render = &s;
    }
    if (render && render->handle.load() != raised) {
      const u32 h = render->handle.load();
      s32 old_priority = -1;
      svcGetThreadPriority(&old_priority, h);
      if (R_SUCCEEDED(svcSetThreadPriority(h, 0x2B))) {
        if (raised && raised_old_priority >= 0) {
          // May fail if that thread is gone; nothing else to undo then.
          svcSetThreadPriority(raised, raised_old_priority);
          std::fprintf(f, "-- priority 0x%X restored for the previous render thread (handle 0x%X)\n",
                       static_cast<unsigned>(raised_old_priority), raised);
        }
        raised = h;
        raised_old_priority = old_priority;
        std::fprintf(f,
                     "-- priority 0x2B for \"%s\" (render thread: %u guest D3D Swaps in the interval; CPU %.1f%%, "
                     "guest entry 0x%08X, was priority 0x%X)\n",
                     render->name, render->swaps_interval, render->cpu,
                     render->guest_entry.load(std::memory_order_relaxed), static_cast<unsigned>(old_priority));
      }
    } else if (!render && !raised && !reported_unknown) {
      reported_unknown = true;
      std::fprintf(f, "-- render thread not identified yet (no guest D3D Swap seen): no thread raised\n");
    }
  }
  const bool light = g_light_core.load(std::memory_order_relaxed);
  for (size_t i = 0; i < kMaxThreads; ++i) {
    const u32 h = g_slots[i].handle.load();
    if (!h || h == pinned) continue;
    s32 preferred = -1;
    u64 mask = 0;
    if (R_FAILED(svcGetThreadCoreMask(&preferred, &mask, h))) continue;
    // Light guest threads (audio, streaming: under 35 % of a core) may use the main thread's spare time on K,
    // which leaves cores 0-1 to the ring and render threads. Same priority as the main thread (0x3B): they
    // only run on K while it waits.
    if (light && std::strncmp(g_slots[i].name, "XThread", 7) == 0 && g_slots[i].cpu < 35.0) {
      if (mask == 0x6 || mask == 0x5 || mask == 0x3) svcSetThreadCoreMask(h, -3, mask | bit);
      continue;
    }
    if (!(mask & bit) || mask == bit) continue;
    const u64 new_entry = mask & ~bit;
    if (preferred == k || (preferred >= 0 && !(new_entry & (1ull << preferred)))) preferred = -1;
    svcSetThreadCoreMask(h, preferred, new_entry);
  }
}

void Report(u64 elapsed_ticks, u64 tick_freq, u64 counters_last[kCounterCount],
            const char* mode) {
  const double seconds = double(elapsed_ticks) / double(tick_freq);

  u64 counters_now[kCounterCount];
  for (unsigned i = 0; i < kCounterCount; ++i) {
    counters_now[i] = g_counters[i].load(std::memory_order_relaxed);
  }

  // CPU of each thread over the interval, as % of one core.
  std::vector<size_t> order;
  for (size_t i = 0; i < kMaxThreads; ++i) {
    Slot& s = g_slots[i];
    const u32 h = s.handle.load();
    if (!h) {
      s.seen = 0;
      continue;
    }
    u64 ticks = 0;
    if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, h, UINT64_MAX))) {
      continue;
    }
    const u32 swaps = s.swaps.load(std::memory_order_relaxed);
    if (s.seen != h) {  // new thread: no interval yet
      s.seen = h;
      s.last_ticks = ticks;
      s.busy = true;
      s.cpu = 0.0;
      s.swaps_last = swaps;
      s.swaps_interval = 0;
      continue;
    }
    s.swaps_interval = swaps - s.swaps_last;
    s.swaps_last = swaps;
    s.cpu = double(ticks - s.last_ticks) * 100.0 / double(elapsed_ticks);
    s.last_ticks = ticks;
    /*
     * All threads are sampled, including those that use no CPU. In a hang caused by
     * a wait, the thread that matters is exactly that one: the stopped one.
     */
    s.busy = true;
    order.push_back(i);
  }
  std::sort(order.begin(), order.end(),
            [](size_t a, size_t b) { return g_slots[a].cpu > g_slots[b].cpu; });

  FILE* f = std::fopen(ReportPath().c_str(), "a");
  if (!f) {
    g_sample_count = 0;
    return;
  }

  ApplyExclusiveCore(f, order);

  u64 lim_used = 0, lim_cap = 0, proc_used = 0, proc_total = 0;
  LimitOfMapping(&lim_used, &lim_cap, &proc_used, &proc_total);
  u64 heap_in_use = 0, heap_free = 0, heap_untouched = 0;
  HeapState(&heap_in_use, &heap_free, &heap_untouched);

  double total_cpu = 0.0;
  for (size_t i : order) {
    total_cpu += g_slots[i].cpu;
  }
  std::fprintf(f,
               "==== %.1f s | mode: %s | game %.1f fps | CPU total %.0f%% (400%% = 4 cores) | faults/s: "
               "emulated read %.0f, SDK handler %.0f, emulated retry %.0f, SEH %.0f, "
               "physical committed %.0f, views %.0f | guest %zu/%zu MB (backing/mapped) | "
               "mapping limit %llu/%llu MB, process %llu/%llu MB, heap: malloc in use %llu MB, free in arena "
               "%llu MB, never taken %llu MB | "
               "samples %zu | watching: %s | samplers: %.0f new/s, %.0f stalls/s\n",
               seconds, mode, double(counters_now[0] - counters_last[0]) / seconds, total_cpu,
               double(counters_now[1] - counters_last[1]) / seconds,
               double(counters_now[2] - counters_last[2]) / seconds,
               double(counters_now[3] - counters_last[3]) / seconds,
               double(counters_now[4] - counters_last[4]) / seconds,
               double(counters_now[17] - counters_last[17]) / seconds,
               double(counters_now[18] - counters_last[18]) / seconds,
               RexGmCommittedBytes() >> 20, RexGmMappedBytes() >> 20,
               (u64)lim_used, (u64)lim_cap, (u64)proc_used, (u64)proc_total, heap_in_use, heap_free,
               heap_untouched, g_sample_count,
                (RexGmProtectionMode() == 1   ? "permissions (readable pages)"
                 : RexGmProtectionMode() == 2 ? "unmapping (every read faults)"
                                              : "untested"),
                double(counters_now[20] - counters_last[20]) / seconds,
                double(counters_now[19] - counters_last[19]) / seconds);
  {
    char label[kLabelBytes];
    ReadLabel(label);
    const uint32_t block = g_report_index.fetch_add(1, std::memory_order_relaxed) + 1;
    std::fprintf(f, "     block %u | label: %s\n", block, label[0] ? label : "-");
  }
  // How the guest window is mapped: 4 KB pages or 2 MB blocks (guest_memory_large_pages). See
  // docs/platform-notes.md, "Guest memory page size".
  {
    static char layout[1536];
    if (RexGmLayoutSummary(layout, sizeof(layout)) > 0) {
      std::fprintf(f, "     guest pages: %s\n", layout);
    }
  }
  // Work the game sends to the GPU, per presented frame. One screen is
  // 1280x720 = 921,600 pixels.
  const auto delta = [&](unsigned id) { return double(counters_now[id] - counters_last[id]); };
  const double frames = delta(0);
  // Mix clipping before conversion to 16 bits (21 samples above 1.0, 22 buffers with a peak of 0.98 or
  // more, 23 maximum peak of the interval in ten-thousandths) and the console mode, for the crackling
  // in handheld mode.
  const u64 audio_peak = g_counters[23].exchange(0, std::memory_order_relaxed);
  std::fprintf(f,
               "     audio: %.0f client blocks mixed, %.0f requests without data, "
               "%.0f/%.0f audout buffers with non-zero PCM, %.0f saturated samples (above 1.0 before clipping), "
               "%.0f buffers with a peak of 0.98 or more, maximum peak %.3f | XMA: %.0f blocks written, "
               "%.0f repeated-block runs, %.0f stalls of 1 s or more (audio_xma_diag) | console in %s mode\n",
               delta(24), delta(25), delta(26), delta(27), delta(21), delta(22), double(audio_peak) / 10000.0,
               delta(58), delta(56), delta(57),
               rex::ui::switch_saltynx::BaseMode(appletGetOperationMode() == AppletOperationMode_Console)
                   ? "docked"
                   : "handheld");
  // Reverse-NX state as is, without interpretation. A whole test session was wasted because its
  // overlay said "Docked" while "Controlled by system" was Yes, and in that case its mode does not
  // rule: it only mirrors the console's. This shows at a glance which of the two is happening.
  {
    const auto reverse = rex::ui::switch_saltynx::ReverseState();
    const bool real = appletGetOperationMode() == AppletOperationMode_Console;
    if (!reverse.has) {
      std::fprintf(f, "     Reverse-NX: no block (console %s, it rules)\n",
                   real ? "docked" : "in hand");
    } else {
      std::fprintf(f,
                   "     Reverse-NX: says %s, rules %s (Controlled by system %s), the game has asked: %s; "
                   "real console %s -> obeying %s\n",
                   reverse.in_base ? "docked" : "handheld", reverse.by_default ? "the console" : "Reverse-NX",
                   reverse.by_default ? "Yes" : "No", reverse.plugin_active ? "yes" : "no",
                   real ? "docked" : "in hand",
                   rex::ui::switch_saltynx::BaseMode(real) ? "docked" : "handheld");
    }
  }
  // Real console clocks (clkrst, 8.0.0+) and the cores the process may use. It tells whether the
  // session ran overclocked and with which CPU limit it was measured: performance tests run without
  // overclock.
  {
    static bool started = false;
    static bool has_clkrst = false;
    static ClkrstSession sessions[3]{};
    static u64 cores_mask = 0;
    if (!started) {
      started = true;
      if (R_FAILED(svcGetInfo(&cores_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) {
        cores_mask = 0;
      }
      if (R_SUCCEEDED(clkrstInitialize())) {
        static const PcvModuleId modules[3] = {PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC};
        has_clkrst = true;
        for (unsigned i = 0; i < 3; ++i) {
          if (R_FAILED(clkrstOpenSession(&sessions[i], modules[i], 3))) {
            has_clkrst = false;
          }
        }
      }
    }
    u32 hz[3] = {0, 0, 0};
    if (has_clkrst) {
      for (unsigned i = 0; i < 3; ++i) {
        clkrstGetClockRate(&sessions[i], &hz[i]);
      }
    }
    unsigned cores = 0;
    for (unsigned i = 0; i < 64; ++i) {
      cores += unsigned((cores_mask >> i) & 1);
    }
    // SoC and board temperature (ts service, sessions on [10.0.0+]). The system lowers clocks around
    // 70 degrees and Erista gets there before Mariko: without this, "it is slow" cannot be told apart
    // from "it is hot". On the OLED model the board sensor reads wrong, because it sits next to the
    // charging IC.
    static bool ts_started = false;
    static bool has_ts = false;
    static TsSession ts_soc{}, ts_board{};
    if (!ts_started) {
      ts_started = true;
      if (R_SUCCEEDED(tsInitialize())) {
        has_ts = R_SUCCEEDED(tsOpenSession(&ts_soc, TsDeviceCode_LocationExternal)) &&
                 R_SUCCEEDED(tsOpenSession(&ts_board, TsDeviceCode_LocationInternal));
      }
    }
    float degrees_soc = 0.0f, degrees_board = 0.0f;
    if (has_ts) {
      tsSessionGetTemperature(&ts_soc, &degrees_soc);
      tsSessionGetTemperature(&ts_board, &degrees_board);
    }
    std::fprintf(f,
                 "     clocks: CPU %.1f MHz, GPU %.1f MHz, memory %.1f MHz%s | process cores %u "
                 "(mask 0x%llX) | SoC %.1f C, board %.1f C%s\n",
                 double(hz[0]) / 1.0e6, double(hz[1]) / 1.0e6, double(hz[2]) / 1.0e6,
                 has_clkrst ? "" : " (clkrst not available)", cores,
                 (unsigned long long)cores_mask, double(degrees_soc), double(degrees_board),
                 has_ts ? "" : " (ts not available)");
  }
  // libnx calls: per second, with the ms per second the calling threads spend inside.
  {
    static u64 last[kCallsCount][3];
    double n[kCallsCount], ms[kCallsCount], mb[kCallsCount];
    for (unsigned i = 0; i < kCallsCount; ++i) {
      const u64 vn = g_calls[i].n.load(std::memory_order_relaxed);
      const u64 vt = g_calls[i].ticks.load(std::memory_order_relaxed);
      const u64 vb = g_calls[i].bytes.load(std::memory_order_relaxed);
      n[i] = double(vn - last[i][0]) / seconds;
      ms[i] = double(vt - last[i][1]) * 1000.0 / double(tick_freq) / seconds;
      mb[i] = double(vb - last[i][2]) / 1048576.0 / seconds;
      last[i][0] = vn;
      last[i][1] = vt;
      last[i][2] = vb;
    }
    std::fprintf(f,
                 "     libnx per second (calls and ms inside): fences queried %.0f (%.1f ms), waited %.0f "
                 "(%.1f ms) | kickoff %.0f (%.1f ms) | NvMap new %.1f cached (%.1f MB) and %.1f uncached "
                 "(%.1f MB), %.1f ms | GPU addresses %.1f (%.1f ms), mappings %.1f (%.1f MB, %.1f ms) | "
                 "armDCacheClean %.0f (%.1f MB, %.1f ms) | window queue %.1f (%.1f ms), dequeue buffer %.1f "
                 "(%.1f ms) | svcSleepThread: 0 %.0f, yield %.0f, up to 1 ms %.0f (%.1f ms), more %.0f\n",
                 n[kFenceQuery], ms[kFenceQuery], n[kFenceWait], ms[kFenceWait], n[kKickoff],
                 ms[kKickoff], n[kNvMapCached], mb[kNvMapCached], n[kNvMapNoCache], mb[kNvMapNoCache],
                 ms[kNvMapCached] + ms[kNvMapNoCache], n[kReserveAddress], ms[kReserveAddress],
                 n[kMappingGpu], mb[kMappingGpu], ms[kMappingGpu], n[kCacheClean], mb[kCacheClean], ms[kCacheClean],
                 n[kQueueBuffer], ms[kQueueBuffer], n[kDequeueBuffer], ms[kDequeueBuffer], n[kSleep0],
                 n[kSleepYield], n[kSleepShort], ms[kSleepShort], n[kSleepLong]);
  }
  // Host-runtime overhead (rex/sys_counters.h): wake-ups spent polling and lock stalls, per second.
  std::fprintf(f,
               "     system per second: alertable waits: %.0f poll-slice wakes, %.0f woken by APC hint | "
               "WaitMultiple: %.0f parked, %.0f woken by signal, %.0f safety re-polls, %.0f legacy 1 ms polls | "
               "timestamp timer ticks %.0f | UpdateGuestClock %.0f calls, %.0f contended | ObjectTable lookups "
               "%.0f, %.0f contended | VolumeChangeMask %.0f | vblank thread wakes %.0f | native audio DSP calls "
               "%.0f, %.0f mismatches | critical-section host waits %.0f\n",
               delta(40) / seconds, delta(41) / seconds, delta(42) / seconds, delta(43) / seconds,
               delta(44) / seconds, delta(45) / seconds, delta(46) / seconds, delta(47) / seconds,
               delta(48) / seconds, delta(49) / seconds, delta(50) / seconds, delta(51) / seconds,
               delta(52) / seconds, delta(53) / seconds, delta(54) / seconds, delta(55) / seconds);
  if (frames > 0) {
    std::fprintf(f,
                 "     per frame: %.0f draws | scissor %.1f screens | %.1f submits | "
                 "%.1f resolves (%.2f screens) | transfers: %.1f calls, %.1f render "
                 "targets, %.1f draws | %.1f textures loaded | %.2f MB of shared "
                 "memory uploaded | pipelines created in the interval: %.0f\n",
                 delta(5) / frames, delta(6) / frames / 921600.0, delta(7) / frames,
                 delta(9) / frames, delta(10) / frames / 921600.0, delta(11) / frames,
                 delta(12) / frames, delta(13) / frames, delta(14) / frames,
                 delta(15) / frames / 1048576.0, delta(16));
    std::fprintf(f,
                 "     draws by surface width: 1600+ %.0f | 1280-1599 %.0f | "
                 "640-1279 %.0f | 256-639 %.0f | under 256 %.0f\n",
                 delta(28) / frames, delta(29) / frames, delta(30) / frames, delta(31) / frames,
                 delta(32) / frames);
  }

  for (unsigned i = 0; i < kCounterCount; ++i) {
    counters_last[i] = counters_now[i];
  }

  std::vector<Sample> samples(g_samples, g_samples + g_sample_count);
  g_sample_count = 0;
  std::sort(samples.begin(), samples.end(),
            [](const Sample& a, const Sample& b) { return a.slot < b.slot; });

  // What each thread was doing during the long frames of this interval.
  {
    const u32 nt = std::min<u32>(g_num_hitches.exchange(0), kMaxHitches);
    if (nt && !samples.empty()) {
      std::vector<WindowHitch> v(g_hitches, g_hitches + nt);
      std::sort(v.begin(), v.end(), [](const WindowHitch& a, const WindowHitch& b) { return a.start < b.start; });
      double ms_total = 0.0;
      for (const WindowHitch& w : v) {
        ms_total += double(w.end - w.start) * 1000.0 / double(tick_freq);
      }
      const auto inside = [&v](u64 t) {
        auto it = std::upper_bound(v.begin(), v.end(), t, [](u64 x, const WindowHitch& w) { return x < w.start; });
        return it != v.begin() && t <= (it - 1)->end;
      };
      std::fprintf(f, "\n== during the hitches: %u frames over 45 ms (%.0f ms in total) ==\n", nt, ms_total);
      for (size_t i : order) {
        std::vector<u64> pcs;
        std::vector<const Sample*> all;
        size_t inside_svc = 0;
        for (const Sample& m : samples) {
          if (m.slot != i || !inside(m.tick)) {
            continue;
          }
          all.push_back(&m);
          if (AfterSvc(m.pc)) {
            ++inside_svc;
          } else {
            pcs.push_back(m.pc);
          }
        }
        if (all.size() < 5) {
          continue;
        }
        const Slot& s = g_slots[i];
        std::fprintf(f, "-- thread \"%s\": %zu samples during the hitches, %.0f%% waiting in the kernel\n",
                     s.name[0] ? s.name : "?", all.size(), double(inside_svc) * 100.0 / double(all.size()));
        const auto h = Histogram(pcs);
        for (size_t k = 0; k < h.size() && k < 25; ++k) {
          std::fprintf(f, "   %5.1f%%  pc ", double(h[k].n) * 100.0 / double(all.size()));
          PrintAddr(f, h[k].addr);
          std::fputc('\n', f);
        }
        std::sort(all.begin(), all.end(), [](const Sample* a, const Sample* b) {
          return std::memcmp(a->frames, b->frames, sizeof(a->frames)) < 0;
        });
        std::vector<std::pair<const Sample*, u32>> stack_groups;
        for (const Sample* m : all) {
          if (stack_groups.empty() || std::memcmp(stack_groups.back().first->frames, m->frames, sizeof(m->frames)) != 0) {
            stack_groups.push_back({m, 1});
          } else {
            ++stack_groups.back().second;
          }
        }
        std::sort(stack_groups.begin(), stack_groups.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (size_t k = 0; k < stack_groups.size() && k < 10; ++k) {
          std::fprintf(f, "   %5.1f%%  stack", double(stack_groups[k].second) * 100.0 / double(all.size()));
          for (size_t j = 0; j < kFrames && stack_groups[k].first->frames[j]; ++j) {
            std::fputs(j == 0 ? " " : " <- ", f);
            PrintAddr(f, stack_groups[k].first->frames[j]);
          }
          std::fputc('\n', f);
        }
      }
    }
  }

  for (size_t i : order) {
    const Slot& s = g_slots[i];
    /*
     * Threads that use no CPU are listed too, with less detail: in a hang their
     * stacks are needed to know what they are waiting for.
     */
    const bool active = s.cpu >= 1.0;
    u64 tid = 0;
    svcGetThreadId(&tid, s.handle.load());
    // Horizon priority, preferred core and allowed cores: who can take the CPU from whom.
    s32 priority = -1;
    s32 preferred = -1;
    u64 mask = 0;
    svcGetThreadPriority(&priority, s.handle.load());
    svcGetThreadCoreMask(&preferred, &mask, s.handle.load());
    std::fprintf(f,
                 "\n-- thread %" PRIu64 " \"%s\": CPU %.1f%% | priority 0x%X, preferred core %d, "
                 "mask 0x%llX",
                 tid, s.name[0] ? s.name : "?", s.cpu, static_cast<unsigned>(priority),
                 static_cast<int>(preferred), static_cast<unsigned long long>(mask));
    // Guest threads: the guest start routine and argument, a name that stays the same between boots.
    if (const u32 entry = s.guest_entry.load(std::memory_order_relaxed)) {
      std::fprintf(f, " | guest entry 0x%08X ctx 0x%08X", entry, s.guest_context.load(std::memory_order_relaxed));
    }
    if (s.swaps_interval) {
      std::fprintf(f, " | %u guest Swaps", s.swaps_interval);
    }
    std::fputc('\n', f);

    auto lo = std::lower_bound(samples.begin(), samples.end(), i,
                               [](const Sample& a, size_t v) { return a.slot < v; });
    auto hi = std::upper_bound(samples.begin(), samples.end(), i,
                               [](size_t v, const Sample& a) { return v < a.slot; });
    const size_t n = size_t(hi - lo);
    if (n == 0) {
      std::fprintf(f, "   no samples\n");
      continue;
    }
    std::vector<u64> pcs, lrs;
    size_t in_svc = 0;
    for (auto it = lo; it != hi; ++it) {
      if (AfterSvc(it->pc)) {
        ++in_svc;
        continue;
      }
      pcs.push_back(it->pc);
      lrs.push_back(it->lr);
    }
    std::fprintf(f, "   %zu samples, %.0f%% waiting in the kernel\n", n,
                 double(in_svc) * 100.0 / double(n));
    const auto pc_hist = Histogram(pcs);
    const size_t busy = pcs.size();
    for (size_t k = 0; k < pc_hist.size() && k < (active ? 80u : 3u); ++k) {
      std::fprintf(f, "   %5.1f%%  pc ", double(pc_hist[k].n) * 100.0 / double(busy));
      PrintAddr(f, pc_hist[k].addr);
      std::fputc('\n', f);
    }
    const auto lr_hist = Histogram(lrs);
    for (size_t k = 0; k < lr_hist.size() && k < (active ? 40u : 3u); ++k) {
      std::fprintf(f, "   %5.1f%%  lr ", double(lr_hist[k].n) * 100.0 / double(busy));
      PrintAddr(f, lr_hist[k].addr);
      std::fputc('\n', f);
    }

    // Full stacks, waits included: they say what the thread is waiting for.
    std::vector<const Sample*> by_stack;
    for (auto it = lo; it != hi; ++it) {
      by_stack.push_back(&*it);
    }
    std::sort(by_stack.begin(), by_stack.end(), [](const Sample* a, const Sample* b) {
      return std::memcmp(a->frames, b->frames, sizeof(a->frames)) < 0;
    });
    struct StackCount {
      const Sample* s;
      u32 n;
    };
    std::vector<StackCount> stacks;
    for (const Sample* s : by_stack) {
      if (stacks.empty() ||
          std::memcmp(stacks.back().s->frames, s->frames, sizeof(s->frames)) != 0) {
        stacks.push_back({s, 1});
      } else {
        ++stacks.back().n;
      }
    }
    std::sort(stacks.begin(), stacks.end(),
              [](const StackCount& a, const StackCount& b) { return a.n > b.n; });
    for (size_t k = 0; k < stacks.size() && k < (active ? 30u : 3u); ++k) {
      std::fprintf(f, "   %5.1f%%  stack", double(stacks[k].n) * 100.0 / double(n));
      for (size_t j = 0; j < kFrames && stacks[k].s->frames[j]; ++j) {
        std::fputs(j == 0 ? " " : " <- ", f);
        PrintAddr(f, stacks[k].s->frames[j]);
      }
      std::fputc('\n', f);
    }
    // Kernel waits (svc) grouped by their first callers only: full stacks of waits are scattered across many
    // distinct stacks and never reach the list above, so a contended lock had no visible owner.
    if (active && in_svc) {
      struct Wait {
        u64 frames[5];
        u32 n;
      };
      std::vector<Wait> waits;
      for (auto it = lo; it != hi; ++it) {
        if (!AfterSvc(it->pc)) continue;
        Wait e{};
        for (size_t j = 0; j < 5 && j < kFrames; ++j) e.frames[j] = it->frames[j];
        auto same = std::find_if(waits.begin(), waits.end(), [&](const Wait& o) {
          return std::memcmp(o.frames, e.frames, sizeof(e.frames)) == 0;
        });
        if (same == waits.end()) {
          e.n = 1;
          waits.push_back(e);
        } else {
          ++same->n;
        }
      }
      std::sort(waits.begin(), waits.end(), [](const Wait& a, const Wait& b) { return a.n > b.n; });
      for (size_t k = 0; k < waits.size() && k < 12; ++k) {
        std::fprintf(f, "   %5.1f%%  wait", double(waits[k].n) * 100.0 / double(n));
        for (size_t j = 0; j < 5 && waits[k].frames[j]; ++j) {
          std::fputs(j == 0 ? " " : " <- ", f);
          PrintAddr(f, waits[k].frames[j]);
        }
        std::fputc('\n', f);
      }
    }
  }
  std::fputc('\n', f);
  std::fclose(f);
}

/*
 * The one-second tick for the console overlays, separate from the profile loop.
 *
 * Two loops use it: the warm-up one (from second zero) and the normal one (from kStartDelayNs on).
 * When it lived only in the normal loop there was neither FPS nor resolution until 8 seconds had
 * passed: they appeared with the EA logo instead of with the first splash screen, which comes much
 * earlier.
 *
 * It is cheap: two counter reads and a few bytes in shared memory. Update also re-attaches the
 * block if it is missing, so the overlay can be opened at any time.
 */
struct TicOverlay {
  u64 tic = 0;
  u64 presented = 0;
  double media[10] = {};
  size_t pos = 0;

  void Boot(u64 now) {
    tic = now;
    presented = g_counters[0].load(std::memory_order_relaxed);
    // Publish what is known right away: the resolution is valid from the first instant (g_resolution
    // starts at 1280x720 and the game corrects it when choosing the video mode), so the overlay's RES
    // row can show up even before the first frame.
    const uint32_t res = g_resolution.load(std::memory_order_relaxed);
    rex::ui::switch_saltynx::Update(0.0, 0.0, res >> 16, res & 0xFFFF, presented);
  }

  // Once per second. `with_clocks` turns off the Reverse-NX and clock sysmodule part, which is better
  // left alone during warm-up.
  void Step(u64 now, u64 freq, bool with_clocks) {
    if (now - tic < freq) {
      return;
    }
    const double seconds = double(now - tic) / double(freq);
    const u64 presented_now = g_counters[0].load(std::memory_order_relaxed);
    const double fps = seconds > 0.0 ? double(presented_now - presented) / seconds : 0.0;
    media[pos] = fps;
    pos = (pos + 1) % 10;
    double sum = 0.0;
    size_t how_many = 0;
    for (double v : media) {
      if (v > 0.0) {
        sum += v;
        ++how_many;
      }
    }
    const uint32_t res = g_resolution.load(std::memory_order_relaxed);
    rex::ui::switch_saltynx::Update(fps, how_many ? sum / double(how_many) : fps, res >> 16, res & 0xFFFF,
                                        presented_now);
    // Reverse-NX is also told that the game keeps asking for the mode (its overlay requires it to show
    // the controls) and, if the system rules, the real mode is mirrored.
    // And if requested, the clock sysmodule is asked for its docked clocks.
    if (with_clocks) {
      const bool docked_real = appletGetOperationMode() == AppletOperationMode_Console;
      const bool docked = rex::ui::switch_saltynx::BaseMode(docked_real);
      rex::ui::switch_sysclk::FollowMode(docked, docked_real);
    }
    /*
     * The memory clock is watched here too. It is outside the if above on purpose: the
     * system raises the EMC right after we request the GPU profile, that is, during
     * warm-up, which is when with_clocks is false. It is one clkrst read per second for
     * the first 20 and one every 30 after that; when we have not changed anything it
     * does not even do that.
     */
    RexSwitchApmWatch();
    presented = presented_now;
    tic = now;
  }
};

void ProfilerMain(void*) {
  const u64 tick_freq = armGetSystemTickFreq();

  /*
   * The overlay first, without waiting for kStartDelayNs. Those 8 seconds are for stack sampling and
   * the first report (which read the SD and pause threads), not for this: publishing FPS and
   * resolution is a few bytes in shared memory, and it has to show already at the first splash screen.
   * During warm-up the tick keeps running every second, so attaching the block is also retried if
   * SaltyNX is slow.
   */
  TicOverlay overlay;
  const u64 start = armGetSystemTick();
  overlay.Boot(start);
  const u64 warmup = (kStartDelayNs / 1000000ULL) * tick_freq / 1000ULL;
  while (armGetSystemTick() - start < warmup) {
    __real_svcSleepThread(kPassiveIntervalNs);
    overlay.Step(armGetSystemTick(), tick_freq, false);
  }

  // Invasive sampling is only enabled explicitly, for diagnostics.
  // The normal mode keeps FPS, CPU and counters, without stopping the game.
  bool sample_stacks = false;
  if (FILE* flag = std::fopen(StackFlagPath().c_str(), "r")) {
    sample_stacks = true;
    std::fclose(flag);
  }

  u64 counters_last[kCounterCount]{};
  for (unsigned i = 0; i < kCounterCount; ++i) {
    counters_last[i] = g_counters[i].load(std::memory_order_relaxed);
  }
  if (FILE* f = std::fopen(ReportPath().c_str(), "w")) {
    std::fprintf(f, "image 0x%016" PRIx64 ", code up to 0x%016" PRIx64 "\n\n", g_base,
                 g_text_hi);
    std::fprintf(f, "Stack sampling: %s\n", sample_stacks ? "active (1 ms)" : "disabled");
    std::fclose(f);
  }

  // The console overlays read from SaltyNX's shared memory. That is already running from second
  // zero (see the warm-up loop above); only the normal loop continues here.

  // A short first report just to set the starting CPU time.
  u64 report_start = armGetSystemTick();
  Report(1, tick_freq, counters_last, "startup");
  report_start = armGetSystemTick();

  size_t rr = 0;
  size_t mode_index = 0;
  while (true) {
    __real_svcSleepThread(sample_stacks ? kSampleIntervalNs : kPassiveIntervalNs);

    for (size_t tries = 0; sample_stacks && tries < kMaxThreads; ++tries) {
      const size_t idx = rr;
      rr = (rr + 1) % kMaxThreads;
      Slot& s = g_slots[idx];
      const u32 h = s.handle.load();
      if (!h || !s.busy) {
        continue;
      }
      if (g_pause_busy.exchange(true, std::memory_order_acquire)) {
        break;  // a hang snapshot is pausing threads right now
      }
      if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) {
        g_pause_busy.store(false, std::memory_order_release);
        break;
      }
      ThreadContext ctx;
      const Result rc = svcGetThreadContext3(&ctx, h);
      if (R_SUCCEEDED(rc) && g_sample_count < kMaxSamples) {
        Sample& out = g_samples[g_sample_count++];
        out = {};
        out.pc = ctx.pc.x;
        out.lr = ctx.lr;
        out.slot = static_cast<u16>(idx);
        out.tick = armGetSystemTick();
        // The stack, with the thread still paused: it is our own memory and
        // there are no locks. Only frames within the region of its sp.
        MemoryInfo smi;
        u32 spi = 0;
        if (R_SUCCEEDED(svcQueryMemory(&smi, &spi, ctx.sp)) && (smi.perm & Perm_R)) {
          const u64 lo = smi.addr, hi = smi.addr + smi.size;
          u64 fp = ctx.fp;
          for (size_t k = 0; k < kFrames && fp >= lo && fp + 16 <= hi && (fp & 7) == 0; ++k) {
            out.frames[k] = reinterpret_cast<const u64*>(fp)[1];
            const u64 next = reinterpret_cast<const u64*>(fp)[0];
            if (next <= fp) {
              break;
            }
            fp = next;
          }
        }
      }
      svcSetThreadActivity(h, ThreadActivity_Runnable);
      g_pause_busy.store(false, std::memory_order_release);
      break;
    }

    const u64 now = armGetSystemTick();
    // Once per second, FPS and resolution for the overlay (the profile report runs every 10 s).
    overlay.Step(now, tick_freq, true);
    if (now - report_start >= kReportSeconds * tick_freq) {
      Report(now - report_start, tick_freq, counters_last, kModes[mode_index].name);
      if (g_ab_active.load(std::memory_order_relaxed)) {
        mode_index = (mode_index + 1) % kModeCount;
        g_skip_mask.store(kModes[mode_index].mask, std::memory_order_relaxed);
        if (mode_index == 0) {
          // Full cycle: it turns itself off and the game looks right again.
          g_ab_active.store(false, std::memory_order_relaxed);
        }
      }
      report_start = armGetSystemTick();
    }
  }
}

/* Priority 102: after switch_crash_hooks.c, before the SDK. */
__attribute__((constructor(102))) void StartProfiler() {
  g_base = reinterpret_cast<u64>(&_start);
  MemoryInfo mi;
  u32 page_info = 0;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &page_info, g_base))) {
    g_text_lo = mi.addr;
    g_text_hi = mi.addr + mi.size;
  }
  Register(envGetMainThreadHandle());
  std::strncpy(g_slots[0].name, "main (UI)", sizeof(g_slots[0].name) - 1);
  // 0x2A: above everything in the game (audio runs at 0x2B), so the samples
  // are taken on time. It sleeps almost all the time.
  if (R_SUCCEEDED(__real_threadCreate(&g_thread, ProfilerMain, nullptr, nullptr, 0x10000, 0x2A,
                                      -2))) {
    threadStart(&g_thread);
  }
}

}  // namespace

extern "C" {

Result __wrap_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem,
                           size_t stack_sz, int prio, int cpuid) {
  const Result rc = __real_threadCreate(t, entry, arg, stack_mem, stack_sz, prio, cpuid);
  if (R_SUCCEEDED(rc) && t) {
    Register(t->handle);
  }
  return rc;
}

Result __wrap_threadClose(Thread* t) {
  if (t) {
    Unregister(t->handle);
  }
  return __real_threadClose(t);
}

/* libnx calls that cost IPC or walk memory. See g_calls. */
Result __wrap_nvFenceWait(NvFence* f, s32 timeout_us) {
  const u64 from = armGetSystemTick();
  const Result rc = __real_nvFenceWait(f, timeout_us);
  Note(timeout_us == 0 ? kFenceQuery : kFenceWait, from);
  return rc;
}

Result __wrap_nvGpuChannelKickoff(NvGpuChannel* c) {
  const u64 from = armGetSystemTick();
  const Result rc = __real_nvGpuChannelKickoff(c);
  Note(kKickoff, from);
  return rc;
}

Result __wrap_nvMapCreate(NvMap* m, void* cpu_addr, u32 size, u32 align, NvKind kind, bool is_cpu_cacheable) {
  const u64 from = armGetSystemTick();
  const Result rc = __real_nvMapCreate(m, cpu_addr, size, align, kind, is_cpu_cacheable);
  Note(is_cpu_cacheable ? kNvMapCached : kNvMapNoCache, from, size);
  return rc;
}

Result __wrap_nvAddressSpaceAllocFixed(NvAddressSpace* a, bool sparse, u64 size, iova_t iova) {
  const u64 from = armGetSystemTick();
  const Result rc = __real_nvAddressSpaceAllocFixed(a, sparse, size, iova);
  Note(kReserveAddress, from, size);
  return rc;
}

Result __wrap_nvioctlNvhostAsGpu_MapBufferEx(u32 fd, u32 flags, u32 kind, u32 nvmap_handle, u32 page_size,
                                             u64 buffer_offset, u64 mapping_size, u64 input_offset, u64* offset) {
  const u64 from = armGetSystemTick();
  const Result rc = __real_nvioctlNvhostAsGpu_MapBufferEx(fd, flags, kind, nvmap_handle, page_size, buffer_offset,
                                                          mapping_size, input_offset, offset);
  Note(kMappingGpu, from, mapping_size);
  return rc;
}

void __wrap_armDCacheClean(void* addr, size_t size) {
  const u64 from = armGetSystemTick();
  __real_armDCacheClean(addr, size);
  Note(kCacheClean, from, size);
}

/*
 * Failed window calls (NWindow and its BufferQueue), kept for the presenter's log.
 *
 * The Mesa WSI (wsi_common_switch.c) turns any failed Binder call of the swapchain into
 * VK_ERROR_SURFACE_LOST_KHR or VK_ERROR_OUT_OF_DATE_KHR and drops the libnx result. Worse, a failure it
 * cannot prove harmless "poisons" the NWindow for the rest of the process: every later swapchain on it is
 * refused, its buffers stay registered, and the screen keeps the last queued image forever (the frozen
 * intro-movie frame at start, 2026-10-09 launch loop: "nwindowSetDimensions(1280x720) failed: 0x00000F59",
 * that is LibnxError_AlreadyInitialized, slots still configured). The wrappers below only record which call
 * failed, its result, the slot and NWindow::cur_slot, so the presenter can print them when presentation is
 * lost. No lock, no I/O: a fixed ring, read by RexSwitchWindowFailures.
 */
enum : u32 {
  kWinQueueBuffer = 1,
  kWinDequeueBuffer,
  kWinCancelBuffer,
  kWinReleaseBuffers,
  kWinConfigureBuffer,
  kWinRequestBuffer,
  kWinBqCancelBuffer,
};
struct WindowFailure {
  std::atomic<u32> seq{0};  // written last (release); 0 = empty
  std::atomic<u32> call{0};
  std::atomic<u32> rc{0};
  std::atomic<s32> slot{0};
  std::atomic<s32> cur_slot{0};
  std::atomic<u64> tick{0};
};
constexpr u32 kWindowFailures = 16;
static WindowFailure g_window_failures[kWindowFailures];
static std::atomic<u32> g_window_failure_count{0};
static std::atomic<u32> g_window_failure_reported{0};

static void NoteWindowFailure(u32 call, Result rc, s32 slot, const NWindow* nw) {
  const u32 n = g_window_failure_count.fetch_add(1, std::memory_order_relaxed) + 1;
  WindowFailure& f = g_window_failures[(n - 1) % kWindowFailures];
  f.call.store(call, std::memory_order_relaxed);
  f.rc.store(u32(rc), std::memory_order_relaxed);
  f.slot.store(slot, std::memory_order_relaxed);
  f.cur_slot.store(nw ? nw->cur_slot : -2, std::memory_order_relaxed);
  f.tick.store(armGetSystemTick(), std::memory_order_relaxed);
  f.seq.store(n, std::memory_order_release);
}

static const char* WindowCallName(u32 call) {
  switch (call) {
    case kWinQueueBuffer:
      return "nwindowQueueBuffer";
    case kWinDequeueBuffer:
      return "bqDequeueBuffer";
    case kWinCancelBuffer:
      return "nwindowCancelBuffer";
    case kWinReleaseBuffers:
      return "nwindowReleaseBuffers";
    case kWinConfigureBuffer:
      return "nwindowConfigureBuffer";
    case kWinRequestBuffer:
      return "bqRequestBuffer";
    case kWinBqCancelBuffer:
      return "bqCancelBuffer";
    default:
      return "?";
  }
}

/*
 * The presentation interval (masseffect_interval_swap).
 *
 * It is reapplied on every present, not when the chain is created, on purpose: the WSI sets it to 1
 * when creating the swapchain, and the chain is recreated when switching from docked to handheld.
 * With 0 nothing is touched and the swapchain mode rules (IMMEDIATE -> interval 0).
 *
 * libnx only accepts interval 0 if the NWindow has three or more buffers; the Horizon WSI always
 * configures exactly 3, so the condition holds. 2 is always accepted.
 *
 * Warning: it is 0 and must stay that way. Measured: with the current frame time, setting 2 sends
 * 22 % of the frames to 66.7 ms. It only makes sense with a median frame time below 31 ms.
 */
std::atomic<unsigned> g_interval_swap{0};

Result __wrap_nwindowQueueBuffer(NWindow* nw, s32 slot, const NvMultiFence* fence) {
  const u64 from = armGetSystemTick();
  const unsigned request = g_interval_swap.load(std::memory_order_relaxed);
  if (request != 0 && nw != nullptr && nw->swap_interval != request) {
    nwindowSetSwapInterval(nw, request);
  }
  const s32 cur = nw ? nw->cur_slot : -2;
  const Result rc = __real_nwindowQueueBuffer(nw, slot, fence);
  Note(kQueueBuffer, from);
  if (R_FAILED(rc)) {
    // cur_slot from before the call: a mismatch with slot means the WSI queued an image it had not dequeued.
    NoteWindowFailure(kWinQueueBuffer, rc, slot, nullptr);
    g_window_failures[(g_window_failure_count.load(std::memory_order_relaxed) - 1) % kWindowFailures]
        .cur_slot.store(cur, std::memory_order_relaxed);
  }
  return rc;
}

Result __wrap_bqDequeueBuffer(Binder* b, bool async, u32 width, u32 height, s32 format, u32 usage, s32* buf,
                              NvMultiFence* fence) {
  const u64 from = armGetSystemTick();
  const Result rc = __real_bqDequeueBuffer(b, async, width, height, format, usage, buf, fence);
  Note(kDequeueBuffer, from);
  // WouldBlock is the normal answer of an asynchronous dequeue with no free buffer.
  if (R_FAILED(rc) && R_VALUE(rc) != MAKERESULT(Module_LibnxBinder, LibnxBinderError_WouldBlock)) {
    NoteWindowFailure(kWinDequeueBuffer, rc, -1, nullptr);
  }
  return rc;
}

/*
 * Formats the window failures recorded since the previous call (oldest first, at most the ring size) into
 * buf and returns how many there were in total since the start (0 = never). Any thread; meant for the
 * presenter when it loses the swapchain.
 */
u32 RexSwitchWindowFailures(char* buf, size_t cap) {
  if (buf && cap) {
    buf[0] = 0;
  }
  const u32 total = g_window_failure_count.load(std::memory_order_acquire);
  u32 from = g_window_failure_reported.exchange(total, std::memory_order_relaxed);
  if (total > from + kWindowFailures) {
    from = total - kWindowFailures;
  }
  size_t used = 0;
  for (u32 n = from + 1; n <= total && buf && used + 1 < cap; ++n) {
    const WindowFailure& f = g_window_failures[(n - 1) % kWindowFailures];
    if (f.seq.load(std::memory_order_acquire) != n) {
      continue;  // overwritten or still being written
    }
    const u32 rc = f.rc.load(std::memory_order_relaxed);
    const int w = std::snprintf(buf + used, cap - used,
                                "%s[#%u %s rc 0x%08X (module %u, description %u) slot %d cur_slot %d at %.3f s]",
                                used ? " " : "", n, WindowCallName(f.call.load(std::memory_order_relaxed)), rc,
                                rc & 0x1FF, (rc >> 9) & 0x1FFF, f.slot.load(std::memory_order_relaxed),
                                f.cur_slot.load(std::memory_order_relaxed),
                                double(armTicksToNs(f.tick.load(std::memory_order_relaxed))) / 1e9);
    if (w <= 0) {
      break;
    }
    used = std::min(cap - 1, used + size_t(w));
  }
  return total;
}

Result __wrap_nwindowCancelBuffer(NWindow* nw, s32 slot, const NvMultiFence* fence) {
  const s32 cur = nw ? nw->cur_slot : -2;
  const Result rc = __real_nwindowCancelBuffer(nw, slot, fence);
  if (R_FAILED(rc)) {
    NoteWindowFailure(kWinCancelBuffer, rc, slot, nullptr);
    g_window_failures[(g_window_failure_count.load(std::memory_order_relaxed) - 1) % kWindowFailures]
        .cur_slot.store(cur, std::memory_order_relaxed);
  }
  return rc;
}

Result __wrap_nwindowReleaseBuffers(NWindow* nw) {
  const Result rc = __real_nwindowReleaseBuffers(nw);
  if (R_FAILED(rc)) {
    NoteWindowFailure(kWinReleaseBuffers, rc, -1, nw);
  }
  return rc;
}

Result __wrap_nwindowConfigureBuffer(NWindow* nw, s32 slot, NvGraphicBuffer* buf) {
  const Result rc = __real_nwindowConfigureBuffer(nw, slot, buf);
  if (R_FAILED(rc)) {
    NoteWindowFailure(kWinConfigureBuffer, rc, slot, nw);
  }
  return rc;
}

Result __wrap_bqRequestBuffer(Binder* b, s32 bufferIdx, BqGraphicBuffer* buf) {
  const Result rc = __real_bqRequestBuffer(b, bufferIdx, buf);
  if (R_FAILED(rc)) {
    NoteWindowFailure(kWinRequestBuffer, rc, bufferIdx, nullptr);
  }
  return rc;
}

Result __wrap_bqCancelBuffer(Binder* b, s32 buf, const NvMultiFence* fence) {
  const Result rc = __real_bqCancelBuffer(b, buf, fence);
  if (R_FAILED(rc)) {
    NoteWindowFailure(kWinBqCancelBuffer, rc, buf, nullptr);
  }
  return rc;
}

void __wrap_svcSleepThread(s64 nano) {
  const u64 from = armGetSystemTick();
  __real_svcSleepThread(nano);
  Note(nano == 0 ? kSleep0 : nano < 0 ? kSleepYield : nano <= 1000000 ? kSleepShort : kSleepLong, from);
}

/*
 * Counters: 0 = guest present, 1 = read emulated through the shadow,
 * 2 = fault resolved by an SDK handler, 3 = retry emulated through the
 * shadow, 4 = SEH. 28..32 = draws per surface width from IssueDraw
 * (1600 or more, 1280-1599, 640-1279, 256-639, under 256).
 */
// A long frame, in armGetSystemTick ticks (see WindowHitch).
void RexSwitchPerfHitch(u64 start, u64 end) {
  const u32 i = g_num_hitches.fetch_add(1);
  if (i < kMaxHitches) {
    g_hitches[i] = {start, end};
  }
}

void RexSwitchPerfExclusiveCore(int core) {
  // 10 + K: core K, light guest threads allowed on it too.
  // 10 + K: light guest threads allowed on K; 20 + K: the render thread raised above the ring thread.
  // Both may be combined: 30 + K.
  if (core < 0) return;
  g_core_render.store(core >= 20, std::memory_order_relaxed);
  g_light_core.store((core % 20) >= 10, std::memory_order_relaxed);
  g_exclusive_core.store(core % 10, std::memory_order_relaxed);
}

void RexSwitchPerfResolution(unsigned w, unsigned h) {
  g_resolution.store(((w & 0xFFFF) << 16) | (h & 0xFFFF), std::memory_order_relaxed);
}

// The label written under the next report blocks (see g_labels); any thread, cheap (no lock, no I/O).
void RexSwitchPerfSetLabel(const char* label) {
  const uint32_t seq = g_label_seq.load(std::memory_order_relaxed) + 1;
  char* dst = g_labels[seq & 1];
  std::strncpy(dst, label ? label : "", kLabelBytes - 1);
  dst[kLabelBytes - 1] = 0;
  g_label_seq.store(seq, std::memory_order_release);
}

// Report blocks written so far (the block being measured now is this + 1).
uint32_t RexSwitchPerfReportIndex(void) {
  return g_report_index.load(std::memory_order_relaxed);
}

// See g_interval_swap, next to the nwindowQueueBuffer wrapper. 0 = touch nothing (the normal case).
void RexSwitchPerfIntervalSwap(unsigned vblanks) {
  g_interval_swap.store(vblanks, std::memory_order_relaxed);
}

void RexSwitchPerfCount(unsigned id) {
  if (id < kCounterCount) {
    g_counters[id].fetch_add(1, std::memory_order_relaxed);
  }
  if (id == 0) {
    // Counter 0 is presents. The console overlay clears the "alive" mark and the resolution mark and
    // only waits 100 ms, so it has to be answered on every frame, not once per second. It is writing a
    // few bytes to shared memory.
    const uint32_t res = g_resolution.load(std::memory_order_relaxed);
    rex::ui::switch_saltynx::Beat(res >> 16, res & 0xFFFF);
  }
}

/*
 * 5 draws, 6 scissor area of the draws (pixels), 7 queue submissions,
 * 9 resolves, 10 resolve area, 11 render target transfer calls,
 * 12 render targets transferred, 13 transfer draws, 14 textures loaded,
 * 15 shared memory bytes uploaded, 16 pipelines created, 17 physical memory
 * chunks committed on touch, 18 chunks mapped into a view on touch,
 * 21 clipped audio samples, 22 audio buffers with a peak of 0.98 or more.
 */
void RexSwitchPerfAdd(unsigned id, u64 value) {
  if (id < kCounterCount) {
    g_counters[id].fetch_add(value, std::memory_order_relaxed);
  }
}

/* max counter (23 = audio mix peak in ten-thousandths; the report resets it to 0). */
void RexSwitchPerfMax(unsigned id, u64 value) {
  if (id >= kCounterCount) {
    return;
  }
  u64 actual = g_counters[id].load(std::memory_order_relaxed);
  while (value > actual && !g_counters[id].compare_exchange_weak(actual, value, std::memory_order_relaxed)) {
  }
}

/*
 * Bits: 1 draws, 2 transfers, 4 resolves, 8 texture loads,
 * 16 presentation effect, 32 tiled dispatch.
 */
bool RexSwitchPerfSkip(unsigned bit) {
  return (g_skip_mask.load(std::memory_order_relaxed) & bit) != 0;
}

// Called by every guest thread when it starts (xthread.cpp): its guest start routine and argument, for the report.
void RexSwitchPerfSetCurrentGuestEntry(u32 entry, u32 context) {
  const u32 self = threadGetCurHandle();
  for (auto& s : g_slots) {
    if (s.handle.load() == self) {
      s.guest_context.store(context, std::memory_order_relaxed);
      s.guest_entry.store(entry, std::memory_order_relaxed);
      return;
    }
  }
}

// Role marks from the game hooks (ApplyExclusiveCore): 0 = the calling thread ran UGameEngine::Tick (main thread),
// 1 = it called the guest D3D Swap (render thread). Cheap enough for once per frame: a cached slot and one relaxed add.
void RexSwitchPerfNoteThreadRole(unsigned role) {
  const u32 self = threadGetCurHandle();
  if (role == 0) {
    if (g_game_thread.load(std::memory_order_relaxed) != self) {
      g_game_thread.store(self, std::memory_order_relaxed);
    }
    return;
  }
  static thread_local Slot* slot = nullptr;
  if (!slot || slot->handle.load(std::memory_order_relaxed) != self) {
    slot = nullptr;
    for (auto& s : g_slots) {
      if (s.handle.load(std::memory_order_relaxed) == self) {
        slot = &s;
        break;
      }
    }
    if (!slot) return;
  }
  slot->swaps.fetch_add(1, std::memory_order_relaxed);
}

void RexSwitchPerfSetThreadName(u32 handle, const char* name) {
  if (!handle || !name) {
    return;
  }
  for (auto& s : g_slots) {
    if (s.handle.load() == handle) {
      char tmp[sizeof(s.name)]{};
      std::strncpy(tmp, name, sizeof(tmp) - 1);
      std::memcpy(s.name, tmp, sizeof(tmp));
      return;
    }
  }
}

/*
 * Hang diagnostics (switch_thread_snapshot.h): every registered thread, paused one at a time. Between pause and
 * resume only system calls and stores into the caller's buffer run (no malloc, no stdio, no locks).
 */
size_t RexSwitchSnapshotThreads(RexSwitchThreadSnapshot* out, size_t max) {
  if (!out || !max) {
    return 0;
  }
  const u32 self = threadGetCurHandle();
  while (g_pause_busy.exchange(true, std::memory_order_acquire)) {
    __real_svcSleepThread(100000);  // the sampler holds it for a few microseconds
  }
  size_t n = 0;
  for (size_t idx = 0; idx < kMaxThreads && n < max; ++idx) {
    Slot& s = g_slots[idx];
    const u32 h = s.handle.load();
    if (!h) {
      continue;
    }
    RexSwitchThreadSnapshot& o = out[n++];
    std::memset(&o, 0, sizeof(o));
    o.handle = h;
    std::memcpy(o.name, s.name, sizeof(o.name));
    o.name[sizeof(o.name) - 1] = 0;
    if (h == self) {
      o.state = kRexSwitchSnapshotSelf;
      continue;
    }
    if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) {
      o.state = kRexSwitchSnapshotPauseFailed;
      continue;
    }
    ThreadContext ctx;
    if (R_FAILED(svcGetThreadContext3(&ctx, h))) {
      o.state = kRexSwitchSnapshotContextFailed;
    } else {
      o.state = kRexSwitchSnapshotOk;
      o.pc = ctx.pc.x;
      o.lr = ctx.lr;
      o.sp = ctx.sp;
      o.fp = ctx.fp;
      o.in_kernel = AfterSvc(ctx.pc.x) ? 1 : 0;
      // Same walk as the sampler: only frames inside the memory region of sp.
      MemoryInfo smi;
      u32 spi = 0;
      if (R_SUCCEEDED(svcQueryMemory(&smi, &spi, ctx.sp)) && (smi.perm & Perm_R)) {
        const u64 lo = smi.addr, hi = smi.addr + smi.size;
        u64 fp = ctx.fp;
        for (u32 k = 0; k < REX_SWITCH_SNAPSHOT_FRAMES && fp >= lo && fp + 16 <= hi && (fp & 7) == 0; ++k) {
          o.frames[k] = reinterpret_cast<const u64*>(fp)[1];
          o.frame_count = k + 1;
          const u64 next = reinterpret_cast<const u64*>(fp)[0];
          if (next <= fp) {
            break;
          }
          fp = next;
        }
      }
    }
    svcSetThreadActivity(h, ThreadActivity_Runnable);
  }
  g_pause_busy.store(false, std::memory_order_release);
  return n;
}

uint64_t RexSwitchImageBase(void) { return g_base; }
uint64_t RexSwitchImageTextEnd(void) { return g_text_hi; }

}  // extern "C"

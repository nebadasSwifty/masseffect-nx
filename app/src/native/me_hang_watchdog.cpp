// Mass Effect - hang watchdog: one dump of where every thread is when the game stops drawing.
//
// WHY
//   About 1 start in 3 on the console plays the logo movies, loads EntryMenu (sometimes GlobalTlk and
//   BIOG_UIWorld too) and then presents ~30 Swaps per second with 0 draws forever: the title screen never
//   comes. The log cannot tell which guest thread is stuck and on what.
//
// WHAT IT DOES (off unless masseffect_hang_watchdog = true)
//   Trigger, checked on the ring thread at every 10-second renderer report (HangWatchdogInterval):
//   masseffect_hang_watchdog_intervals consecutive intervals with Swaps but 0 draws, after at least one interval
//   with draws. Fires once per run. Also, at any time: the flag file <NRO folder>/logs/rex/hang_dump.flag
//   (polled every 2 s; deleted after each dump, so creating it again gives another dump).
//   The dump runs on its own thread and writes "[hang]" lines to the log:
//     1. host threads (Switch): every thread is paused in turn, its pc/lr and the return addresses of its x29
//        frame chain are copied, and it is resumed (RexSwitchSnapshotThreads in sdk/src/ui/switch_perf.cpp,
//        same walk as the stack profiler). This is the reliable call stack: generated functions are host
//        functions named __imp__sub_<guest address>.
//     2. guest threads (XThread objects): handle, thread id, name, priority, the host thread with the same name,
//        guest r1/lr/r3-r6 from the PPCContext, and the guest back chain walked from r1 (word at r1 = caller's
//        r1, saved LR at caller's r1 - 8).
//   Caveat on the guest LR: the generated code does not update ctx.lr on `bl` (only mtlr writes it), so a saved
//   LR is whatever ctx.lr held when the function was entered, usually the return address of the previous call
//   the caller made: it points into the caller (right function, earlier call site) more often than not, but it
//   is a hint. The guest pc is not tracked at all. Trust the host frames.
//
// SYMBOLIZING
//   Host frames print as image+0x<offset> (offset from the executable's base, as in rex_profile.log), followed
//   by "~sub_XXXXXXXX+0x.." when a generated function starts at most 64 KiB below it (nearest-start guess
//   from PPCFuncMappings; frames in the SDK, libc or Mesa can be mislabeled by it). Exact names: addr2line on
//   the ELF of the same build, e.g.
//     docker run --rm -v "$PWD":/w devkitpro/devkita64:latest sh -c \
//       '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line -f -C -e /w/<elf> 0x<offset> ...'
//   (the ELF is the build output next to the .nro; offsets are relative to the image base, which addr2line on
//   the ELF expects). __imp__sub_82ABCDEF is the recompiled guest function at 0x82ABCDEF.
//   Guest addresses print as 82xxxxxx (sub_XXXXXXXX+0x..): the containing function is the generated function
//   with the largest start <= address (PPCFuncMappings in app/generated/default/masseffect_init.cpp, sorted),
//   i.e. `grep -n "sub_8xxxxxxx" app/generated/default/masseffect_init.cpp`, and the body is
//   DEFINE_REX_FUNC(sub_XXXXXXXX) in app/generated/default/masseffect_recomp.*.cpp, with the PowerPC
//   instruction as a comment above each line.

#include "me_native_system.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if REX_PLATFORM_SWITCH
#include <pthread.h>

#include <rex/ui/switch_thread_snapshot.h>
extern "C" const char* RexSwitchLogDir(void);  // <NRO folder>/logs/rex/, ends in '/'
#endif

REXCVAR_DEFINE_BOOL(masseffect_hang_watchdog, false, "Mass Effect",
                    "Diagnostics: when the renderer presents Swaps without draws for "
                    "masseffect_hang_watchdog_intervals consecutive 10-second reports (after having drawn), or when "
                    "logs/rex/hang_dump.flag exists, write every host and guest thread's call stack to the log once")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(masseffect_hang_peek, "", "Mass Effect",
                      "Hang watchdog: guest words to print in the dump, comma separated hex: ADDR, or *ADDR+OFF to "
                      "follow the pointer at ADDR and read pointer+OFF")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_hang_watchdog_intervals, 3, "Mass Effect",
                     "Hang watchdog: consecutive 10-second renderer reports with Swaps but 0 draws that trigger the "
                     "dump")
    .range(1, 60)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace me::native {
namespace {

constexpr uint32_t kGuestFrames = 32;
constexpr uint32_t kGuestStackLo = rex::system::XThread::kStackAddressRangeBegin;
constexpr uint32_t kGuestStackHi = rex::system::XThread::kStackAddressRangeEnd;
constexpr uint32_t kMaxGuestFrameSize = 1u << 20;

std::atomic<bool> g_started{false};
std::atomic<bool> g_requested{false};
std::atomic<uint32_t> g_quiet_intervals{0};
std::atomic<uint64_t> g_quiet_swaps{0};
bool g_seen_draws = false;  // ring thread only
bool g_auto_done = false;   // ring thread only
std::chrono::steady_clock::time_point g_start_time;

// Guest function containing a guest address: the largest PPCFuncMappings start <= address.
size_t g_mapping_count = 0;
std::string GuestName(uint32_t address) {
  if (!g_mapping_count) {
    while (PPCFuncMappings[g_mapping_count].guest) ++g_mapping_count;
  }
  const PPCFuncMapping* first = PPCFuncMappings;
  const PPCFuncMapping* last = PPCFuncMappings + g_mapping_count;
  const PPCFuncMapping* it = std::upper_bound(first, last, size_t(address),
                                              [](size_t a, const PPCFuncMapping& m) { return a < m.guest; });
  if (it == first) return "?";
  --it;
  return fmt::format("sub_{:08X}+0x{:X}", uint32_t(it->guest), address - uint32_t(it->guest));
}

#if REX_PLATFORM_SWITCH
// Host start address of each generated function, sorted, for the "~sub_" hint on host frames.
std::vector<std::pair<uint64_t, uint32_t>> g_host_starts;

std::string HostAddress(uint64_t address) {
  const uint64_t base = RexSwitchImageBase();
  const uint64_t text_end = RexSwitchImageTextEnd();
  if (address < base || address >= text_end) return fmt::format("0x{:016X}", address);
  std::string out = fmt::format("image+0x{:X}", address - base);
  auto it = std::upper_bound(g_host_starts.begin(), g_host_starts.end(), std::make_pair(address, UINT32_MAX));
  if (it != g_host_starts.begin()) {
    --it;
    if (address - it->first < 0x10000) out += fmt::format(" (~sub_{:08X}+0x{:X})", it->second, address - it->first);
  }
  return out;
}
#endif

// Reads a guest word only if its host page is mapped and readable (a new thread's stack top is not, and an
// unchecked read there killed the process in the middle of a dump). Returns false otherwise.
bool TryLoadGuest32(rex::memory::Memory* memory, uint32_t address, uint32_t& value) {
  uint8_t* host = memory->TranslateVirtual<uint8_t*>(address);
  size_t length = 0;
  rex::memory::PageAccess access;
  if (!rex::memory::QueryProtect(host, length, access) ||
      !(uint32_t(access) & uint32_t(rex::memory::PageAccess::kReadOnly)) || length < 4) return false;
  value = rex::memory::load_and_swap<uint32_t>(host);
  return true;
}

uint32_t LoadGuest32(rex::memory::Memory* memory, uint32_t address) {
  uint32_t value = 0;
  TryLoadGuest32(memory, address, value);
  return value;
}

// masseffect_hang_peek: "ADDR" reads the word at ADDR; "*ADDR+OFF" reads the pointer at ADDR, then the word at
// pointer + OFF (hex, comma separated). Example: the D3D device owner thread is *<device global>+2A08 (addresses per edition: docs/do-not-break.md)
void Peek(rex::memory::Memory* memory, const std::string& spec) {
  size_t start = 0;
  while (start < spec.size()) {
    size_t end = spec.find(',', start);
    if (end == std::string::npos) end = spec.size();
    std::string item = spec.substr(start, end - start);
    start = end + 1;
    if (item.empty()) continue;
    const bool deref = item[0] == '*';
    if (deref) item.erase(0, 1);
    const size_t plus = item.find('+');
    const uint32_t address = uint32_t(std::strtoul(item.substr(0, plus).c_str(), nullptr, 16));
    const uint32_t offset = plus == std::string::npos ? 0 : uint32_t(std::strtoul(item.substr(plus + 1).c_str(), nullptr, 16));
    uint32_t value = 0;
    if (!TryLoadGuest32(memory, address, value)) {
      REXLOG_WARN("[hang] peek {}: {:08X} unreadable", item, address);
      continue;
    }
    if (!deref) {
      REXLOG_WARN("[hang] peek {:08X} = {:08X}", address, value);
      continue;
    }
    uint32_t target = 0;
    const bool ok = value && TryLoadGuest32(memory, value + offset, target);
    REXLOG_WARN("[hang] peek *{:08X} = {:08X}, [{:08X}+{:X}] = {}", address, value, value, offset,
                ok ? fmt::format("{:08X}", target) : std::string("unreadable"));
  }
}

void Dump(const char* reason) {
  const double uptime =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start_time).count();
  REXLOG_WARN("[hang] ===== thread dump ({}), {:.0f} s after start =====", reason, uptime);

  // 1. Host threads first: no locks are needed for them, while the guest side below takes the object table lock,
  // which a stuck thread could be holding.
  std::vector<std::pair<std::string, uint32_t>> host_names;  // host thread name -> handle, to pair guest threads
#if REX_PLATFORM_SWITCH
  {
    std::vector<RexSwitchThreadSnapshot> snaps(192);
    const size_t n = RexSwitchSnapshotThreads(snaps.data(), snaps.size());
    if (g_host_starts.empty()) {
      if (!g_mapping_count) GuestName(0);
      g_host_starts.reserve(g_mapping_count);
      for (size_t i = 0; i < g_mapping_count; ++i) {
        g_host_starts.emplace_back(reinterpret_cast<uint64_t>(PPCFuncMappings[i].host),
                                   uint32_t(PPCFuncMappings[i].guest));
      }
      std::sort(g_host_starts.begin(), g_host_starts.end());
    }
    REXLOG_WARN("[hang] {} host threads, image base 0x{:016X} (frames: image+offset, resolve with addr2line on the "
                "ELF of this build; ~sub_ = nearest generated function, a guess)",
                n, RexSwitchImageBase());
    static const char* kStates[] = {"ok", "pause failed (suspended or exiting)", "no context", "the dumping thread"};
    for (size_t i = 0; i < n; ++i) {
      const RexSwitchThreadSnapshot& s = snaps[i];
      host_names.emplace_back(s.name, s.handle);
      if (s.state != kRexSwitchSnapshotOk) {
        REXLOG_WARN("[hang] host {:08X} '{}': {}", s.handle, s.name,
                    kStates[std::clamp(s.state, 0, 3)]);
        continue;
      }
      std::string frames;
      for (uint32_t k = 0; k < s.frame_count; ++k) {
        frames += fmt::format(" #{} {}", k, HostAddress(s.frames[k]));
      }
      REXLOG_WARN("[hang] host {:08X} '{}': {} pc {} lr {} sp 0x{:X} |{}", s.handle, s.name,
                  s.in_kernel ? "IN KERNEL WAIT" : "running", HostAddress(s.pc), HostAddress(s.lr), s.sp, frames);
    }
  }
#endif

  // 2. Guest threads.
  auto* kernel = rex::system::kernel_state();
  if (!kernel) {
    REXLOG_WARN("[hang] no kernel state: guest threads skipped");
    return;
  }
  rex::memory::Memory* memory = kernel->memory();
  auto threads = kernel->object_table()->GetObjectsByType<rex::system::XThread>();
  REXLOG_WARN("[hang] {} guest threads (lr and back-chain LRs are hints: bl does not update ctx.lr in generated "
              "code; the guest pc is not tracked)", threads.size());
  for (auto& t : threads) {
    std::string name = t->thread() ? t->thread()->name() : std::string();
    std::string host = "-";
    for (const auto& [host_name, handle] : host_names) {
      if (host_name.size() >= 8 && name.compare(0, host_name.size(), host_name) == 0) {
        host = fmt::format("{:08X}", handle);
        break;
      }
    }
    rex::runtime::ThreadState* state = t->thread_state();
    if (!state || !state->context()) {
      REXLOG_WARN("[hang] guest {:08X} id {} '{}' host {}: no context", t->handle(), t->thread_id(), name, host);
      continue;
    }
    const PPCContext& ctx = *state->context();
    const uint32_t r1 = ctx.r1.u32;
    const uint32_t lr = uint32_t(ctx.lr);
    std::string chain;
    uint32_t sp = r1;
    for (uint32_t k = 0; k < kGuestFrames; ++k) {
      if (sp < kGuestStackLo || sp >= kGuestStackHi || (sp & 3)) break;
      uint32_t next = 0;
      if (!TryLoadGuest32(memory, sp, next)) break;
      if (next <= sp || next - sp > kMaxGuestFrameSize || next >= kGuestStackHi) break;
      const uint32_t saved_lr = LoadGuest32(memory, next - 8);
      chain += fmt::format(" #{} {:08X}:{:08X} ({})", k, next, saved_lr, GuestName(saved_lr));
      sp = next;
    }
    REXLOG_WARN("[hang] guest {:08X} id {} '{}' host {} prio {}{}{}: r1 {:08X} lr {:08X} ({}) r3 {:08X} r4 {:08X} "
                "r5 {:08X} r6 {:08X} | back chain (frame:saved lr):{}",
                t->handle(), t->thread_id(), name, host, t->priority(), t->is_running() ? "" : " not running",
                t->main_thread() ? " main" : "", r1, lr, GuestName(lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                ctx.r6.u32, chain.empty() ? " (empty: r1 outside the guest stack range)" : chain);
  }
  Peek(memory, REXCVAR_GET(masseffect_hang_peek));
  REXLOG_WARN("[hang] ===== end of thread dump =====");
}

std::string FlagPath() {
#if REX_PLATFORM_SWITCH
  return std::string(RexSwitchLogDir()) + "hang_dump.flag";
#else
  return "hang_dump.flag";
#endif
}

}  // namespace

// No thread of its own: creating one failed on the Switch (std::system_error at start, the game never ran).
// The checks run on the ring thread at every 10-second renderer report (HangWatchdogInterval).
void StartHangWatchdog() {
  if (!REXCVAR_GET(masseffect_hang_watchdog) || g_started.exchange(true)) return;
  g_start_time = std::chrono::steady_clock::now();
  REXLOG_INFO("[hang] watchdog on: dump after {} renderer reports with Swaps and no draws, or when {} exists",
              REXCVAR_GET(masseffect_hang_watchdog_intervals), FlagPath());
}

void HangWatchdogInterval(uint64_t swaps, uint64_t draws) {
  if (!g_started.load(std::memory_order_relaxed)) return;
  {
    static const std::string flag = FlagPath();
    if (std::FILE* f = std::fopen(flag.c_str(), "r")) {
      std::fclose(f);
      std::remove(flag.c_str());
      Dump("flag file");
    }
  }
  if (g_auto_done) return;
  if (draws) {
    g_seen_draws = true;
    g_quiet_intervals.store(0, std::memory_order_relaxed);
    g_quiet_swaps.store(0, std::memory_order_relaxed);
    return;
  }
  if (!swaps || !g_seen_draws) {
    g_quiet_intervals.store(0, std::memory_order_relaxed);
    g_quiet_swaps.store(0, std::memory_order_relaxed);
    return;
  }
  g_quiet_swaps.fetch_add(swaps, std::memory_order_relaxed);
  const uint32_t quiet = g_quiet_intervals.fetch_add(1, std::memory_order_relaxed) + 1;
  if (quiet >= uint32_t(REXCVAR_GET(masseffect_hang_watchdog_intervals))) {
    g_auto_done = true;
    const std::string reason = fmt::format("{} renderer reports in a row with Swaps but no draws ({} Swaps)",
                                           g_quiet_intervals.load(), g_quiet_swaps.load());
    Dump(reason.c_str());
  }
}

}  // namespace me::native

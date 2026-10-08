// Mass Effect - UnrealScript profiler: which script functions the game runs, per map (docs/tour.md).
//
// HOW
//   Hooks on the two entry points of the UE3 script VM (addresses in me_tour_guest.h, one copy per edition):
//   - UObject::CallFunction(Stack, Result, UFunction*): every script-to-script call and every call of a native
//     function that has no fixed bytecode (iNative == 0), i.e. the "native C++ exec functions called from script";
//   - UObject::ProcessEvent(UFunction*, Parms): every call from C++ into script (events: Tick, Touch, timers, ...).
//   Each call is counted per UFunction (key: object address + FName index, so a function unloaded with its package and
//   replaced by another object at the same address is a new entry). The name ("Class.Function", the UFunction's
//   Outer and Name) is resolved once, when the entry is created. With masseffect_script_prof = 2 the profiler also
//   keeps a shadow stack and measures self and inclusive time with armGetSystemTick (cntpct_el0; cntvct_el0 traps on
//   Horizon). Natives with a fixed bytecode (operators, iNative != 0) go straight through GNatives and are not seen.
//
// OUTPUT
//   me::sprof::DumpAndReset(label) (the tour calls it when it leaves a map and at the end; it is also called when
//   GWorld changes) logs "[script_prof] <label>: N calls in S s, top 50 by calls" and, with timing, "top 50 by self
//   time", then starts a new interval.
//
// COST
//   Off (default): one relaxed atomic load and a call through the hook. Counting: one hash lookup per call (only on the
//   game thread; other threads pass through). Timing: two counter reads per call more.

#include "me_tour.h"

#define ME_TOUR_GUEST_DEFINE_HOOKS
#include "me_tour_guest.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

REXCVAR_DEFINE_INT32(masseffect_script_prof, 0, "Mass Effect",
                     "UnrealScript profiler (docs/tour.md): 0 = off, 1 = count calls per script function, 2 = also "
                     "self and inclusive time; tables in the log per map ([script_prof])")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_script_prof_top, 50, "Mass Effect",
                     "UnrealScript profiler: rows per table")
    .range(5, 1000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

namespace g = me::tour_guest;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v = 0;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

// Same range as the tour: functions of the start-up packages are outside the 0x4xxxxxxx heap (first console run).
bool Plausible(uint32_t p) { return p >= 0x00010000u && p < 0xFFFF0000u && (p & 3) == 0; }

std::string ObjectName(const uint8_t* base, uint32_t object) { return me::tour::ObjectNameOf(base, object); }

struct Stat {
  uint64_t calls = 0, self = 0, incl = 0;
  uint32_t via_mask = 0;
  bool native = false;
  std::string name;
};

struct Frame {
  Stat* stat;
  uint64_t start, child;
};

std::atomic<int> g_mode{-1};
thread_local bool t_game_thread = false;
std::unordered_map<uint64_t, Stat> g_stats;
std::vector<Frame> g_stack;
uint64_t g_total_calls = 0;
// Hook health (logged with each table): calls seen per hook on the game thread, and calls passed through unseen.
uint64_t g_hook_counted[2] = {0, 0};
std::atomic<uint64_t> g_hook_other_thread{0}, g_hook_filtered{0};
uint32_t g_world = 0;
std::string g_world_label;
std::chrono::steady_clock::time_point g_interval_start = std::chrono::steady_clock::now();

int Mode() {
  int m = g_mode.load(std::memory_order_relaxed);
  if (m < 0) [[unlikely]] {
    m = REXCVAR_GET(masseffect_script_prof);
    g_mode.store(m, std::memory_order_relaxed);
    if (m) {
      REXLOG_INFO("[script_prof] on: mode {} ({}), {} rows per table, edition {}", m,
                  m == 2 ? "calls and time" : "calls", REXCVAR_GET(masseffect_script_prof_top), g::kEdition);
      g_stats.reserve(16384);
      g_stack.reserve(512);
    }
  }
  return m;
}

Stat* Lookup(uint8_t* base, uint32_t function, me::sprof::Via via) {
  const uint32_t name_index = Load32(base, function + g::kObjName);
  const uint64_t key = (uint64_t(function) << 32) | name_index;
  auto [it, inserted] = g_stats.try_emplace(key);
  Stat& s = it->second;
  if (inserted) {
    const uint32_t outer = Load32(base, function + g::kObjOuter);
    s.name = ObjectName(base, outer) + "." + ObjectName(base, function);
    s.native = (Load32(base, function + g::kFunctionFlags) & g::kFunctionNative) != 0;
  }
  s.via_mask |= 1u << via;
  return &s;
}

}  // namespace

namespace me::sprof {

bool Enabled() { return Mode() > 0; }

void NoteGameThread() {
  t_game_thread = true;
}

void Wrap(PPCContext& ctx, uint8_t* base, uint32_t function, Via via, PPCFunc* original) {
  const int mode = g_mode.load(std::memory_order_relaxed);
  if (mode == 0 || !t_game_thread || !Plausible(function)) [[likely]] {
    if (mode < 0) Mode();
    if (mode > 0) (t_game_thread ? g_hook_filtered : g_hook_other_thread).fetch_add(1, std::memory_order_relaxed);
    original(ctx, base);
    return;
  }
  ++g_hook_counted[via];
  Stat* s = Lookup(base, function, via);
  ++s->calls;
  ++g_total_calls;
  if (mode < 2 || g_stack.size() >= 4096) {
    original(ctx, base);
    return;
  }
  g_stack.push_back({s, armGetSystemTick(), 0});
  original(ctx, base);
  const uint64_t now = armGetSystemTick();
  if (g_stack.empty()) return;  // reset while inside (DumpAndReset from a nested Tick is not expected)
  const Frame f = g_stack.back();
  g_stack.pop_back();
  const uint64_t total = now - f.start;
  f.stat->incl += total;
  f.stat->self += total > f.child ? total - f.child : 0;
  if (!g_stack.empty()) g_stack.back().child += total;
}

void DumpAndReset(uint8_t* base, const std::string& label) {
  (void)base;
  if (Mode() <= 0) return;
  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - g_interval_start).count();
  const double tick_ms = 1000.0 / double(armGetSystemTickFreq());
  // Several keys can carry the same name (reloaded packages): merge by name.
  std::unordered_map<std::string, Stat> merged;
  for (auto& [key, s] : g_stats) {
    if (!s.calls) continue;
    Stat& m = merged[s.name];
    m.name = s.name;
    m.calls += s.calls;
    m.self += s.self;
    m.incl += s.incl;
    m.via_mask |= s.via_mask;
    m.native = m.native || s.native;
  }
  std::vector<const Stat*> rows;
  rows.reserve(merged.size());
  for (auto& [name, s] : merged) rows.push_back(&s);
  const size_t top = size_t(REXCVAR_GET(masseffect_script_prof_top));
  auto print = [&](const char* what) {
    REXLOG_INFO("[script_prof] {}: {} calls of {} functions in {:.1f} s, top {} by {}", label, g_total_calls,
                rows.size(), secs, std::min(top, rows.size()), what);
    for (size_t i = 0; i < rows.size() && i < top; ++i) {
      const Stat& s = *rows[i];
      const char* kind = s.native ? "native" : (s.via_mask & 2) ? "event" : "script";
      if (Mode() == 2)
        REXLOG_INFO("[script_prof] {} | {:3} | {:9} calls {:9.1f}/s | self {:8.1f} ms {:6.2f} ms/s | incl {:8.1f} ms | "
                    "{} | {}",
                    label, i + 1, s.calls, double(s.calls) / std::max(secs, 0.001), double(s.self) * tick_ms,
                    double(s.self) * tick_ms / std::max(secs, 0.001), double(s.incl) * tick_ms, kind, s.name);
      else
        REXLOG_INFO("[script_prof] {} | {:3} | {:9} calls {:9.1f}/s | {} | {}", label, i + 1, s.calls,
                    double(s.calls) / std::max(secs, 0.001), kind, s.name);
    }
  };
  REXLOG_INFO("[script_prof] {}: hooks: CallFunction {}, ProcessEvent {} counted; {} on other threads, {} filtered "
              "(near-zero CallFunction counts = the hook is not linked in: regenerate the code)",
              label, g_hook_counted[0], g_hook_counted[1], g_hook_other_thread.exchange(0), g_hook_filtered.exchange(0));
  g_hook_counted[0] = g_hook_counted[1] = 0;
  std::sort(rows.begin(), rows.end(), [](const Stat* a, const Stat* b) { return a->calls > b->calls; });
  print("calls");
  if (Mode() == 2) {
    std::sort(rows.begin(), rows.end(), [](const Stat* a, const Stat* b) { return a->self > b->self; });
    print("self time");
  }
  for (auto& [key, s] : g_stats) {
    s.calls = s.self = s.incl = 0;
    s.via_mask = 0;
  }
  // Entries of unloaded packages would grow the table across maps: drop it when it gets large.
  if (g_stack.empty() && g_stats.size() > 60000) g_stats.clear();
  g_total_calls = 0;
  g_interval_start = std::chrono::steady_clock::now();
}

// Without the tour: called by me_tour.cpp's Tick when GWorld changes; dumps what the previous world ran under its label.
void NoteWorld(uint8_t* base, uint32_t world, const std::string& label) {
  if (Mode() <= 0 || world == g_world) return;
  if (g_world) DumpAndReset(base, "map " + g_world_label);
  g_world = world;
  g_world_label = label;
}

}  // namespace me::sprof

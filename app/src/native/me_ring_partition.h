// Ring thread time partition (masseffect_native_ring_partition, measurement only).
//
// The "GPU ring native" thread is split into phases; every switch of phase adds the time since the previous switch
// to the phase that was running. Time is read from the ARM generic timer (CNTVCT_EL0, 19.2 MHz on the Switch):
// one tick is 52 ns, coarser than a single phase of a single draw, but the phases partition the thread's time
// without gaps, so the totals over 10 s are exact up to one tick per switch at the edges, and unbiased. Cost when
// on: two counter reads per scope (~20-40 ns on the A57). When off: one load and a branch per scope.
//
// Only the ring thread records (thread_local flag set by BindThisThread); scopes on other threads do nothing.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>

namespace me::native::ring_partition {

enum Phase : uint32_t {
  kParse = 0,       // PM4 packet loop and everything not inside another scope
  kWait,            // waiting for the write pointer (condition variable)
  kRegisters,       // type-0 runs, SET_CONSTANT*, LOAD_ALU_CONSTANT
  kShaderLoad,      // IM_LOAD and IM_LOAD_IMMEDIATE (identity, memo)
  kPair,            // PairDraw (Draw* records, object table)
  kDrawFront,       // NativeGraphicsSystem::Draw before the targets (identity checks, extent proofs)
  kEdramPrepare,    // TargetsVulkan::Draw: redirected clears, PrepareDrawEDRAM4 (sync walks)
  kEdramTransfer,   // recording of mode-4 tile transfers (phase 2 of SynchronizeEDRAM4), in draws and copies
  kDrawVulkan,      // DrawsVulkanImpl::Draw outside the texture loop
  kTextures,        // the sampler loop of DrawsVulkanImpl::Draw (caches, PrepareTexture, rechecks, creation)
  kEdramPublish,    // TargetsVulkan::Draw after the draw (publish of the written tiles)
  kCopy,            // Copy(): resolves and clears
  kPresent,         // Present() at XE_SWAP (output, submission)
  kWaitRegMem,      // WAIT_REG_MEM polling
  kFlush,           // end of a ring segment: read pointer write-back, wake-ups, deferred recording flush
  kReport,          // the 10 s reports
  kCount,
  kNone = 0xFFFFFFFFu  // Scope(kNone) does nothing
};

inline constexpr const char* kNames[kCount] = {
    "parse", "wait", "registers", "shader loads", "pairing", "draw front", "EDRAM prepare", "EDRAM transfers",
    "Vulkan draw", "textures", "EDRAM publish", "copies", "present", "WAIT_REG_MEM", "segment end", "reports"};

struct State {
  bool on = false;
  uint32_t phase = kParse;
  uint64_t last = 0;
  std::array<uint64_t, kCount> ticks{};
  std::array<uint64_t, kCount> entries{};
};

inline State g_state;               // ring thread only
inline thread_local bool t_ring = false;

inline uint64_t Now() {
#if defined(__aarch64__) && defined(__SWITCH__)
  uint64_t v;
  // cntpct_el0 (libnx armGetSystemTick): reading cntvct_el0 traps on Horizon (EC 0x18) and killed the ring thread.
  asm volatile("mrs %0, cntpct_el0" : "=r"(v));
  return v;
#else
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
#endif
}

inline double TicksPerSecond() {
#if defined(__aarch64__) && defined(__SWITCH__)
  return 19200000.0;
#else
  return 1e9;
#endif
}

inline bool Active() { return g_state.on && t_ring; }

// The calling thread is the ring thread; on = the cvar.
inline void BindThisThread(bool on) {
  t_ring = true;
  g_state.on = on;
  g_state.phase = kParse;
  g_state.last = on ? Now() : 0;
}

// Switches to `phase`; returns the phase that was running.
inline uint32_t Switch(uint32_t phase) {
  State& s = g_state;
  const uint64_t now = Now();
  s.ticks[s.phase] += now - s.last;
  s.last = now;
  const uint32_t previous = s.phase;
  s.phase = phase;
  return previous;
}

class Scope {
 public:
  explicit Scope(uint32_t phase) : active_(phase != kNone && Active()) {
    if (active_) {
      previous_ = Switch(phase);
      ++g_state.entries[phase];
    }
  }
  ~Scope() {
    if (active_) Switch(previous_);
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  bool active_;
  uint32_t previous_ = kParse;
};

// Closes the running interval into its phase and returns the totals since the previous call (then resets them).
inline void Take(std::array<uint64_t, kCount>& ticks, std::array<uint64_t, kCount>& entries) {
  Switch(g_state.phase);
  ticks = g_state.ticks;
  entries = g_state.entries;
  g_state.ticks.fill(0);
  g_state.entries.fill(0);
}

}  // namespace me::native::ring_partition

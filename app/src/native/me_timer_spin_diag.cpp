// Mass Effect - diagnostic probe of a spinning guest timer thread (docs/kernel-waits.md, part 4). Off by default.
//
// WHAT IT WATCHES
//   A guest timer thread (probably Bink's background IO) runs an inner loop that, for every registered item, calls the
//   item's ready test, waits up to 1 ms for the ready items' mutexes, runs the fill callback of the one it got and
//   releases the mutex, then scans again without advancing its tick. The hooked function is that ready test,
//   ready(item, tick):
//     [item-228] != 0                              -> 0 (not ready)
//     pct = ([item-184] + 1) * 100 / ([item-188] + 1)
//     pct < 50                                     -> -1 - pct (ready, regardless of the tick)
//     [item+328] != tick                           -> 0x80000000 - [item+328] (ready once per tick)
//     else                                         -> 0
//   While pct stays under 50 the item is ready on every scan, so the loop never waits for the next tick: one boot spent
//   92 % of a core there. The fill callback (owner = item - 500) calls the function at [owner+256] with owner+240 and
//   stores the tick at [owner+828] = [item+328].
//
// WHAT THE CVAR DOES
//   masseffect_diag_timer_spin = N > 0: count, per item, the ready tests with the same tick; past N of them, log the
//   item's fields once per second per item ([timer_spin] lines). It reads guest memory only: the original runs
//   unchanged and its result is returned as is. 0 = off (one load and a branch per call).
//
// Adding this hook changes the hooked set (the next build regenerates the code; tools/edition.sh does it).

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <chrono>
#include <cstdint>
#include <cstring>

REXCVAR_DEFINE_INT32(masseffect_diag_timer_spin, 0, "Mass Effect",
                     "Diagnostics (docs/kernel-waits.md): log the fields of a guest timer item whose ready test runs "
                     "more than this many times without a tick change (once per second per item); 0 = off")
    .range(0, 100000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82D7E990);  // ready test of the timer item (indirect call from the timer's inner loop)

namespace {

constexpr int32_t kOffBlocked = -228;   // non-zero: never ready
constexpr int32_t kOffTotal = -188;     // denominator of the fill percentage
constexpr int32_t kOffFilled = -184;    // numerator of the fill percentage
constexpr int32_t kOffLastTick = 328;   // tick of the last fill callback
constexpr int32_t kOffOwner = -500;     // the fill callback's object
constexpr uint32_t kOwnerFillFn = 256;  // function the fill callback calls ...
constexpr uint32_t kOwnerFillArg = 240; // ... with owner + 240

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v = 0;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

struct ItemState {
  uint32_t item = 0;
  uint32_t tick = 0;
  uint64_t same_tick = 0;  // ready tests since the tick last changed
  uint64_t calls = 0;      // ready tests since the last log line
  std::chrono::steady_clock::time_point last_log{};
};

// Per thread (the timer threads call it; no locking). A few items per timer; the oldest slot is reused.
thread_local ItemState t_items[8];
thread_local unsigned t_next = 0;

ItemState& StateOf(uint32_t item) {
  for (auto& s : t_items) {
    if (s.item == item) return s;
  }
  ItemState& s = t_items[t_next++ % (sizeof(t_items) / sizeof(t_items[0]))];
  s = ItemState{};
  s.item = item;
  return s;
}

void Probe(const uint8_t* base, uint32_t item, uint32_t tick, uint32_t blocked, uint32_t total, uint32_t filled,
           uint32_t last_tick, uint32_t result, int32_t threshold) {
  ItemState& s = StateOf(item);
  ++s.calls;
  if (s.tick != tick) {
    s.tick = tick;
    s.same_tick = 0;
  }
  if (++s.same_tick <= uint64_t(threshold)) return;
  const auto now = std::chrono::steady_clock::now();
  if (now - s.last_log < std::chrono::seconds(1)) return;
  const double seconds =
      s.last_log.time_since_epoch().count() ? std::chrono::duration<double>(now - s.last_log).count() : 0.0;
  s.last_log = now;
  const uint32_t owner = item + uint32_t(kOffOwner);
  // Same 32-bit arithmetic as the guest (mulli, divwu; a zero divisor gives 0).
  const uint32_t pct = total + 1u ? ((filled + 1u) * 100u) / (total + 1u) : 0u;
  REXLOG_INFO("[timer_spin] item {:08X} tick {} | {} ready tests with this tick, {:.0f}/s | [item-228] {:08X} "
              "[item-188] {} [item-184] {} -> pct {} | [item+328] {} -> ready {:08X} | fill fn {:08X}({:08X})",
              item, tick, s.same_tick, seconds > 0.0 ? double(s.calls) / seconds : 0.0, blocked, total, filled, pct,
              last_tick, result, Load32(base, owner + kOwnerFillFn), owner + kOwnerFillArg);
  s.calls = 0;
}

}  // namespace

REX_HOOK_RAW(sub_82D7E990) {
  static const int32_t threshold = REXCVAR_GET(masseffect_diag_timer_spin);
  if (threshold <= 0) {
    __imp__sub_82D7E990(ctx, base);
    return;
  }
  const uint32_t item = ctx.r3.u32;
  const uint32_t tick = ctx.r4.u32;
  // The ready test only reads these; sampled before it runs, so the line shows its exact inputs.
  const uint32_t blocked = Load32(base, item + uint32_t(kOffBlocked));
  const uint32_t total = Load32(base, item + uint32_t(kOffTotal));
  const uint32_t filled = Load32(base, item + uint32_t(kOffFilled));
  const uint32_t last_tick = Load32(base, item + uint32_t(kOffLastTick));
  __imp__sub_82D7E990(ctx, base);
  Probe(base, item, tick, blocked, total, filled, last_tick, ctx.r3.u32, threshold);
}

// sub_82212540 - TArray<uint32>::Add (append r4 to the array object r3), fast path only.
//
//   r3 = array {[+0] data, [+12] num (s32), [+16] max (s32)}, r4 = value.
//   num < max: data[num] = value; num = num + 1 (the original reloads num after the store; kept: the data block may alias
//   the array header).  num >= max: the original grows the array (calls sub_822124E0, a virtual call per element and
//   RtlLeaveCriticalSection): the native version DECLINES (returns false, nothing modified) and the original runs.
//
// Registers: the original leaves r3 (= input r3, never assigned on this path) and every other volatile register it
// does not touch unchanged (r27/r31 are locals, not in ctx); r1 restored; the back-chain word it stores at r1 - 144 is
// callee-owned stack scratch and is not written here. No FPU use. liveness.py: no direct call site reads a volatile
// register after the call.
#pragma once

#include "../me_hot_common.h"

namespace me::hot::n_82212540 {

inline constexpr Cmp kCmp = kCmpRet;

inline bool Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t a = ctx.r3.u32;
  const int32_t max = int32_t(Ld32(base, a + 16));
  const int32_t num = int32_t(Ld32(base, a + 12));
  if (!(num < max)) return false;
  const uint32_t data = Ld32(base, a + 0);
  St32(base, data + (uint32_t(num) << 2), ctx.r4.u32);
  St32(base, a + 12, Ld32(base, a + 12) + 1u);
  return true;
}

// Writes() also lists what the original writes on the declined path (the fuzzer checks every byte changed by either side;
// the guard only needs the native part, a superset is harmless).
inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) {
  const uint32_t a = ctx.r3.u32;
  const int32_t max = int32_t(Ld32(base, a + 16));
  const int32_t num = int32_t(Ld32(base, a + 12));
  if (!(num < max)) {  // declined: the original grows the array (flush: num = 0, data[0] = value, num = 1)
    w.Add(a + 12, 4);
    w.Add(Ld32(base, a + 0), 4);
    return;
  }
  w.Add(Ld32(base, a + 0) + (uint32_t(num) << 2), 4);
  w.Add(a + 12, 4);
}

}  // namespace me::hot::n_82212540

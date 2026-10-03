// sub_82BB0748 - hash table "find entry" wrapper: *out = { table slot, index } or { 0, 0 }.
//
//   r3 = out (2 words), r4 -> slot A ([r4] = table-entries base a, null -> not found), r5 -> slot B ([r5] = object b).
//   key = [b + 4] & [a + 4]; idx = sub_82BAFF58(r4, r5, key) (inlined: n_82BAFF58.h Find);
//   if (a != 0 && (s32)idx >= 0) { out[0] = r4 (the slot pointer as passed), out[1] = idx } else { out[0] = 0, out[1] = 0 }.
//   Returns r3 = out (unchanged).
//
// Exactness notes: the callee is a read-only leaf, so inlining it changes no memory effect; the two output words are
// stored after every load (as in the original). r3 is returned as the original 64-bit value (mr r3,r31): left alone.
// Stack spills (-8/-16/-24(r1), stwu) are callee-owned scratch. No FPU. Volatile scratch not reproduced (liveness.py:
// no call site reads any volatile register after the call).
#pragma once

#include "n_82BAFF58.h"

namespace me::hot::n_82BB0748 {

inline constexpr Cmp kCmp = kCmpRet;

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t out = ctx.r3.u32, slotA = ctx.r4.u32, slotB = ctx.r5.u32;
  uint32_t first = 0, second = 0;
  const uint32_t a = Ld32(base, slotA);
  if (a != 0) {
    const uint32_t b = Ld32(base, slotB);
    const uint32_t key = Ld32(base, b + 4) & Ld32(base, a + 4);
    const uint64_t idx = n_82BAFF58::Find(base, slotA, slotB, key);
    if (int32_t(uint32_t(idx)) >= 0) {
      first = slotA;
      second = uint32_t(idx);
    }
  }
  St32(base, out, first);
  St32(base, out + 4, second);
}

inline void Writes(const PPCContext& ctx, const uint8_t*, me::hot::Writes& w) { w.Add(ctx.r3.u32, 8); }

}  // namespace me::hot::n_82BB0748

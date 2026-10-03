// sub_8230D5F0 - UE3 hash-map style lookup of a 64-bit key along two chains of tables; returns the first non-zero value.
//
//   r3 = object: [+24] -> owner, [+52] = first table of the second chain; owner [+32] = first table of the first chain.
//   r4 = 64-bit key (hi = r4 >> 32, lo = r4 & 0xFFFFFFFF), r5 = flag: the first chain is searched only if r5 == 0
//   (32-bit compare), the owner and its [+32] are non-null.
//   table: [+60] next table (parent chain), [+164] entry array (16-byte entries: next, key hi, key lo, value),
//          [+168] entry count (> 0 needed), [+176] bucket array (u32 indices, -1 = empty), [+180] bucket count (power of
//          two; the bucket is hi & (count - 1)).
//   A table is probed by walking its bucket's entry chain for key == (hi, lo); the value found (entry [+12], may be 0)
//   ends the search when non-zero. Result r3 = that value (zero-extended) or 0. No guest memory is written.
//
// The original spills r4 to the caller's parameter area (std r4,24(r1)) and reloads both halves; the key halves come
// straight from r4 here (that spill is callee-owned scratch). Registers: r3 = result is the only output read by
// callers (648 direct call sites read r3 only); r4-r12 are dead scratch, no FPU use.
#pragma once

#include "../me_hot_common.h"

namespace me::hot::n_8230D5F0 {

inline constexpr Cmp kCmp = kCmpRet;

// returns the entry value for key (hi, lo) in `node` (0 if absent)
inline uint32_t Probe(const uint8_t* base, uint32_t node, uint32_t hi, uint32_t lo) {
  const uint32_t buckets = Ld32(base, node + 176);
  if (buckets == 0) return 0;
  if (int32_t(Ld32(base, node + 168)) <= 0) return 0;
  const uint32_t mask = Ld32(base, node + 180) - 1u;
  uint32_t head = Ld32(base, ((mask & hi) << 2) + buckets);
  if (int32_t(head) == -1) return 0;
  const uint32_t entries = Ld32(base, node + 164);
  for (;;) {
    const uint32_t e = (head << 4) + entries;
    if (Ld32(base, e + 4) == hi && Ld32(base, e + 8) == lo) return Ld32(base, e + 12);
    head = Ld32(base, e);
    if (int32_t(head) == -1) return 0;
  }
}

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t obj = ctx.r3.u32;
  const uint32_t hi = uint32_t(ctx.r4.u64 >> 32), lo = ctx.r4.u32;
  uint32_t res = 0;
  uint32_t owner = Ld32(base, obj + 24);
  if (owner != 0) {
    const uint32_t first = Ld32(base, owner + 32);
    if (first != 0 && ctx.r5.s32 == 0) {
      for (uint32_t n = first; n != 0; n = Ld32(base, n + 60)) {
        res = Probe(base, n, hi, lo);
        if (res != 0) {
          ctx.r3.u64 = res;
          return;
        }
      }
    }
  }
  for (uint32_t n = Ld32(base, obj + 52); n != 0; n = Ld32(base, n + 60)) {
    res = Probe(base, n, hi, lo);
    if (res != 0) break;
  }
  ctx.r3.u64 = res;
}

inline void Writes(const PPCContext&, const uint8_t*, me::hot::Writes&) {}

}  // namespace me::hot::n_8230D5F0

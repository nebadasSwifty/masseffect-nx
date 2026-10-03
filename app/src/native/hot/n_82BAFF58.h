// sub_82BAFF58 - chained hash table lookup (leaf, read only): index of the entry that refers to a given object, or -1.
//
//   r3 -> table T ([T + 0] = entries base E, [E + 4] = mask M, entry i at E + 8 + 32 * i: [e + 0] = next index,
//   [e + 8] -> object o, [o + 4] = hash), r4 -> pointer P (the object looked up is [P + 0]), r5 = key (bucket index).
//   Entry key: [e] == 0xFFFFFFFE (empty) -> -1; its object must hash to the bucket ((hash & M) == key) else -1.
//   Then follow the chain: if (hash(o) & M) == key and o == [P] return the current index (initially r5 with all its 64
//   bits, afterwards the zero-extended next index); next = [e]; next == -1 (as s32) -> -1 (all 64 bits).
//
// No guest memory is written. r3 is the only register a caller reads (liveness.py: r3 at both direct call sites); the
// other scratch the original leaves (r4, r5 -> ..., r7-r11) is not reproduced. Entry addresses are 32-bit wrapping sums,
// like the generated code's r11.u32 uses.
#pragma once

#include "../me_hot_common.h"

namespace me::hot::n_82BAFF58 {

inline constexpr Cmp kCmp = kCmpRet;

// Returns the PPC r3 value (64 bits).
inline uint64_t Find(const uint8_t* base, uint32_t tbl, uint32_t pref, uint64_t key64) {
  const uint32_t key = uint32_t(key64);
  const uint32_t ebase = Ld32(base, tbl);
  uint32_t e = (key << 5) + ebase + 8;
  if (Ld32(base, e) == 0xFFFFFFFEu) return ~0ull;
  const uint32_t mask = Ld32(base, ebase + 4);
  if ((Ld32(base, Ld32(base, e + 8) + 4) & mask) != key) return ~0ull;
  uint64_t cur = key64;
  for (;;) {
    const uint32_t o = Ld32(base, e + 8);
    if ((Ld32(base, o + 4) & mask) == key && Ld32(base, pref) == o) return cur;
    const uint32_t next = Ld32(base, e);
    if (next == 0xFFFFFFFFu) return ~0ull;
    cur = next;
    e = (next << 5) + ebase + 8;
  }
}

inline void Native(PPCContext& ctx, uint8_t* base) { ctx.r3.u64 = Find(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u64); }

inline void Writes(const PPCContext&, const uint8_t*, me::hot::Writes&) {}

}  // namespace me::hot::n_82BAFF58

// sub_8230F598 - hash lookup of an object by (owner, 64-bit key, class filter), walking a bucket chain.
// Russian edition copy: only the guest constants differ from the English file (data addresses translated through the
// generated code of the RU function, which is instruction-for-instruction the same as the English one).
//
//   r3 = class filter (0 = any), r4 = owner (0 = lookup in the second table), r5 = 64-bit key (std'ed to the caller's
//   parameter save area and re-read as two words by the original: hi = r5 >> 32, lo = (u32)r5), r6 = exact-class flag
//   (s32 != 0: [node+52] must equal the filter, == 0: the filter may be anywhere in the class chain [node+52], next = [c+60]),
//   r7 = (second table only) when (s32)r7 == 0 the node's owner word [+40] must be 0, r8 = 64-bit exclusion mask
//   ([node+8] & mask must be 0, mask != -1).
//   Table 1 (owner != 0): bucket = [0x82E874A0 + ((lo ^ hi ^ owner) << 2 & 0x7FFC)], chain via [node+20], owner word [+40]
//   must equal r4. Table 2 (owner == 0): bucket = [0x82E8F4A0 + ((lo ^ hi) << 2 & 0x7FFC)], chain via [node+16].
//   Node key: 8 bytes at [node+44] unless [node+4] == -1 (key not computed yet): then the original calls sub_82389D48 to
//   build it (a guest function with unknown side effects). This version DECLINES (returns false, nothing was modified: the
//   lookup only reads memory until then) and the original runs from scratch.
//   Returns r3 = the node or 0.
// Registers: r3 (compared); every other volatile register the original leaves behind is dead scratch (liveness.py: the 19
// direct call sites read only r3). The stack words the original writes (back chain at r1 - 176, r5 at r1 + 32 = the caller's
// parameter save area, key copy at r1 - 96) are callee-owned scratch. No FPU use. Writes(): none.
#pragma once

#include "../me_hot_common.h"

namespace me::hot::n_8230F620 {

inline constexpr Cmp kCmp = kCmpRet;

inline constexpr uint32_t kTable1 = 0x82E874A0u;  // 8 KB of buckets (index = hash << 2 & 0x7FFC)
inline constexpr uint32_t kTable2 = 0x82E8F4A0u;

inline bool Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t filter = ctx.r3.u32, owner = ctx.r4.u32;
  const uint32_t hi = uint32_t(ctx.r5.u64 >> 32), lo = ctx.r5.u32;
  const bool exact = ctx.r6.s32 != 0;
  const uint64_t mask = ctx.r8.u64;
  uint32_t node, next;
  if (owner != 0) {
    node = Ld32(base, kTable1 + (((lo ^ hi ^ owner) << 2) & 0x7FFCu));
    next = 20;
  } else {
    node = Ld32(base, kTable2 + (((lo ^ hi) << 2) & 0x7FFCu));
    next = 16;
  }
  for (; node != 0; node = Ld32(base, node + next)) {
    if (Ld32(base, node + 4) == 0xFFFFFFFFu) return false;  // key not built yet: the original calls sub_82389D48
    if (mask == ~0ull) continue;                              // (mask & x) != 0 or the mask == -1 test: always skipped
    if (Ld32(base, node + 44) != hi || Ld32(base, node + 48) != lo) continue;
    if (Ld64(base, node + 8) & mask) continue;
    if (owner != 0) {
      if (Ld32(base, node + 40) != owner) continue;
    } else if (ctx.r7.s32 == 0 && Ld32(base, node + 40) != 0) {
      continue;
    }
    if (filter != 0) {
      uint32_t c = Ld32(base, node + 52);
      if (exact) {
        if (c != filter) continue;
      } else {
        for (;; c = Ld32(base, c + 60)) {
          if (c == 0) goto next_node;
          if (c == filter) break;
        }
      }
    }
    ctx.r3.u64 = node;
    return true;
  next_node:;
  }
  ctx.r3.u64 = 0;
  return true;
}

inline void Writes(const PPCContext&, const uint8_t*, me::hot::Writes&) {}  // read-only (stack scratch aside)

}  // namespace me::hot::n_8230F620

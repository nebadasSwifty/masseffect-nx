// sub_8225C688 - iterator advance over a two-level object graph: container -> array of groups -> array of item pointers.
// Russian edition copy: only the guest constants differ from the English file (data addresses translated through the
// generated code of the RU function, which is instruction-for-instruction the same as the English one).
//
//   r3 = iterator: [+0] item index inside the current group, [+4] group index, [+8] done flag, [+12] visited counter,
//        [+16] current item (0 = none yet).
//   Globals: [0x82EAEAB4] -> container (+72 groups array, +76 group count), [0x82EAC8F8] flag word.
//   group: +60 items array, +64 item count, +216 first item index, +364 flag.
//   item: +132 u32, bit 31 set => the item is skipped (the iterator's current item is cleared again).
// The control flow is the generated one (labels below keep the original addresses); every guest load / store is issued
// in the original order (the iterator may alias the graph).
//
// Registers: r3 unchanged (compared); everything else the original touches is dead scratch (liveness.py: no direct call
// site of the 16 reads a volatile register after the call). No FPU use, no stack scratch.
#pragma once

#include "../me_hot_common.h"

namespace me::hot::n_8225CA80 {

inline constexpr Cmp kCmp = kCmpRet;

inline constexpr uint32_t kGlobalContainer = 0x82EB0000u - 5452u;  // 0x82EAEAB4
inline constexpr uint32_t kGlobalFlag = 0x82EB0000u - 14088u;      // 0x82EAC8F8

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t it = ctx.r3.u32;
  uint32_t r10 = Ld32(base, it + 4);
  St32(base, it + 16, 0);
  uint32_t grp = Ld32(base, kGlobalContainer);  // r11
  uint32_t r9 = Ld32(base, grp + 76);
  if (!(int32_t(r10) < int32_t(r9))) {
    St32(base, it + 0, 0);
    St32(base, it + 4, 0);
    grp = Ld32(base, kGlobalContainer);
  }
  r9 = Ld32(base, it + 4);
  const uint32_t arr = Ld32(base, grp + 72);
  const uint32_t r6 = Ld32(base, it + 8);
  grp = Ld32(base, (r9 << 2) + arr);
  if (r6 != 0) return;
  uint32_t r5;
  for (;;) {  // loc_8225CACC
    if (Ld32(base, it + 16) != 0) return;
    r10 = Ld32(base, grp + 364);
    if (r10 != 0 && Ld32(base, kGlobalFlag) == 0) goto loc_8225CB08;
    // loc_8225CAF0
    r10 = Ld32(base, it + 0) + 1u;
    St32(base, it + 0, r10);
    r5 = Ld32(base, grp + 64);
    if (int32_t(r10) < int32_t(r5)) goto loc_8225CB58;
  loc_8225CB08:
    r9 += 1u;
    St32(base, it + 4, r9);
    grp = Ld32(base, kGlobalContainer);
    r10 = Ld32(base, grp + 76);
    if (!(int32_t(r9) < int32_t(r10))) {  // loc_8225CBA0: ran out of groups
      St32(base, it + 0, 0);
      St32(base, it + 4, 0);
      St32(base, it + 8, 1);
      return;
    }
    grp = Ld32(base, grp + 72);
    grp = Ld32(base, (r9 << 2) + grp);
    r10 = Ld32(base, grp + 364);
    if (r10 != 0 && Ld32(base, kGlobalFlag) == 0) goto loc_8225CB90;
    // loc_8225CB44
    r10 = Ld32(base, grp + 216);
    St32(base, it + 0, r10);
    r5 = Ld32(base, grp + 64);
    if (!(int32_t(r10) < int32_t(r5))) goto loc_8225CB90;
  loc_8225CB58:
    r5 = Ld32(base, it + 0) << 2;
    St32(base, it + 12, Ld32(base, it + 12) + 1u);
    r10 = Ld32(base, grp + 60);
    r10 = Ld32(base, r5 + r10);
    St32(base, it + 16, r10);
    if (r10 != 0) {
      r10 = Ld32(base, r10 + 132);
      if ((r10 & 0x80000000u) == 0) goto loc_8225CB90;
    }
    St32(base, it + 16, 0);  // loc_8225CB8C
  loc_8225CB90:
    if (Ld32(base, it + 8) != 0) return;
  }
}

inline void Writes(const PPCContext& ctx, const uint8_t*, me::hot::Writes& w) { w.Add(ctx.r3.u32, 20); }

}  // namespace me::hot::n_8225CA80

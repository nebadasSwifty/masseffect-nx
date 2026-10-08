// sub_82270788 - UE3 "Cast<T>(obj)" / IsA check: returns obj when T's class is on obj's class chain, else 0.
// Russian edition copy: only the guest constants differ from the English file (data addresses translated through the
// generated code of the RU function, which is instruction-for-instruction the same as the English one).
//
//   r3 = obj. null -> 0. Otherwise cls = sub_822709C8() (lazy static-class getter: [0x82EAF16C], created on first use),
//   walk c = [obj + 52]; while (c != 0) { if (c == cls) return obj; c = [c + 60]; }  and when the chain ends without a hit:
//   return (cls == 0) ? obj : 0.
//
// Exactness notes:
//  * r3 is returned as the ORIGINAL 64-bit r3 (mr r3,r31), so the register is simply left alone on the "found" paths.
//  * The getter's first call (singleton still null) runs constructors and writes arbitrary guest memory: that case is
//    delegated to the original function (__imp__sub_82270788, always the unpatched entry) and Writes() declares overflow
//    for it so the guard never has to roll it back. When the singleton exists the getter is a pure load.
//  * No memory is written otherwise (the prologue/epilogue stack spills are callee-owned scratch); LR, r1 and all
//    non-volatile registers are untouched. Volatile scratch (r11, r12, ...) is not reproduced (liveness.py: only r3 is read).
#pragma once

#include "../me_hot_common.h"

extern "C" void __imp__sub_82270788(PPCContext&, uint8_t*);

namespace me::hot::n_82270C78 {

inline constexpr Cmp kCmp = kCmpRet;

inline constexpr uint32_t kSingleton = 0x82EAF16Cu;  // lis r31,-32021; lwz r3,-3732(r31)

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t obj = ctx.r3.u32;
  if (obj == 0) {
    ctx.r3.u64 = 0;
    return;
  }
  const uint32_t cls = Ld32(base, kSingleton);
  if (cls == 0) [[unlikely]] {
    __imp__sub_82270788(ctx, base);
    return;
  }
  uint32_t c = Ld32(base, obj + 52);
  while (c != 0) {
    if (c == cls) return;  // r3 (= obj, all 64 bits) unchanged
    c = Ld32(base, c + 60);
  }
  ctx.r3.u64 = 0;
}

inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) {
  if (ctx.r3.u32 != 0 && Ld32(base, kSingleton) == 0) w.overflow = true;  // lazy init: only the original may run
}

}  // namespace me::hot::n_82270C78

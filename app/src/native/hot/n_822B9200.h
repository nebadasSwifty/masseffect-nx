// sub_822B9200 - UE3 "Cast<T>(obj)" / IsA check (same template as sub_82270C78; static-class getter sub_822B9560).
//
//   r3 = obj. null -> 0. Otherwise cls = sub_822B9560() (lazy static-class getter: [0x82EAF420]), walk
//   c = [obj + 52]; while (c != 0) { if (c == cls) return obj; c = [c + 60]; } chain end: return (cls == 0) ? obj : 0.
//
// Exactness notes: see n_82270C78.h (r3 is the original 64-bit r3 on "found"; the getter's first call, singleton still
// null, creates the class object and writes arbitrary memory -> delegated to the original, Writes() = overflow).
// No other memory is written; volatile scratch not reproduced (liveness.py: only r3 is read after the call).
#pragma once

#include "../me_hot_common.h"

extern "C" void __imp__sub_822B9200(PPCContext&, uint8_t*);

namespace me::hot::n_822B9200 {

inline constexpr Cmp kCmp = kCmpRet;

inline constexpr uint32_t kSingleton = 0x82EAF420u;  // lis r31,-32021; lwz r3,-3040(r31) in sub_822B9560

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t obj = ctx.r3.u32;
  if (obj == 0) {
    ctx.r3.u64 = 0;
    return;
  }
  const uint32_t cls = Ld32(base, kSingleton);
  if (cls == 0) [[unlikely]] {
    __imp__sub_822B9200(ctx, base);
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

}  // namespace me::hot::n_822B9200

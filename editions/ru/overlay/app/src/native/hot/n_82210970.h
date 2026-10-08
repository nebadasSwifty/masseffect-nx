// sub_822108B8 - object-iterator advance with a class-chain (IsA) test.
// Russian edition copy: only the guest constants differ from the English file (data addresses translated through the
// generated code of the RU function, which is instruction-for-instruction the same as the English one).
//
//   r3 = iterator {[+0] class filter (0 = any), [+4] current index (s32)}. Globals: [0x82EAC8EC] flag word,
//   [0x82EC1300] -> object array data, [0x82EC1304] = array count (s32).
//   Advances [it+4] until a non-null object whose flags word [obj+8] (high half of the 64-bit load) has no bit of the
//   exclusion mask (bits 1 and 10, or only bit 1 when the flag word is non-zero: 0x402 / 0x2; the original's 64-bit mask
//   0x40200000000 / 0x200000000, never -1) and whose class chain ([obj+52], next = [c+60]) contains the filter, or the
//   filter is 0; returns when the index reaches the count, or on the first match. Nothing is returned (r3 unchanged:
//   liveness.py: no call site of the 153 reads any volatile register after the call).
// The iterator store [+4] is issued every step in the original order (the iterator may alias the array / objects). No
// FPU use, no stack use.
#pragma once

#include "../me_hot_common.h"

namespace me::hot::n_82210970 {

inline constexpr Cmp kCmp = {R(3), 0, 0};

inline constexpr uint32_t kFlag = 0x82EB0000u - 14100u;   // 0x82EAC8EC
inline constexpr uint32_t kArr = 0x82EC0000u + 4864u;     // 0x82EC1300: {data, count}

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t it = ctx.r3.u32;
  const uint32_t mask = Ld32(base, kFlag) == 0 ? 0x402u : 0x2u;
  uint32_t i = Ld32(base, it + 4);
  for (;;) {
    ++i;
    St32(base, it + 4, i);
    if (!(int32_t(i) < int32_t(Ld32(base, kArr + 4)))) return;
    const uint32_t obj = Ld32(base, Ld32(base, kArr + 0) + (i << 2));
    if (obj == 0) continue;
    if (Ld32(base, obj + 8) & mask) continue;
    uint32_t c = Ld32(base, obj + 52);
    const uint32_t want = Ld32(base, it + 0);
    for (; c != 0; c = Ld32(base, c + 60))
      if (c == want) return;
    if (want == 0) return;
  }
}

inline void Writes(const PPCContext& ctx, const uint8_t*, me::hot::Writes& w) { w.Add(ctx.r3.u32 + 4, 4); }

}  // namespace me::hot::n_82210970

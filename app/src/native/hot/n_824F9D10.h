// sub_824F9D10 - Direct3D: set one float4 constant (vertex-shader side bookkeeping) and mark it dirty.
//
//   r3 = device, r5 -> {u16 reg, u16 count}, r6 -> float[4]. If count == 0 nothing happens (not even the FPU mode).
//   float[0] -> [device + ((reg + 368) << 4)], float[1..3] -> [device + (reg << 4) + 5892 + 4 * (i - 1)],
//   [device + 8] |= (0x8000000000000000 >> ((reg >> 2) & 63)) unless bit 6 of (reg >> 2) is set (then 0).
// The recompiled lfs/stfs pairs are float->double->float round trips that GCC folds into raw 32-bit copies (checked in
// the disassembly of the built ELF), so they are byte copies here; the FPU flush mode is cleared as in the original.
#pragma once

#include "../me_hot_common.h"

namespace me::hot::n_824F9D10 {

inline constexpr Cmp kCmp = kCmpRet;

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, desc = ctx.r5.u32, src = ctx.r6.u32;
  if (Ld16(base, desc + 2) == 0) return;
  const uint32_t reg = Ld16(base, desc);
  ctx.fpscr.disableFlushMode();
  const uint32_t a0 = dev + ((reg + 368u) << 4);
  const uint32_t a1 = dev + (reg << 4);
  // load / store pairs in the original order (the destination may alias the source float4: a store is seen by the later loads)
  uint32_t t;
  std::memcpy(&t, Raw(base, src), 4);  // raw big-endian words: copied without swapping
  std::memcpy(Raw(base, a0), &t, 4);
  std::memcpy(&t, Raw(base, src + 4), 4);
  std::memcpy(Raw(base, a1 + 5892), &t, 4);
  std::memcpy(&t, Raw(base, src + 8), 4);
  std::memcpy(Raw(base, a1 + 5896), &t, 4);
  std::memcpy(&t, Raw(base, src + 12), 4);
  std::memcpy(Raw(base, a1 + 5900), &t, 4);
  const uint32_t sh = reg >> 2;
  const uint64_t bit = (sh & 0x40) ? 0 : (0x8000000000000000ull >> (sh & 0x3F));
  St64(base, dev + 8, Ld64(base, dev + 8) | bit);
}

inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) {
  const uint32_t dev = ctx.r3.u32, desc = ctx.r5.u32;
  if (Ld16(base, desc + 2) == 0) return;
  const uint32_t reg = Ld16(base, desc);
  w.Add(dev + ((reg + 368u) << 4), 4);
  w.Add(dev + (reg << 4) + 5892, 12);
  w.Add(dev + 8, 8);
}

}  // namespace me::hot::n_824F9D10

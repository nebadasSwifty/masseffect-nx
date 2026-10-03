// sub_82219258 / sub_82219360 - Direct3D: copy N 16-byte vectors from an arbitrarily aligned guest source into the
// device's constant block and OR a dirty mask into the device. The two functions are identical except for two constants
// (instruction-by-instruction diff of the generated code): vertex-shader side 82219258 uses bias 112 and mask word +0,
// pixel-shader side 82219360 uses bias 368 and mask word +8.
//
//   r3 = device, r4 = first register (the destination is device + (r4 + bias) * 16), r5 = source (unaligned),
//   r6 = number of vectors, r7 = 64-bit dirty mask: [device + maskoff] |= r7 after the copy.
//
// PPC: lvlx/lvrx pairs build an unaligned 16-byte load (bytes [src, src + 16) in guest order), stvx stores it at
// dst & ~15; four vectors per loop iteration (all four loaded before the first store), then single vectors. Net effect:
// a byte copy of N * 16 bytes, forward in 64-byte then 16-byte steps, destination rounded down to 16 bytes.
// Register state: r3 is returned unchanged (a caller reads it); everything else the original leaves behind is dead
// scratch (checked by tests/hot_fuzz/liveness.py: no call site reads a volatile register but r3).
#pragma once

#include "../me_hot_common.h"

namespace me::hot::n_d3d_cbcopy {

inline constexpr Cmp kCmp = kCmpRet;

template <uint32_t kBias>
inline uint32_t Dst(const PPCContext& c) {
  return ((c.r4.u32 + kBias) << 4) + c.r3.u32;
}

template <uint32_t kBias, uint32_t kMaskOff>
inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  uint32_t src = ctx.r5.u32;
  uint32_t dst = Dst<kBias>(ctx) & ~15u;
  uint32_t n = ctx.r6.u32;
  while (n >= 4) {
    uint8_t t[64];
    std::memcpy(t, Raw(base, src), 64);   // all four loads before the stores, as the original
    std::memcpy(Raw(base, dst), t, 64);
    src += 64;
    dst += 64;
    n -= 4;
  }
  while (n) {
    uint8_t t[16];
    std::memcpy(t, Raw(base, src), 16);
    std::memcpy(Raw(base, dst), t, 16);
    src += 16;
    dst += 16;
    --n;
  }
  St64(base, dev + kMaskOff, Ld64(base, dev + kMaskOff) | ctx.r7.u64);
}

template <uint32_t kBias, uint32_t kMaskOff>
inline void Writes(const PPCContext& ctx, const uint8_t*, me::hot::Writes& w) {
  const uint64_t bytes = uint64_t(ctx.r6.u32) * 16;
  if (bytes > me::hot::Writes::kMaxBytes) {
    w.overflow = true;
    return;
  }
  w.Add(Dst<kBias>(ctx) & ~15u, uint32_t(bytes));
  w.Add(ctx.r3.u32 + kMaskOff, 8);
}

}  // namespace me::hot::n_d3d_cbcopy

namespace me::hot::n_82219258 {
inline constexpr Cmp kCmp = n_d3d_cbcopy::kCmp;
inline void Native(PPCContext& c, uint8_t* b) { n_d3d_cbcopy::Native<112, 0>(c, b); }
inline void Writes(const PPCContext& c, const uint8_t* b, me::hot::Writes& w) { n_d3d_cbcopy::Writes<112, 0>(c, b, w); }
}  // namespace me::hot::n_82219258

namespace me::hot::n_82219360 {
inline constexpr Cmp kCmp = n_d3d_cbcopy::kCmp;
inline void Native(PPCContext& c, uint8_t* b) { n_d3d_cbcopy::Native<368, 8>(c, b); }
inline void Writes(const PPCContext& c, const uint8_t* b, me::hot::Writes& w) { n_d3d_cbcopy::Writes<368, 8>(c, b, w); }
}  // namespace me::hot::n_82219360

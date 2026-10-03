// sub_82533028 - loop over an index array with a float compare and swap-remove (virtual method, no direct call sites).
//
//   r3 = object: [+112] count n (u32), [+48] u16 index array, [+108] element stride (s32), [+44] element base,
//        [+100] offset added to the element address for the "clear" block.
//   Statics: float [0x820BCD64] threshold, float [0x820B90B8] clear value (read once, before the loop).
//   for i = n-1 down to 0 (n == 0 or n > 0x80000000 does nothing, as the original's signed 32-bit test on n - 1):
//     idx = array[i]; e = stride * idx + base; if (float [e+12] > threshold)   (NaN: false)
//        { float [[+100] + e + 0..16] = clear value (5 words, raw copy: the recompiled lfs/stfs pairs are float->double->float
//          round trips that GCC folds, checked in the disassembly of the built ELF);
//          array[i] = array[n_now - 1]; array[n_now - 1] = idx; n_now = n_now - 1 }
// Every load after a store is re-issued as in the original (the arrays / the object may alias); on the common no-removal
// path the three object fields are cached (no store happened since they were read). The FPU flush mode is cleared once
// (idempotent; the original calls disableFlushMode at every lfs, after the early return of n == 0).
// Registers: no return value; r3 unchanged; every other volatile register the original leaves behind is dead scratch
// (no direct call site; indirect callers follow the ABI).
// Writes(): [+112], the index array [+48, +2n), and the span of all element clear blocks that any array entry could address
// (computed from the initial array; swap-remove only permutes the first n entries). Overflow if that span is huge.
#pragma once

#include "../me_hot_common.h"

namespace me::hot::n_82533028 {

inline constexpr Cmp kCmp = {R(3), 0, 0};

inline constexpr uint32_t kConsts = 0x820B0000u + 0x90B8u;  // base of the float constants (0x820B90B8)
inline constexpr uint32_t kThreshold = kConsts + 15532u;     // 0x820BCD64
inline constexpr uint32_t kClear = kConsts + 0u;

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t o = ctx.r3.u32;
  int32_t i = int32_t(Ld32(base, o + 112) - 1u);
  if (i < 0) return;
  ctx.fpscr.disableFlushMode();
  const float thr = LdF32(base, kThreshold);
  const uint32_t clear = Ld32(base, kClear);  // raw bits
  uint32_t r8 = uint32_t(i) << 1;             // byte offset of array[i]
  uint32_t arr = Ld32(base, o + 48);
  uint32_t stride = Ld32(base, o + 108);
  uint32_t ebase = Ld32(base, o + 44);
  for (;;) {
    const uint32_t idx = Ld16(base, arr + r8);
    const uint32_t e = stride * idx + ebase;
    if (LdF32(base, e + 12) > thr) {
      const uint32_t p = Ld32(base, o + 100) + e;
      St32(base, p + 0, clear);
      St32(base, p + 4, clear);
      St32(base, p + 8, clear);
      St32(base, p + 12, clear);
      St32(base, p + 16, clear);
      uint32_t n = Ld32(base, o + 112);
      uint32_t a = Ld32(base, o + 48);
      const uint16_t t = Ld16(base, ((n << 1) + a) - 2u);
      St16(base, r8 + a, t);
      n = Ld32(base, o + 112);
      a = Ld32(base, o + 48);
      St16(base, ((n << 1) + a) - 2u, uint16_t(idx));
      n = Ld32(base, o + 112);
      St32(base, o + 112, n - 1u);
      // reload what the stores may have changed
      arr = Ld32(base, o + 48);
      stride = Ld32(base, o + 108);
      ebase = Ld32(base, o + 44);
    }
    if (--i < 0) return;
    r8 -= 2;
  }
}

inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) {
  const uint32_t o = ctx.r3.u32;
  const uint32_t n = Ld32(base, o + 112);
  if (n == 0 || int32_t(n - 1u) < 0) return;
  if (n > 16384) {
    w.overflow = true;
    return;
  }
  const uint32_t arr = Ld32(base, o + 48), stride = Ld32(base, o + 108), ebase = Ld32(base, o + 44), off = Ld32(base, o + 100);
  w.Add(o + 112, 4);
  w.Add(arr, n * 2u);
  uint32_t lo = 0xFFFFFFFFu, hi = 0;
  for (uint32_t k = 0; k < n; ++k) {
    const uint32_t p = stride * uint32_t(Ld16(base, arr + 2 * k)) + ebase + off;
    if (p < lo) lo = p;
    if (p > hi) hi = p;
    if (p > 0xFFFFFFFFu - 20) {  // the 5-word block would wrap around the address space
      w.overflow = true;
      return;
    }
  }
  if (uint64_t(hi) + 20 - lo > Writes::kMaxBytes) {
    w.overflow = true;
    return;
  }
  w.Add(lo, hi + 20 - lo);
}

}  // namespace me::hot::n_82533028

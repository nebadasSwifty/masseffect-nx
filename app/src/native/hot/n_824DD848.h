// sub_824DD848 - UE3 particle distribution (float table) lookup with linear interpolation and an optional random
// row selector / random lerp driven by a global LCG word.
//
//   r3 = distribution object: [+1] u8 type (3: random row, 2: random lerp between the next sample), [+2] u8 x [+3] u8
//        table dimensions, [+4] table pointer, [+8] u32 size, [+16] float step, [+20] float minimum.
//   f1 = time (double), r5 = output float array, r6 = signed count.
//   The time is mapped to a row q = trunc((f1 - min) / step) (clamped to 0..last), the fraction is interpolated between
//   two rows by 'fmadds'; the global LCG state [0x82EAC8C0] (seed * 0x0BB38435 + 0x3619636B) is advanced exactly where
//   the original advances it (once if type == 3 before the loop, once per element for type == 2).
//
// Exactness notes:
//  * disableFlushMode() is the first instruction of the original (idempotent), so it is called once up front.
//  * The recompiled double(float(...)) expressions are copied as written (single rounding after every PPC "s" op,
//    std::fma where the PPC code has fmadds/fnmsubs, simde_mm_cvttsd_si32 for fctiwz with the generated saturation).
//  * The lfs/stfs pass-through of an unmodified table entry (no interpolation) is a raw word copy, as GCC compiled the
//    original on the Switch (the fuzzer's table data therefore contains no signalling NaNs: clang on the Mac keeps the
//    quieting round trip of the original).
//  * The memory operations keep the original order and re-read the object bytes every iteration (the output array may
//    alias anything); the RNG word is read-modified-written at the same points.
//  * The two PPC trap instructions (twllei r10,0 divisor zero check; twlgei r7,-1 overflow check) only log a
//    warning in ppc_trap; they are omitted here (the divw result for a zero divisor is 0 as in the generated code).
//    The condition is a degenerate table (x * y == 0).
//  * Registers: r3/f1 unchanged (return regs compared); r4-r12, f0-f13 and ctx.lr/r12 (the original leaves
//    lr = 0x824DD850) are dead scratch: tests/hot_fuzz/liveness.py shows no direct caller reads a volatile register
//    after the call, and indirect callers follow the ABI. Stack scratch (-64..-48 of r1) is not written.
#pragma once

#include <cmath>
#include <climits>

#include "../me_hot_common.h"

namespace me::hot::n_824DD848 {

inline constexpr Cmp kCmp = kCmpRet;

inline constexpr uint32_t kRng = 0x82EAC8C0u;     // lis -32021 (0x82EB0000) - 14144
inline constexpr uint32_t kCstF12 = 0x820B90B8u;  // r29 - 15388, r29 = lis -32244 (0x820C0000) - 13100 = 0x820BCCD4
inline constexpr uint32_t kCstThr = 0x820BCCD4u;  // r29 + 0
inline constexpr uint32_t kMul = (2995u << 16) | 33845u;
inline constexpr uint32_t kAdd = (13849u << 16) | 25451u;

// Division with the NaN operand order of the PPC instruction (a first): the compiler must not reorder or fold it.
inline double Div(double a, double b) {
#if defined(__aarch64__)
  double r;
  asm("fdiv %d0, %d1, %d2" : "=w"(r) : "w"(a), "w"(b));
  return r;
#else
  return a / b;
#endif
}

inline float BitsToF(uint32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

// one LCG step on the guest word; returns the random mantissa pattern 1.0f + [0,1)
inline uint32_t Step(uint8_t* base) {
  const uint32_t s = Ld32(base, kRng) * kMul + kAdd;
  St32(base, kRng, s);
  return (s & 0x7FFFFFu) | 0x3F800000u;
}

inline void Native(PPCContext& ctx, uint8_t* base) {
  ctx.fpscr.disableFlushMode();
  const uint32_t obj = ctx.r3.u32;
  const double t = ctx.f1.f64;
  double f0 = double(LdF32(base, obj + 20));
  uint32_t tab, r28 = 0;
  double f12 = 0.0;
  bool lookup = false;
  double f13 = 0.0;
  if (t > f0) {
    f13 = double(LdF32(base, obj + 16));
    lookup = !(f13 == double(LdF32(base, kCstF12)));
  }
  if (lookup) {
    f0 = double(float(t - f0));
    f13 = double(float(f0 / f13));
    int64_t q64 = std::isnan(f13)                    ? int64_t(0x80000000U)
                  : (f13 >= double(INT_MAX))         ? INT_MAX
                                                     : simde_mm_cvttsd_si32(simde_mm_load_sd(&f13));
    int32_t q = int32_t(uint32_t(q64));
    if (q < 0) q = 0;
    const uint32_t n = uint32_t(Ld8(base, obj + 2)) * uint32_t(Ld8(base, obj + 3));  // 0..65025
    const uint32_t r7 = Ld32(base, obj + 8) - 2u;
    const int32_t div = n ? int32_t(r7) / int32_t(n) : 0;
    const int32_t lim = int32_t(uint32_t(div) - 1u);
    const uint32_t p = Ld32(base, obj + 4);
    if (q < lim) {
      f13 = double(LdF32(base, obj + 16));
      r28 = (((uint32_t(q) + 1u) * n + 2u) << 2) + p;
      f12 = double(float(double(int64_t(q))));
      // fnmsubs: negate AFTER the fma (the sign of a NaN result matters); keep the compiler from folding the negation
      // into the fma (fmsub would not negate a NaN coming from f13)
      double m = std::fma(f12, f13, -f0);
#if defined(__aarch64__)
      asm("" : "+w"(m));
#endif
      f0 = double(float(-m));
      f12 = double(float(Div(f0, f13)));
    } else {
      q = lim;
    }
    tab = ((n * uint32_t(q) + 2u) << 2) + p;
  } else {
    tab = Ld32(base, obj + 4) + 8u;
  }

  uint32_t r4 = 0;
  if (Ld8(base, obj + 1) == 3) {
    const float fr = BitsToF(Step(base));
    // fctiwz(fr) == 1 for fr in [1, 2): fr - 1.0 is exact
    const double d = double(float(double(fr) - 1.0));
    if (d > double(LdF32(base, kCstThr))) r4 = 1;
  }

  const int32_t cnt = ctx.r6.s32;
  uint32_t dst = ctx.r5.u32;
  for (int32_t i = 0; i < cnt; ++i, dst += 4) {
    const uint32_t idx = (uint32_t(Ld8(base, obj + 3)) * r4 + uint32_t(i)) << 2;
    if (r28) {
      double v = double(LdF32(base, idx + tab));
      double w = double(LdF32(base, idx + r28));
      w = double(float(w - v));
      v = double(float(std::fma(w, f12, v)));
      StF32(base, dst, float(v));
    } else {
      // lfs + stfs: the Switch build (GCC) folds the float -> double -> float round trip into a raw 32-bit copy
      // (checked in the disassembly of the built ELF), i.e. a signalling NaN is NOT quieted.
      St32(base, dst, Ld32(base, idx + tab));
    }
    if (Ld8(base, obj + 1) == 2) {
      const uint32_t idx2 = (uint32_t(Ld8(base, obj + 3)) + uint32_t(i)) << 2;
      double a;
      if (r28 == 0) {
        a = double(LdF32(base, idx2 + tab));
      } else {
        const double b = double(LdF32(base, idx2 + tab));
        a = double(LdF32(base, idx2 + r28));
        a = double(float(a - b));
        a = double(float(std::fma(a, f12, b)));
      }
      const uint32_t rb = Step(base);
      const double cur = double(LdF32(base, dst));
      const double diff = double(float(a - cur));
      const double fr = double(float(double(BitsToF(rb)) - 1.0));
      StF32(base, dst, float(double(float(std::fma(fr, diff, cur)))));
    }
  }
}

inline void Writes(const PPCContext& ctx, const uint8_t*, me::hot::Writes& w) {
  w.Add(kRng, 4);
  const int32_t cnt = ctx.r6.s32;
  if (cnt > 0) {
    if (uint64_t(cnt) * 4 > me::hot::Writes::kMaxBytes) {
      w.overflow = true;
      return;
    }
    w.Add(ctx.r5.u32, uint32_t(cnt) * 4u);
  }
}

}  // namespace me::hot::n_824DD848

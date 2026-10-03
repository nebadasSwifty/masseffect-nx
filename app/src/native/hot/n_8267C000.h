// sub_8267C000 - swept test of a moving segment/interval against a slab: projects the axis r6 on three vectors
// (r4, r5, r3), builds the interval [min - f3, max + f3] of the projections and intersects it with the time range
// [f1, f2]; updates a running best time and a best normal.
//
//   r3, r4, r5 = float3 vectors, r6 = float3 axis, f1 = t0, f2 = t1, f3 = margin (a full double),
//   r10 -> float best time (compare / update), stack args: [r1 + 84] -> float (running far time, min-updated),
//   [r1 + 92] -> float3 (receives r6 or -r6 when the best time improves).
//   Returns r3 = 1 if the interval overlaps, else 0.
//
// Exactness notes:
//  * ctx.fpscr.disableFlushMode() is the first instruction of the original (and repeated, idempotent): done once.
//  * All arithmetic is copied from the generated code (double(float(...)) forms, std::fma where the PPC code has fmadds,
//    fsel as "x >= 0.0 ? a : b", fcmpu as the C++ relational operators: NaNs make every ordered comparison false).
//  * -r6 is written as float(-double(w)) in the original: a signalling NaN word is quieted, the sign flipped (the Switch
//    GCC output keeps the fcvt pair here, checked in the disassembly); FlipQuiet() does exactly that. The unchanged
//    r6 words are copied raw (lwz/stw).
//  * The final "fsel + stfs" into [r1 + 84] stores either the arithmetic result or - when the select keeps the loaded
//    value - the word it just loaded from the same address (GCC folded that float -> double -> float trip into a raw
//    copy, checked in the disassembly): no memory change, so no store here. (The fuzz data for that word has no
//    signalling NaNs, which clang's unfolded original would quiet.)
//  * The f29-f31 spills (stfd ... -24(r1)) and the -48..-40(r1) temporaries are callee-owned stack scratch (below r1).
//
// Registers: r3 (compared) is the result; f1 is returned unchanged; r4-r12, f0-f13 are dead scratch (liveness.py: the
// single direct call site reads no volatile register after the call).
#pragma once

#include <cmath>

#include "../me_hot_common.h"

namespace me::hot::n_8267C000 {

inline constexpr Cmp kCmp = kCmpRet;

inline constexpr uint32_t kEps = 0x821C0000u - 6936u;      // lfd: double epsilon on |f2 - f1|
inline constexpr uint32_t kThr = 0x820C0000u - 28488u;     // lfs: float threshold (lis -32244 / addi -28488)
inline constexpr uint32_t kScale = kThr + 15532u;          // lfs: float numerator of the reciprocal

inline double F(const uint8_t* base, uint32_t a) { return double(LdF32(base, a)); }

// float(-double(w)) for the float bit pattern w
inline uint32_t FlipQuiet(uint32_t w) {
  if ((w & 0x7F800000u) == 0x7F800000u && (w & 0x7FFFFFu)) w |= 0x400000u;
  return w ^ 0x80000000u;
}

inline void Native(PPCContext& ctx, uint8_t* base) {
  ctx.fpscr.disableFlushMode();
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32, r10 = ctx.r10.u32, sp = ctx.r1.u32;
  const double f1 = ctx.f1.f64;
  const double f8 = double(float(ctx.f2.f64 - f1));
  const double f3 = ctx.f3.f64;

  const double z6 = F(base, r6 + 8), z4 = F(base, r4 + 8), z5 = F(base, r5 + 8), z3 = F(base, r3 + 8);
  const double y6 = F(base, r6 + 4), y4 = F(base, r4 + 4), y5 = F(base, r5 + 4), y3 = F(base, r3 + 4);
  const double x6 = F(base, r6 + 0), x4 = F(base, r4 + 0), x5 = F(base, r5 + 0), x3 = F(base, r3 + 0);
  double eps;
  {
    const uint64_t b = Ld64(base, kEps);
    std::memcpy(&eps, &b, 8);
  }
  const bool par = std::fabs(f8) < eps;

  double a = double(float(z4 * z6));
  double b = double(float(z5 * z6));
  double c = double(float(z3 * z6));
  a = double(float(std::fma(y4, y6, a)));
  b = double(float(std::fma(y5, y6, b)));
  c = double(float(std::fma(y3, y6, c)));
  const double p4 = double(float(std::fma(x6, x4, a)));
  const double p5 = double(float(std::fma(x6, x5, b)));
  const double p3 = double(float(std::fma(x6, x3, c)));
  const double d = double(float(p4 - p5));
  const double mn = d >= 0.0 ? p5 : p4;
  const double mx = d >= 0.0 ? p4 : p5;
  const double e1 = double(float(p3 - mn));
  const double e2 = double(float(p3 - mx));
  const double lo = e1 >= 0.0 ? mn : p3;
  const double hi = e2 >= 0.0 ? p3 : mx;
  const double f12 = double(float(lo - f3));
  const double f11 = double(float(hi + f3));

  if (par) {
    ctx.r3.u64 = (f1 < f12) ? 0 : ((f1 > f11) ? 0 : 1);
    return;
  }

  const double thr = F(base, kThr);
  const double nlo = double(float(f12 - f1));
  double f0 = double(float(F(base, kScale) / f8));
  double f13;
  uint32_t w0, w1, w2;
  if (f8 > thr) {
    const double nhi = double(float(f11 - f1));
    w0 = FlipQuiet(Ld32(base, r6 + 0));
    w1 = FlipQuiet(Ld32(base, r6 + 4));
    w2 = FlipQuiet(Ld32(base, r6 + 8));
    f13 = double(float(nlo * f0));
    f0 = double(float(nhi * f0));
  } else {
    f13 = double(float(f11 - f1));
    w0 = Ld32(base, r6 + 0);
    w1 = Ld32(base, r6 + 4);
    w2 = Ld32(base, r6 + 8);
    f13 = double(float(f13 * f0));
    f0 = double(float(nlo * f0));
  }

  if (f13 > F(base, r10)) {
    const uint32_t o = Ld32(base, sp + 92);
    StF32(base, r10, float(f13));
    St32(base, o + 0, w0);
    St32(base, o + 4, w1);
    St32(base, o + 8, w2);
  }
  const uint32_t o2 = Ld32(base, sp + 84);
  const double cur = F(base, r10);
  const double g = F(base, o2);
  double res;
  if (double(float(g - f0)) >= 0.0) {
    res = f0;
    StF32(base, o2, float(f0));
  } else {
    res = g;  // fsel keeps the loaded value: the original stores the word it just read (no change)
  }
  ctx.r3.u64 = (res < cur) ? 0 : ((res < thr) ? 0 : 1);
}

inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) {
  w.Add(ctx.r10.u32, 4);
  w.Add(Ld32(base, ctx.r1.u32 + 92), 12);
  w.Add(Ld32(base, ctx.r1.u32 + 84), 4);
}

}  // namespace me::hot::n_8267C000

// sub_8256AB90 - UE3 ray / slab clip against a set of vertices projected on the ray direction (leaf, FPU only).
// Russian edition copy: only the guest constants differ from the English file (data addresses translated through the
// generated code of the RU function, which is instruction-for-instruction the same as the English one).
//
//   r3 -> { u32 vertices (array of float[3], 12-byte stride), s32 count }   (vertex array read twice: loads only)
//   r5 -> float[3] direction d;   f1, f2, f3 = (double) ray parameters t, t_end and padding;
//   r9 -> float tmin_out, r10 -> float tmax_out (read, and conditionally updated);
//   [r1 + 84] -> float[3] normal for the near side, [r1 + 92] -> float[3] normal for the far side (caller's argument area).
//   Returns r3 = 0 / 1.
//
// Step 1 (loop): lo = min(v . d), hi = max(v . d) over the vertices (fmuls, two fmadds [double fma, then frsp], fsel).
// Step 2: ext = f2 - f1 (frsp), lo -= f3, hi += f3 (frsp). If |ext| < K1 the "parallel" branch tests f1 against [lo, hi]
// and may write the global vector G1 = -d (or +d) plus the flag word G2 = 1; otherwise (or when |ext| >= K2) the "slab"
// branch scales (lo - f1, hi - f1) by K3 / ext, and updates *r9 / *r10 and the two normals with +-d.
//
// Exactness notes:
//  * The original calls ctx.fpscr.disableFlushMode() before the first FPU op on every path and never enables it, so
//    one call at the start leaves the same csr / hardware FPCR.
//  * All vertex / direction / constant values are floats (lfs), so +,-,*,/ in float equal the recompiled
//    double-op-then-frsp (double rounding is innocuous since 53 >= 2 * 24 + 2); the fmadds keep the double fma (double
//    rounding is not innocuous for fma). f1, f2, f3 are arbitrary doubles and are used as doubles exactly as the original.
//  * lfs -> fneg -> stfs of the original is float(-double(x)): a NaN input comes out quieted (signalling NaN -> quiet
//    NaN), emulated bit-exactly by NegQ (GCC does not fold that round trip, see the disassembly of the built ELF).
//  * The stack spills (-48(r1).. -16(r1)) are callee-owned scratch: kept in locals. Memory read/write order of everything
//    that touches caller memory is the original's (outputs may alias each other or the inputs).
//  * Volatile scratch left behind (r0, r4-r8, r11, r30/r31 spills, f0, f7-f13) is not reproduced: liveness.py shows no
//    caller reads it; r3 is the return value, f1 and the rest are untouched.
#pragma once

#include "../me_hot_common.h"

namespace me::hot::n_8256A1A0 {

inline constexpr Cmp kCmp = kCmpRet;

inline constexpr uint32_t kMinInit = 0x821BE0E4u;  // float: initial lo (lis r8,-32228; -7964)
inline constexpr uint32_t kMaxInit = 0x821BE34Cu;  // float: initial hi (-7348)
inline constexpr uint32_t kK1 = 0x820BCE24u;       // float (lis r11,-32244; addi r6,r11,-28520; 15756(r6))
inline constexpr uint32_t kK3 = 0x820BCD44u;       // float, 15532(r6)
inline constexpr uint32_t kK4 = 0x820B9098u;       // float, 0(r6)
inline constexpr uint32_t kK2 = 0x821BE4B8u;       // double (lfd -6984(r11))
inline constexpr uint32_t kG1 = 0x82EC3134u;       // 3 words written by the parallel branch
inline constexpr uint32_t kFlag = 0x82EAF530u;     // word set to 1 together with G1

// float(-double(w)) on the bit pattern of a float: sign flipped, a signalling NaN is quieted.
inline uint32_t NegQ(uint32_t w) {
  uint32_t r = w ^ 0x80000000u;
  if ((w & 0x7FFFFFFFu) > 0x7F800000u) r |= 0x00400000u;
  return r;
}
// Keeps the compiler from turning "!(a < b) && !(a < c)" into a >= fmaxnm(b, c), which differs when b is a NaN (seen with
// clang -O2 on arm64: LLVM combines the two compares as if no NaN existed). Emits no instruction.
inline bool Opaque(bool x) {
#if defined(__aarch64__)
  __asm__("" : "+r"(x));
#endif
  return x;
}
inline uint32_t Bits(float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  return v;
}

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t lst = ctx.r3.u32, dir = ctx.r5.u32, o9 = ctx.r9.u32, o10 = ctx.r10.u32, sp = ctx.r1.u32;
  const int32_t cnt = int32_t(Ld32(base, lst + 4));
  ctx.fpscr.disableFlushMode();
  float lo = LdF32(base, kMinInit);  // f13
  float hi = LdF32(base, kMaxInit);  // f12
  if (cnt > 0) {
    const float dz = LdF32(base, dir + 8);
    uint32_t p = Ld32(base, lst);
    const float dy = LdF32(base, dir + 4);
    const float dx = LdF32(base, dir);
    const double ddy = dy, ddx = dx;
    uint32_t n = uint32_t(cnt);
    do {
      const float pz = LdF32(base, p + 8);
      const float py = LdF32(base, p + 4);
      const float px = LdF32(base, p);
      p += 12;
      float d = pz * dz;
      d = float(std::fma(double(py), ddy, double(d)));
      d = float(std::fma(double(px), ddx, double(d)));
      const float s1 = d - lo;
      const float s2 = d - hi;
      lo = (s1 >= 0.0f) ? lo : d;
      hi = (s2 >= 0.0f) ? d : hi;
    } while (--n);
  }
  const double t = ctx.f1.f64, tend = ctx.f2.f64, pad = ctx.f3.f64;
  const float ext = float(tend - t);          // f11
  lo = float(double(lo) - pad);               // f13
  hi = float(double(hi) + pad);               // f12
  const float k1 = LdF32(base, kK1);          // f0
  const float aext = __builtin_fabsf(ext);    // f10
  bool slab = !(aext < k1);                   // bge cr6 -> 8256A2F0
  if (!slab) {
    if (t < double(lo)) {
      const float f9 = float(double(lo) - double(k1));
      if (t > double(f9)) {
        const uint32_t x = Ld32(base, dir), y = Ld32(base, dir + 4), z = Ld32(base, dir + 8);
        St32(base, kG1, NegQ(x));
        St32(base, kG1 + 4, NegQ(y));
        St32(base, kFlag, 1);
        St32(base, kG1 + 8, NegQ(z));
      }
    }
    if (t > double(hi)) {
      const float k = float(double(hi) + double(k1));
      if (t < double(k)) {
        const uint32_t a = Ld32(base, dir);
        St32(base, kG1, a);
        St32(base, kFlag, 1);
        const uint32_t b = Ld32(base, dir + 4);
        St32(base, kG1 + 4, b);
        const uint32_t c = Ld32(base, dir + 8);
        St32(base, kG1 + 8, c);
      }
    }
    uint64_t k2b = Ld64(base, kK2);
    double k2;
    std::memcpy(&k2, &k2b, 8);
    if (double(aext) < k2) {
      ctx.r3.u64 = (t < double(lo) || t > double(hi)) ? 0 : 1;
      return;
    }
  }
  // 8256A2F0
  const float k3 = LdF32(base, kK3);
  const float hi2 = float(double(hi) - t);
  const float k4 = LdF32(base, kK4);
  const float inv = float(double(k3) / double(ext));
  float t13, t0;
  uint32_t w8, w7, w6;
  if (double(ext) > double(k4)) {
    const float lo2 = float(double(lo) - t);
    const uint32_t x = Ld32(base, dir), y = Ld32(base, dir + 4), z = Ld32(base, dir + 8);
    w8 = NegQ(x);
    w7 = NegQ(y);
    w6 = NegQ(z);
    t13 = lo2 * inv;
    t0 = hi2 * inv;
  } else {
    const float lo2 = float(double(lo) - t);
    w8 = Ld32(base, dir);
    t13 = hi2 * inv;
    w7 = Ld32(base, dir + 4);
    w6 = Ld32(base, dir + 8);
    t0 = lo2 * inv;
  }
  const float cur9 = LdF32(base, o9);
  if (t13 > cur9) {
    const uint32_t p84 = Ld32(base, sp + 84);
    St32(base, o9, Bits(t13));
    St32(base, p84, w8);
    St32(base, p84 + 4, w7);
    St32(base, p84 + 8, w6);
  }
  const float cur10 = LdF32(base, o10);
  if (t0 < cur10) {
    const uint32_t p92 = Ld32(base, sp + 92);
    St32(base, o10, Bits(t0));
    St32(base, p92, NegQ(w8));
    St32(base, p92 + 4, NegQ(w7));
    St32(base, p92 + 8, NegQ(w6));
  }
  const float f0 = LdF32(base, o10);
  const float f13 = LdF32(base, o9);
  const bool lt = Opaque(f0 < f13);
  ctx.r3.u64 = lt ? 0 : ((f0 < k4) ? 0 : 1);
}

inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) {
  w.Add(kG1, 12);
  w.Add(kFlag, 4);
  w.Add(ctx.r9.u32, 4);
  w.Add(ctx.r10.u32, 4);
  w.Add(Ld32(base, ctx.r1.u32 + 84), 12);
  w.Add(Ld32(base, ctx.r1.u32 + 92), 12);
}

}  // namespace me::hot::n_8256A1A0

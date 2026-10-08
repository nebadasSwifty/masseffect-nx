// sub_8256B9C8 - UE3 ray vs. projected vertex interval, "sphere/ray" variant of sub_8256AB90 (leaf, FPU only).
// Russian edition copy: only the guest constants differ from the English file (data addresses translated through the
// generated code of the RU function, which is instruction-for-instruction the same as the English one).
//
//   r3 -> { u32 vertices (float[3], 12-byte stride), s32 count }, r4 -> float[3] direction d, f1 = t, f2 = pad (doubles),
//   r7 -> float s (in/out), r8 -> float[3] (out). Returns r3 = 0 / 1.
//
//   lo = min(v . d), hi = max(v . d) (same loop as sub_8256AB90), lo' = lo - f2, hi' = hi + f2 (frsp);
//   t < lo' or t > hi'  ->  0.   Otherwise a = t - lo', b = hi' - t, inv = frsp(K / sqrt(d . d)) (K a double constant),
//   lim = s / inv (fdivs); if a < lim: *r7 = a * inv and r8[] = -d * inv; if b < lim: *r7 = b * inv, r8[] = d * inv; -> 1.
//
// Exactness notes: as in n_8256A1A0.h (one disableFlushMode at the start matches the original: it clears the flush mode
// before the first FPU op on every path and nothing enables it again; +,-,*,/ of float-valued operands in float equal the
// recompiled double-op + frsp; fmadds stay a double fma; sqrt and the K / sqrt division are done in double as the
// original; f1, f2 are arbitrary doubles). The stack spill words (-16(r1)..-8(r1)) are locals. The direction is read again
// (after the stores of the first block) exactly where the original reloads it, outputs may alias it.
// Volatile scratch (r4-r11, f0, f7-f13) not reproduced: no caller reads it (liveness.py); r3 = return value.
#pragma once

#include "../me_hot_common.h"

namespace me::hot::n_8256AFD8 {

inline constexpr Cmp kCmp = kCmpRet;

inline constexpr uint32_t kMinInit = 0x821BE0E4u;  // float
inline constexpr uint32_t kMaxInit = 0x821BE34Cu;  // float
inline constexpr uint32_t kK = 0x821BE198u;        // double (lfd -7784(r11), r11 = lis -32228)

inline uint32_t Bits(float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  return v;
}

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t lst = ctx.r3.u32, dir = ctx.r4.u32, io = ctx.r7.u32, out = ctx.r8.u32;
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
  const double t = ctx.f1.f64, pad = ctx.f2.f64;
  const float lo2 = float(double(lo) - pad);  // f0
  const float hi2 = float(double(hi) + pad);  // f12
  if (t < double(lo2) || t > double(hi2)) {
    ctx.r3.u64 = 0;
    return;
  }
  const float a = float(t - double(lo2));  // f11
  const float dz = LdF32(base, dir + 8);
  const float b = float(double(hi2) - t);  // f8
  const float dx = LdF32(base, dir);
  const float dy = LdF32(base, dir + 4);
  const float cur = LdF32(base, io);  // f10
  float q = dz * dz;
  q = float(std::fma(double(dy), double(dy), double(q)));
  q = float(std::fma(double(dx), double(dx), double(q)));
  const double rt = std::sqrt(double(q));
  uint64_t kb = Ld64(base, kK);
  double kd;
  std::memcpy(&kd, &kb, 8);
  const float inv = float(kd / rt);
  const float lim = float(double(cur) / double(inv));  // f12
  if (a < lim) {
    const float ny = -LdF32(base, dir + 4);
    const float nz = -LdF32(base, dir + 8);
    const float nx = -dx;
    const float r = a * inv;
    St32(base, io, Bits(r));
    const uint32_t w0 = Bits(nx * inv), w1 = Bits(ny * inv), w2 = Bits(nz * inv);
    St32(base, out, w0);
    St32(base, out + 4, w1);
    St32(base, out + 8, w2);
  }
  if (b < lim) {
    const float x = LdF32(base, dir);
    const float x1 = x * inv;
    const float y = LdF32(base, dir + 4);
    const float z = LdF32(base, dir + 8);
    const float y1 = inv * y;
    const float z1 = z * inv;
    const float r = b * inv;
    St32(base, io, Bits(r));
    St32(base, out, Bits(x1));
    St32(base, out + 4, Bits(y1));
    St32(base, out + 8, Bits(z1));
  }
  ctx.r3.u64 = 1;
}

inline void Writes(const PPCContext& ctx, const uint8_t*, me::hot::Writes& w) {
  w.Add(ctx.r7.u32, 4);
  w.Add(ctx.r8.u32, 12);
}

}  // namespace me::hot::n_8256AFD8

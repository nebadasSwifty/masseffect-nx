// sub_822631E8 - 4x4 float matrix inverse: r3 = destination (16 floats), r4 = source matrix (16 floats, row major).
//
//   det = sub_822634D8(src) (cofactor expansion; a leaf, inlined here: it only reads the 16 source floats and spills
//   f28-f31 to its own stack frame). If det == 0.0 (fcmpu equal; NaN is unordered => not equal) the destination is
//   overwritten with the 64-byte constant block at 0x82E64F30 (the identity matrix, read from guest memory), else
//   the adjugate scaled by 1.0f / det (fdivs; the constants are read from guest memory) is stored (negations as the
//   original's fneg). Returns r3 (unchanged, all 64 bits).
// Exactness: the original's float arithmetic is copied operation by operation (fmuls = double(float(a * b));
// fmsubs/fmadds/fnmsubs = double(float(std::fma(...))), a double fma rounded to float; sources are loaded as
// double(float)). FPU mode: both paths call disableFlushMode() before the first FP op (the callee already did) -> once.
// All 16 results are computed from the source before any destination byte is written (so dst == src works, as in the
// original, which writes the results to a stack temporary and copies it in 8-byte steps).
// Not reproduced (dead scratch, checked by liveness.py: callers only read r3 after the call): f0-f13, r0, r4-r12,
// and the stack scratch below r1 (the 64-byte temporary, the callee's f28-f31 spills, lr/back-chain words).
// Residual difference (impossible in practice): a destination that points into the callee-owned stack frame itself.
// NaN results: the sign and payload of a NaN depend on which operand the FPU returns when NaNs meet (AArch64 returns
// the first NaN operand of the instruction the compiler emitted; a fresh invalid-operation NaN is +default NaN and
// fneg flips it), so they are only reproducible bit-exactly by running the very same compiled code. Whenever the
// determinant or any of the 16 results is NaN (NaN inputs, inf*0, inf-inf, overflow to +-inf followed by a
// subtraction), nothing has been written yet and the call is declined (returns false: the hook runs the original,
// __imp__sub_822631E8; declining instead of calling it from here keeps the self-check guard's shadow run free of guest
// stores). NaN-free results are fully determined by IEEE rounding, which both paths share.
#pragma once

#include <cmath>

#include "../me_hot_common.h"

namespace me::hot::n_822631E8 {

inline constexpr Cmp kCmp = {R(3), 0, 0};

inline constexpr uint32_t kR = uint32_t(-2113142784 + -12956);   // r11 of the original: 1.0f at kR, compare constant at kR-15532
inline constexpr uint32_t kIdent = uint32_t(-2098855936 + 20272);  // 64 bytes copied if det == 0

inline bool Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32;
  const uint32_t m = ctx.r4.u32;
  auto ld = [&](uint32_t k) { return double(LdF32(base, m + k)); };
  ctx.fpscr.disableFlushMode();
  double d0, d1, d2, d3, d4, d5, d6, d7, d8, d9, d10, d11, d12, d13, d26, d27, d28, d29, d30, d31;
    d12 = ld(56);
    d11 = ld(44);
    d9 = ld(12);
    d2 = double(float(d11 * d12));
    d28 = double(float(d9 * d12));
    d13 = ld(40);
    d7 = ld(28);
    d26 = double(float(d9 * d13));
    d8 = ld(24);
    d27 = double(float(d7 * d12));
    d0 = ld(60);
    d9 = double(float(d9 * d8));
    d10 = ld(8);
    d6 = ld(36);
    d5 = ld(4);
    d4 = ld(20);
    d3 = ld(52);
    d12 = double(float(std::fma(d13, d0, -d2)));
    d1 = ld(16);
    d2 = double(float(std::fma(d10, d0, -d28)));
    d31 = ld(0);
    d28 = double(float(d7 * d13));
    d30 = ld(32);
    d13 = double(float(std::fma(d10, d11, -d26)));
    d29 = ld(48);
    d10 = double(float(std::fma(d10, d7, -d9)));
    d0 = double(float(std::fma(d8, d0, -d27)));
    d9 = double(float(d2 * d6));
    d11 = double(float(std::fma(d8, d11, -d28)));
    d8 = double(float(d2 * d4));
    d2 = double(float(d13 * d4));
    d7 = double(float(d6 * d0));
    d9 = double(float(std::fma(d5, d12, -d9)));
    d0 = double(float(std::fma(d5, d0, -d8)));
    d12 = double(float(std::fma(d4, d12, -d7)));
    d13 = double(float(std::fma(d13, d3, d9)));
    d9 = double(float(std::fma(d5, d11, -d2)));
    d0 = double(float(std::fma(d10, d3, d0)));
    d12 = double(float(std::fma(d3, d11, d12)));
    d13 = double(float(d13 * d1));
    d11 = double(float(std::fma(d10, d6, d9)));
    d13 = double(float(std::fma(d12, d31, -d13)));
    d0 = double(float(std::fma(d0, d30, d13)));
    d1 = double(float(-std::fma(d11, d29, -d0)));
  if (d1 != d1) [[unlikely]] {  // NaN determinant: every result is NaN, see the header
    return false;
  }
  if (d1 == double(LdF32(base, kR - 15532))) {
    // singular: copy the constant block, 8 bytes at a time exactly as the original (ld/std pairs, no byte swap needed)
    for (uint32_t i = 0; i < 64; i += 8) {
      uint64_t v;
      std::memcpy(&v, Raw(base, kIdent + i), 8);
      std::memcpy(Raw(base, dst + i), &v, 8);
    }
    return true;
  }
  double f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26,
      f27, f28, f29, f30, f31;
  float o[16];
  f1 = d1;  // the determinant
    f2 = ld(44);
    f3 = ld(56);
    f29 = ld(12);
    f6 = double(float(f2 * f3));
    f4 = ld(40);
    f26 = double(float(f29 * f3));
    f25 = double(float(f29 * f4));
    f5 = ld(60);
    f31 = ld(28);
    f30 = ld(8);
    f27 = double(float(f31 * f4));
    f0 = double(LdF32(base, kR));
    f0 = double(float(f0 / f1));
    f1 = ld(24);
    f12 = ld(36);
    f23 = double(float(f29 * f1));
    f13 = ld(20);
    f18 = double(float(f29 * f12));
    f11 = ld(4);
    f17 = double(float(f12 * f31));
    f28 = double(float(std::fma(f4, f5, -f6)));
    f10 = ld(52);
    f6 = double(float(f31 * f3));
    f8 = ld(32);
    f26 = double(float(std::fma(f30, f5, -f26)));
    f9 = ld(16);
    f25 = double(float(std::fma(f30, f2, -f25)));
    f7 = ld(48);
    f27 = double(float(std::fma(f1, f2, -f27)));
    f23 = double(float(std::fma(f30, f31, -f23)));
    f24 = double(float(std::fma(f1, f5, -f6)));
    f6 = double(float(f26 * f12));
    f21 = double(float(f25 * f13));
    f22 = double(float(f26 * f13));
    f20 = double(float(f12 * f24));
    f6 = double(float(std::fma(f11, f28, -f6)));
    f21 = double(float(std::fma(f11, f27, -f21)));
    f22 = double(float(std::fma(f11, f24, -f22)));
    f19 = double(float(f8 * f24));
    f20 = double(float(std::fma(f13, f28, -f20)));
    f6 = double(float(std::fma(f25, f10, f6)));
    f21 = double(float(std::fma(f23, f12, f21)));
    f22 = double(float(std::fma(f23, f10, f22)));
    f19 = double(float(std::fma(f9, f28, -f19)));
    f20 = double(float(std::fma(f10, f27, f20)));
    f6 = double(float(f6 * f0));
    f21 = double(float(f21 * f0));
    f22 = double(float(f22 * f0));
    o[2] = float(f22);
    f22 = double(float(f10 * f2));
    f19 = double(float(std::fma(f7, f27, f19)));
    f20 = double(float(f20 * f0));
    o[0] = float(f20);
    f6 = -f6;
    o[1] = float(f6);
    f6 = -f21;
    o[3] = float(f6);
    f21 = double(float(f29 * f10));
    f6 = ld(0);
    f20 = double(float(f10 * f31));
    f22 = double(float(std::fma(f12, f5, -f22)));
    f19 = double(float(f19 * f0));
    f21 = double(float(std::fma(f11, f5, -f21)));
    f5 = double(float(std::fma(f13, f5, -f20)));
    f20 = double(float(f29 * f13));
    f29 = double(float(std::fma(f11, f2, -f18)));
    f18 = double(float(f9 * f26));
    f26 = double(float(f8 * f26));
    f19 = -f19;
    o[4] = float(f19);
    f2 = double(float(std::fma(f13, f2, -f17)));
    f31 = double(float(std::fma(f11, f31, -f20)));
    f20 = double(float(f9 * f25));
    f24 = double(float(std::fma(f6, f24, -f18)));
    f28 = double(float(std::fma(f6, f28, -f26)));
    f26 = double(float(f8 * f21));
    f21 = double(float(f9 * f21));
    f19 = double(float(f9 * f29));
    f27 = double(float(std::fma(f6, f27, -f20)));
    f20 = double(float(f8 * f5));
    f24 = double(float(std::fma(f7, f23, f24)));
    f26 = double(float(std::fma(f6, f22, -f26)));
    f5 = double(float(std::fma(f6, f5, -f21)));
    f28 = double(float(std::fma(f7, f25, f28)));
    f27 = double(float(std::fma(f8, f23, f27)));
    f25 = double(float(std::fma(f9, f22, -f20)));
    f24 = double(float(f24 * f0));
    f29 = double(float(std::fma(f7, f29, f26)));
    f5 = double(float(std::fma(f7, f31, f5)));
    f28 = double(float(f28 * f0));
    o[5] = float(f28);
    f28 = double(float(f27 * f0));
    o[7] = float(f28);
    f28 = double(float(std::fma(f7, f2, f25)));
    f27 = -f24;
    o[6] = float(f27);
    f27 = double(float(f12 * f1));
    f2 = double(float(std::fma(f6, f2, -f19)));
    f29 = double(float(f29 * f0));
    f5 = double(float(f5 * f0));
    o[10] = float(f5);
    f28 = double(float(f28 * f0));
    o[8] = float(f28);
    f28 = double(float(f30 * f10));
    f5 = -f29;
    o[9] = float(f5);
    f5 = double(float(f10 * f4));
    f29 = double(float(f10 * f1));
    f10 = double(float(std::fma(f12, f3, -f5)));
    f5 = double(float(std::fma(f13, f3, -f29)));
    f29 = double(float(f30 * f12));
    f12 = double(float(std::fma(f11, f3, -f28)));
    f3 = double(float(f30 * f13));
    f13 = double(float(std::fma(f13, f4, -f27)));
    f4 = double(float(std::fma(f11, f4, -f29)));
    f11 = double(float(std::fma(f11, f1, -f3)));
    f3 = double(float(std::fma(f8, f31, f2)));
    f2 = double(float(f9 * f12));
    f12 = double(float(f8 * f12));
    f3 = double(float(f3 * f0));
    f12 = double(float(std::fma(f6, f10, -f12)));
    f3 = -f3;
    o[11] = float(f3);
    f3 = double(float(f8 * f5));
    f5 = double(float(std::fma(f6, f5, -f2)));
    f12 = double(float(std::fma(f7, f4, f12)));
    f3 = double(float(std::fma(f9, f10, -f3)));
    f9 = double(float(f9 * f4));
    f12 = double(float(f12 * f0));
    o[13] = float(f12);
    f10 = double(float(std::fma(f7, f13, f3)));
    f13 = double(float(std::fma(f6, f13, -f9)));
    f9 = double(float(std::fma(f7, f11, f5)));
    f10 = double(float(f10 * f0));
    f13 = double(float(std::fma(f8, f11, f13)));
    f12 = double(float(f9 * f0));
    f11 = -f10;
    o[12] = float(f11);
    f0 = double(float(f13 * f0));
    o[15] = float(f0);
    f0 = -f12;
    o[14] = float(f0);
  bool nan = false;
  for (int i = 0; i < 16; ++i) nan |= (o[i] != o[i]);
  if (nan) [[unlikely]] {  // NaN sign/payload is operand-order dependent, see the header
    return false;
  }
  uint32_t w[16];
  for (int i = 0; i < 16; ++i) std::memcpy(&w[i], &o[i], 4);
  for (int i = 0; i < 16; ++i) St32(base, dst + 4 * i, w[i]);
  return true;
}

inline void Writes(const PPCContext& ctx, const uint8_t*, me::hot::Writes& w) { w.Add(ctx.r3.u32, 64); }

}  // namespace me::hot::n_822631E8

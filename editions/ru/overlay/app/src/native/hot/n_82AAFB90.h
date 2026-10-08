// Russian edition copy: hooked as sub_82B2D4F0 (instruction-for-instruction the English function of the same
// namespace), only the .rdata addresses below differ. The namespace keeps the English name.
//
// sub_82AAFB90 - XAudio voice resampler: mono 16-bit PCM -> float, linear interpolation with a 32.32 fixed-point read
// position and a linear volume ramp (RU edition: sub_82B2D4F0, same instructions, other .rdata addresses).
//
//   r3 = source (big-endian s16 samples), r5 = destination (floats, written as 16-byte vectors), r6 = output samples,
//   r7 = voice state st:
//     st+0  source start (for the consumed-sample count)   st+4  source length (samples)   st+8  <- consumed (clamped)
//     st+13 byte: bytes per sample / 2 (divisor; 0 traps)  st+20 destination start          st+24 destination capacity
//     st+28 <- written (clamped)                            st+36 gain at the start (<- st+40 on return: the ramp target)
//     st+40 gain target   st+44 rate   st+48 read position fraction (float, in and out)
//   Prologue (FZ=0): pos = fctidz(st48 * K1), rate = fctidz(st44 * K1) (K1 = 2^32, double in .rdata), step =
//   float((st40 - st36) / float(count)), step8 = float(step * KF). Gains (FZ=1): g[0..3] = step * C_B + st36,
//   g[4..7] = step * C_A + st36 (C_A / C_B = 0..3 / 4..7 style vectors in .rdata); both advance by step8 per block.
//   Per block of 8 outputs (do { ... r6 -= 8 } while (s32)r6 > 0, so at least one block):
//     sample k: s0 = src16[pos_k >> 32], s1 = the next one, frac = (u32)pos_k >> 1;
//     out = (s1/32768 - s0/32768) * (frac * 2^-31 * g) + s0/32768 * g;   pos_k advances by the 64-bit rate.
//     After the block the integer part moves into the source pointer and pos keeps its low 32 bits.
//   Epilogue (FZ=0): st8 = min(((u32)(src - st0)) / (2 * st13), st4); st28 = min((dst - st20) >> 2, st24);
//   st48 = float(double(frac32) * K2) through the CRT __floatundidf helper (not hooked: its
//   address is not written here, so its direct calls stay inlinable), whose r3/r5/f1 are reproduced.
//
// Exactness notes:
//  * Same operations in the same order and operand order as the recompiled code; the scalar prologue/epilogue use the
//    same double expressions. Vector math runs with FPCR.FZ=1 like the recompiled VMX ops, the scalar parts with FZ=0;
//    the FPSCR calls follow the original's sequence (final state: FZ off).
//  * lvlx semantics of the sample loads: the two bytes of a halfword at an address with (a & 15) == 15 come from two
//    16-byte blocks and lvlx only loads the first one, the low byte reads 0 (reproduced; never happens for even sources).
//  * The s16 -> float conversion is exact (scale by 2^-15); frac >> 1 < 2^31 converts exactly like the recompiled
//    simde_mm_cvtepu32_ps_ (signed path) with round-to-nearest.
//  * Only the guest stack frame (below r1, callee-owned scratch) is not written. Returned: r3, r5, f1 (the helper's),
//    FPSCR. liveness.py: the only direct call site reads nothing after the call.
//  * Declines (the original runs) when st+13 is 0 (the original traps) or when (s32)r6 < INT32_MIN + 8 (the counter would
//    wrap); nothing has been modified at that point.
#pragma once

#include <climits>
#include <cmath>
#include <cstdint>

#include "../me_hot_common.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include <simde/x86/sse4.1.h>

namespace me::hot::n_82AAFB90 {

// Guest .rdata constants (Russian edition; English 0x82086108 / 0x82086130 / 0x82086110 / 0x821BE2A8 / 0x82086100).
inline constexpr uint32_t kK1 = 0x82090950u;   // double, rate/position scale
inline constexpr uint32_t kCA = 0x82090940u;   // 4 floats, gain ramp multipliers of samples 4..7
inline constexpr uint32_t kCB = 0x82090960u;   // 4 floats, gain ramp multipliers of samples 0..3
inline constexpr uint32_t kKF = 0x821BE278u;   // float, gain step per block of 8
inline constexpr uint32_t kK2 = 0x82090930u;   // double, fraction scale on return

inline constexpr Cmp kCmp = {R(3) | R(5), R(1), 0};

namespace detail {

#if defined(__aarch64__)
using V4 = float32x4_t;
using I4 = uint32x4_t;
inline V4 LoadBE(const uint8_t* base, uint32_t a) {
  return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(Raw(base, a & ~0xFu))));
}
inline void StoreBE(uint8_t* base, uint32_t a, V4 v) {
  vst1q_u8(Raw(base, a & ~0xFu), vrev32q_u8(vreinterpretq_u8_f32(v)));
}
inline V4 SplatBits(uint32_t bits) { return vreinterpretq_f32_u32(vdupq_n_u32(bits)); }
inline V4 Mul(V4 a, V4 b) { return vmulq_f32(a, b); }
inline V4 Add(V4 a, V4 b) { return vaddq_f32(a, b); }
inline V4 Sub(V4 a, V4 b) { return vsubq_f32(a, b); }
inline I4 SplatU(uint32_t x) { return vdupq_n_u32(x); }
inline I4 AddU(I4 a, I4 b) { return vaddq_u32(a, b); }
inline I4 MakeU(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  const uint32_t t[4] = {a, b, c, d};
  return vld1q_u32(t);
}
// frac >> 1 as float * 2^-31 (vcfux v,31 of the halved fraction)
inline V4 FracToF(I4 f) {
  return vmulq_f32(vcvtq_f32_u32(vshrq_n_u32(f, 1)), vreinterpretq_f32_u32(vdupq_n_u32(0x30000000u)));
}
// 4 s16 -> float * 2^-15 (vcfsx v,15)
inline V4 S16ToF(const int16_t* s) {
  return vmulq_f32(vcvtq_f32_s32(vmovl_s16(vld1_s16(s))), vreinterpretq_f32_u32(vdupq_n_u32(0x38000000u)));
}
#else
using V4 = simde__m128;
using I4 = simde__m128i;
inline V4 LoadBE(const uint8_t* base, uint32_t a) {
  return simde_mm_castsi128_ps(
      simde_mm_shuffle_epi8(simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(Raw(base, a & ~0xFu))),
                            simde_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)));
}
inline void StoreBE(uint8_t* base, uint32_t a, V4 v) {
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(Raw(base, a & ~0xFu)),
                        simde_mm_shuffle_epi8(simde_mm_castps_si128(v),
                                              simde_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)));
}
inline V4 SplatBits(uint32_t bits) { return simde_mm_castsi128_ps(simde_mm_set1_epi32(int(bits))); }
inline V4 Mul(V4 a, V4 b) { return simde_mm_mul_ps(a, b); }
inline V4 Add(V4 a, V4 b) { return simde_mm_add_ps(a, b); }
inline V4 Sub(V4 a, V4 b) { return simde_mm_sub_ps(a, b); }
inline I4 SplatU(uint32_t x) { return simde_mm_set1_epi32(int(x)); }
inline I4 AddU(I4 a, I4 b) { return simde_mm_add_epi32(a, b); }
inline I4 MakeU(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  return simde_mm_setr_epi32(int(a), int(b), int(c), int(d));
}
inline V4 FracToF(I4 f) {
  return simde_mm_mul_ps(simde_mm_cvtepi32_ps(simde_mm_srli_epi32(f, 1)), SplatBits(0x30000000u));
}
inline V4 S16ToF(const int16_t* s) {
  return simde_mm_mul_ps(
      simde_mm_cvtepi32_ps(simde_mm_cvtepi16_epi32(simde_mm_loadl_epi64(reinterpret_cast<const simde__m128i*>(s)))),
      SplatBits(0x38000000u));
}
#endif

inline uint32_t FloatBits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}
inline double DoubleFromBits(uint64_t b) {
  double d;
  std::memcpy(&d, &b, 8);
  return d;
}
inline uint64_t DoubleBits(double d) {
  uint64_t b;
  std::memcpy(&b, &d, 8);
  return b;
}

// fctidz as generated: NaN -> INT64_MIN, above INT64_MAX -> INT64_MAX, else the SSE truncating conversion.
inline int64_t Fctidz(double x) {
  return std::isnan(x) ? int64_t(0x8000000000000000ULL)
         : (x > double(LLONG_MAX)) ? LLONG_MAX
                                   : simde_mm_cvttsd_si64(simde_mm_load_sd(&x));
}

// One big-endian halfword the way lvlx + vsldoi(.., 2) picks it: the low byte is 0 when it lies in the next 16-byte block.
inline int16_t Sample(const uint8_t* base, uint32_t a) {
  const uint8_t hi = *Raw(base, a);
  const uint8_t lo = ((a & 15u) == 15u) ? uint8_t(0) : *Raw(base, a + 1);
  return int16_t(uint16_t(uint16_t(hi) << 8 | lo));
}

inline uint32_t Blocks(int32_t count) {
  return count <= 0 ? 1u : uint32_t((int64_t(count) + 7) / 8);
}

}  // namespace detail

inline bool Native(PPCContext& ctx, uint8_t* base) {
  using namespace detail;
  const uint32_t st = ctx.r7.u32;
  const int32_t count = ctx.r6.s32;
  if (Ld8(base, st + 13) == 0 || count < INT32_MIN + 8) return false;

  // ---- prologue (scalar, FZ=0) ----
  ctx.fpscr.disableFlushMode();
  const double f10 = double(LdF32(base, st + 36));
  const double f12 = double(LdF32(base, st + 44));
  double f11 = double(LdF32(base, st + 48));
  const float g0f = float(f10);  // stfs f10,80(r1); re-read by lvlx as the ramp start
  double f0 = double(LdF32(base, st + 40));
  StF32(base, st + 36, float(f0));
  const double k1 = DoubleFromBits(Ld64(base, kK1));
  f11 = f11 * k1;
  double f13 = f12 * k1;
  const V4 ca = LoadBE(base, kCA);
  const V4 cb = LoadBE(base, kCB);
  const uint64_t pos0 = uint64_t(Fctidz(f11));
  const uint64_t rate = uint64_t(Fctidz(f13));
  f13 = double(float(f0 - f10));
  f0 = double(int64_t(count));
  f0 = double(float(f0));
  f0 = double(float(f13 / f0));
  const double kf = double(LdF32(base, kKF));
  const float stepf = float(f0);
  f0 = double(float(f0 * kf));
  const float step8f = float(f0);

  // ---- vector part (FZ=1) ----
  ctx.fpscr.enableFlushModeUnconditional();
  const V4 vstep = SplatBits(FloatBits(stepf));
  const V4 vg0 = SplatBits(FloatBits(g0f));
  const V4 vstep8 = SplatBits(FloatBits(step8f));
  V4 g_hi = Add(Mul(vstep, ca), vg0);  // v10: samples 4..7
  V4 g_lo = Add(Mul(vstep, cb), vg0);  // v9: samples 0..3
  const uint32_t r = uint32_t(rate);
  const uint32_t frac0 = uint32_t(pos0);
  I4 frac_lo = MakeU(frac0, frac0 + r, frac0 + 2 * r, frac0 + 3 * r);
  I4 frac_hi = MakeU(frac0 + 4 * r, frac0 + 5 * r, frac0 + 6 * r, frac0 + 7 * r);
  const I4 r8 = SplatU(8 * r);

  uint64_t pos = pos0;
  uint32_t src = ctx.r3.u32;
  uint32_t dst = ctx.r5.u32;
  int64_t left = ctx.r6.s64;
  alignas(16) int16_t s0[8];
  alignas(16) int16_t s1[8];
  // An even source stays even (it advances by whole samples): no halfword can then start at offset 15 of a 16-byte
  // block, and each sample pair is one big-endian 32-bit load.
  const bool even = (src & 1u) == 0;
  do {
    if (even) [[likely]] {
      for (int k = 0; k < 8; ++k) {
        const uint32_t a = src + (uint32_t(pos >> 32) << 1);
        pos += rate;
        const uint32_t pair = Ld32(base, a);
        s0[k] = int16_t(uint16_t(pair >> 16));
        s1[k] = int16_t(uint16_t(pair));
      }
    } else {
      for (int k = 0; k < 8; ++k) {
        const uint32_t a = src + (uint32_t(pos >> 32) << 1);
        pos += rate;
        s0[k] = Sample(base, a);
        s1[k] = Sample(base, a + 2);
      }
    }
    src += uint32_t(pos >> 32) << 1;
    pos &= 0xFFFFFFFFull;
    const V4 a0 = S16ToF(s0), a1 = S16ToF(s0 + 4);
    const V4 b0 = S16ToF(s1), b1 = S16ToF(s1 + 4);
    const V4 f_lo = FracToF(frac_lo), f_hi = FracToF(frac_hi);
    const V4 v4 = Mul(a0, g_lo);
    const V4 d_lo = Sub(b0, a0);
    const V4 fg_lo = Mul(f_lo, g_lo);
    const V4 v3 = Mul(a1, g_hi);
    const V4 d_hi = Sub(b1, a1);
    const V4 fg_hi = Mul(f_hi, g_hi);
    StoreBE(base, dst, Add(Mul(d_lo, fg_lo), v4));
    StoreBE(base, dst + 16, Add(Mul(d_hi, fg_hi), v3));
    left -= 8;
    g_hi = Add(g_hi, vstep8);
    g_lo = Add(g_lo, vstep8);
    dst += 32;
    frac_hi = AddU(frac_hi, r8);
    frac_lo = AddU(frac_lo, r8);
  } while (int32_t(left) > 0);

  // ---- epilogue (scalar, FZ=0) ----
  const uint32_t start = Ld32(base, st + 0);
  const uint32_t div = uint32_t(Ld8(base, st + 13)) << 1;
  const uint32_t length = Ld32(base, st + 4);
  const uint32_t consumed = (src - start) / div;
  St32(base, st + 8, consumed < length ? consumed : length);
  const uint32_t written = (dst - Ld32(base, st + 20)) >> 2;
  const uint32_t capacity = Ld32(base, st + 24);
  St32(base, st + 28, written < capacity ? written : capacity);
  // __floatundidf(frac): exact for 32-bit values; r3 = its bit pattern, r5 = the biased exponent (64 for 0).
  const uint64_t frac = pos;
  if (frac == 0) {
    ctx.r3.u64 = 0;
    ctx.r5.u64 = 64;
  } else {
    ctx.r5.u64 = uint64_t(1086 - __builtin_clzll(frac));
    ctx.r3.u64 = DoubleBits(double(frac));
  }
  ctx.fpscr.disableFlushMode();
  ctx.f1.u64 = ctx.r3.u64;
  f0 = ctx.f1.f64 * DoubleFromBits(Ld64(base, kK2));
  f0 = double(float(f0));
  StF32(base, st + 48, float(f0));
  return true;
}

inline void Writes(const PPCContext& ctx, const uint8_t*, me::hot::Writes& w) {
  const int32_t count = ctx.r6.s32;
  if (count < INT32_MIN + 8) return;  // declined
  w.Add(ctx.r7.u32, 52);
  const uint64_t bytes = uint64_t(detail::Blocks(count)) * 32 + 16;
  if (bytes > me::hot::Writes::kMaxBytes) {
    w.overflow = true;
    return;
  }
  w.Add(ctx.r5.u32 & ~0xFu, uint32_t(bytes));
}

}  // namespace me::hot::n_82AAFB90

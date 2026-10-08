// Mass Effect - native replacements of the XAudio mixer's hottest recompiled DSP functions (aarch64 NEON).
//
// Header only: shared by the hooks (me_audio_hooks.cpp) and by the host differential test
// (tests/audio_dsp/), which runs these against the recompiled originals bit by bit.
//
// WHY (Switch profiles, mixer thread XThread6A61F7A0, ~24 % of a core)
//   Every recompiled VMX op goes through ctx.vN memory and every vector/scalar FP switch writes FPCR
//   (ctx.fpscr.enable/disableFlushModeUnconditional). The leaf kernels below are small and exact:
//   sub_82AA53C0  ramped gain mix   dst[i] += src[i] * (g[i%4] + (i/4) * step[i%4])       6.4 % of the thread
//   sub_82B4D580  one-pole smoother + clamp over the channel buffers (scalar fmadds recurrence between VMX
//                 multiplies and min/max; 16 FPCR switches per 16 floats in the original)   17 % of the thread
//
// BIT EXACTNESS
//   The same operations in the same order with the same FPCR.FZ state as the recompiled code:
//   vector ops with FZ=1 (VMX flushes denormals), the scalar recurrence with FZ=0, as double fma rounded to
//   float. min/max go through simde like the generated code (x86 NaN semantics). The caller-visible state is
//   the memory written, FPCR.FZ and ctx.fpscr.csr at exit, r3 (sub_82B4D580 returns 0). Volatile registers
//   the original leaves as loop leftovers are not reproduced (the callers reload them; checked for the
//   three call sites in sub_82AA54E8 and the dispatch-table use of sub_82B4D580).

#pragma once

#include <cstdint>
#include <cstring>

#include <rex/platform.h>
#include <rex/ppc.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#define ME_AUDIO_DSP_NEON 1
#else
#define ME_AUDIO_DSP_NEON 0
#endif

REX_EXTERN(__imp__sub_82AA65E8);

namespace me::audio_dsp {

#if ME_AUDIO_DSP_NEON

// Host pointer of a guest address: on the Switch (and Linux) base + address; on macOS arm64 / Windows the
// guest physical range 0xE0000000+ sits 0x1000 further (REX_PHYS_HOST_OFFSET in the generated pch).
inline uint8_t* GP(uint8_t* base, uint32_t address) {
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
#else
  return base + address;
#endif
}

// lvx128/stvx: aligned 16-byte vector, big-endian words. Lane k = guest word k.
inline float32x4_t LoadBE4(uint8_t* base, uint32_t address) {
  return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(GP(base, address & ~0xFu))));
}
inline void StoreBE4(uint8_t* base, uint32_t address, float32x4_t v) {
  vst1q_u8(GP(base, address & ~0xFu), vrev32q_u8(vreinterpretq_u8_f32(v)));
}
inline float LoadBEf(uint8_t* base, uint32_t address) {
  uint32_t bits;
  std::memcpy(&bits, GP(base, address), 4);
  bits = __builtin_bswap32(bits);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
inline void StoreBEf(uint8_t* base, uint32_t address, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  bits = __builtin_bswap32(bits);
  std::memcpy(GP(base, address), &bits, 4);
}

// ---------------------------------------------------------------------------------------------------------
// sub_82AA53C0(r3 dst, r4 src, r5 &gain[4], r6 &step[4], r7 n floats)
// Leaf. Processes ((n/4 - 1)/4 + 1) blocks of 16 floats (4 vectors); nothing when n < 4. Per block the four
// vectors use gains g, g+s, g+2s, g+3s, which then advance by 4s. dst = src*gain + dst (mul, then add).
// All eight loads of a block happen before its four stores (as in the recompiled code).
inline void Native82AA53C0(PPCContext& ctx, uint8_t* base) {
  ctx.fpscr.enableFlushMode();
  const uint32_t dst = ctx.r3.u32;
  const uint32_t src = ctx.r4.u32;
  const uint32_t n = ctx.r7.u32;
  const float32x4_t step = LoadBE4(base, ctx.r6.u32);
  const float32x4_t g0_init = LoadBE4(base, ctx.r5.u32);
  const float32x4_t s2 = vaddq_f32(step, step);
  float32x4_t g1 = vaddq_f32(g0_init, step);
  const float32x4_t s3 = vaddq_f32(s2, step);
  const float32x4_t s4 = vaddq_f32(s2, s2);
  float32x4_t g2 = vaddq_f32(g0_init, s2);
  float32x4_t g3 = vaddq_f32(g0_init, s3);
  float32x4_t g0 = g0_init;
  const uint32_t vectors = n >> 2;
  if (vectors == 0) {
    return;
  }
  uint32_t iterations = ((vectors - 1) >> 2) + 1;
  uint32_t d = dst;
  uint32_t s = src;
  while (iterations--) {
    const float32x4_t d0 = LoadBE4(base, d);
    const float32x4_t d1 = LoadBE4(base, d + 16);
    const float32x4_t d2 = LoadBE4(base, d + 32);
    const float32x4_t d3 = LoadBE4(base, d + 48);
    const float32x4_t s0v = LoadBE4(base, s);
    const float32x4_t s1v = LoadBE4(base, s + 16);
    const float32x4_t s2v = LoadBE4(base, s + 32);
    const float32x4_t s3v = LoadBE4(base, s + 48);
    const float32x4_t r0 = vaddq_f32(vmulq_f32(s0v, g0), d0);
    const float32x4_t r1 = vaddq_f32(vmulq_f32(s1v, g1), d1);
    const float32x4_t r2 = vaddq_f32(vmulq_f32(s2v, g2), d2);
    const float32x4_t r3 = vaddq_f32(vmulq_f32(s3v, g3), d3);
    g0 = vaddq_f32(g0, s4);
    g1 = vaddq_f32(g1, s4);
    g2 = vaddq_f32(g2, s4);
    g3 = vaddq_f32(g3, s4);
    StoreBE4(base, d, r0);
    StoreBE4(base, d + 16, r1);
    StoreBE4(base, d + 32, r2);
    StoreBE4(base, d + 48, r3);
    d += 64;
    s += 64;
  }
}

// ---------------------------------------------------------------------------------------------------------
// sub_82B4D580(r3 this, r4 arg): per-channel one-pole smoother over 256 floats per channel.
//   sub_82AA65E8(arg, &frame[80]) fills the channel count (byte +1) and the buffer address (word +8).
//   A = this[12], B = this[16], state[ch] = this[20 + 4*ch]; constants C_lo @0x821BE0F0, C_hi @0x82002978,
//   Z @0x82002974 (guest .rdata).
//   A == Z: nothing. B == Z: memset(buffer, 0, 1024). Otherwise, per channel ch (buffer + 1024*ch):
//     x[j] = in[j] * B (VMX multiply, FZ=1); y = state; y[j] = (float)fma(A, y, x[j]) (scalar, FZ=0);
//     out[j] = max(min(y[j], C_hi), C_lo) (VMX, FZ=1, simde semantics); state = y[255] (unclamped).
// Returns 0 in r3 and leaves FZ=0, like the original.
namespace detail {

constexpr uint32_t kFloatsPerChannel = 256;

// Phase 1 (FZ=1): x = in * B for every channel vector, into scratch.
__attribute__((noinline)) inline void MulPhase(uint8_t* base, uint32_t buffer, uint32_t vectors,
                                                float32x4_t b, float* x) {
  for (uint32_t v = 0; v < vectors; ++v) {
    const float32x4_t in = LoadBE4(base, buffer + 16 * v);
    vst1q_f32(x + 4 * v, vmulq_f32(in, b));
  }
}

// Phase 2 (FZ=0): the scalar recurrence, per channel, in double like fmadds:
//   y = (float)fma(a, y, x[j]), kept as double between steps.
// The recurrence is serial within a channel (latency bound: fma + two conversions per sample), but the channels are
// independent, so two channels share one float64x2 lane pair and up to three pairs run interleaved. FMLA / FCVTN /
// FCVTL on the vector unit round exactly like the scalar FMADD / FCVT (IEEE fma, same FPCR), so every lane is
// bit-identical to the one-channel scalar loop (tests/audio_dsp).
template <int P>
inline void ChainPairs(uint8_t* base, uint32_t state_address, uint32_t ch0, float64x2_t a2, const float* x,
                       float* y) {
  float64x2_t s[P];
  const float* xa[P];
  const float* xb[P];
  float* ya[P];
  float* yb[P];
  for (int p = 0; p < P; ++p) {
    const uint32_t c = ch0 + 2 * p;
    const float s0 = LoadBEf(base, state_address + 4 * c);
    const float s1 = LoadBEf(base, state_address + 4 * (c + 1));
    s[p] = vcvt_f64_f32(vset_lane_f32(s1, vdup_n_f32(s0), 1));
    xa[p] = x + size_t(c) * kFloatsPerChannel;
    xb[p] = xa[p] + kFloatsPerChannel;
    ya[p] = y + size_t(c) * kFloatsPerChannel;
    yb[p] = ya[p] + kFloatsPerChannel;
  }
  for (uint32_t j = 0; j < kFloatsPerChannel; j += 4) {
    for (int p = 0; p < P; ++p) {
      const float32x4_t va = vld1q_f32(xa[p] + j);
      const float32x4_t vb = vld1q_f32(xb[p] + j);
      const float32x4_t lo = vzip1q_f32(va, vb);  // a0 b0 a1 b1
      const float32x4_t hi = vzip2q_f32(va, vb);  // a2 b2 a3 b3
      const float32x2_t r0 = vcvt_f32_f64(vfmaq_f64(vcvt_f64_f32(vget_low_f32(lo)), a2, s[p]));
      s[p] = vcvt_f64_f32(r0);
      const float32x2_t r1 = vcvt_f32_f64(vfmaq_f64(vcvt_high_f64_f32(lo), a2, s[p]));
      s[p] = vcvt_f64_f32(r1);
      const float32x2_t r2 = vcvt_f32_f64(vfmaq_f64(vcvt_f64_f32(vget_low_f32(hi)), a2, s[p]));
      s[p] = vcvt_f64_f32(r2);
      const float32x2_t r3 = vcvt_f32_f64(vfmaq_f64(vcvt_high_f64_f32(hi), a2, s[p]));
      s[p] = vcvt_f64_f32(r3);
      const float32x4_t q01 = vcombine_f32(r0, r1);  // a0 b0 a1 b1
      const float32x4_t q23 = vcombine_f32(r2, r3);  // a2 b2 a3 b3
      vst1q_f32(ya[p] + j, vuzp1q_f32(q01, q23));
      vst1q_f32(yb[p] + j, vuzp2q_f32(q01, q23));
    }
  }
  for (int p = 0; p < P; ++p) {
    const uint32_t c = ch0 + 2 * p;
    const float32x2_t f = vcvt_f32_f64(s[p]);
    StoreBEf(base, state_address + 4 * c, vget_lane_f32(f, 0));
    StoreBEf(base, state_address + 4 * (c + 1), vget_lane_f32(f, 1));
  }
}

__attribute__((noinline)) inline void ChainPhase(uint8_t* base, uint32_t state_address,
                                                  uint32_t channels, double a, const float* x, float* y) {
  const float64x2_t a2 = vdupq_n_f64(a);
  uint32_t ch = 0;
  for (; ch + 6 <= channels; ch += 6) ChainPairs<3>(base, state_address, ch, a2, x, y);
  if (ch + 4 <= channels) {
    ChainPairs<2>(base, state_address, ch, a2, x, y);
    ch += 4;
  }
  if (ch + 2 <= channels) {
    ChainPairs<1>(base, state_address, ch, a2, x, y);
    ch += 2;
  }
  for (; ch < channels; ++ch) {  // odd channel: the scalar loop
    double state = double(LoadBEf(base, state_address + 4 * ch));
    const float* xc = x + size_t(ch) * kFloatsPerChannel;
    float* yc = y + size_t(ch) * kFloatsPerChannel;
    for (uint32_t j = 0; j < kFloatsPerChannel; ++j) {
      state = double(float(__builtin_fma(a, state, double(xc[j]))));
      yc[j] = float(state);
    }
    StoreBEf(base, state_address + 4 * ch, float(state));
  }
}

// Phase 3 (FZ=1): clamp and store the channel vectors.
__attribute__((noinline)) inline void ClampPhase(uint8_t* base, uint32_t buffer, uint32_t vectors,
                                                  float lo, float hi, const float* y) {
  const simde__m128 vlo = simde_mm_set1_ps(lo);
  const simde__m128 vhi = simde_mm_set1_ps(hi);
  for (uint32_t v = 0; v < vectors; ++v) {
    simde__m128 t = simde_mm_load_ps(y + 4 * v);
    t = simde_mm_min_ps(t, vhi);
    t = simde_mm_max_ps(t, vlo);
    float32x4_t out;
    simde_mm_store_ps(reinterpret_cast<float*>(&out), t);
    StoreBE4(base, buffer + 16 * v, out);
  }
}

}  // namespace detail

inline void Native82B4D580(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t old_r1 = ctx.r1.u32;
  const uint32_t frame = old_r1 - 224;
  // The callee gets the same frame position the original used (its own frame goes below it).
  {
    uint32_t chain = __builtin_bswap32(old_r1);
    std::memcpy(GP(base, frame), &chain, 4);
  }
  const auto old_lr = ctx.lr;
  ctx.r3.u64 = ctx.r4.u64;
  ctx.r1.u32 = frame;
  ctx.r4.s64 = int64_t(frame) + 80;
  ctx.lr = 0x82B4D5A0;
  __imp__sub_82AA65E8(ctx, base);
  ctx.r1.u32 = old_r1;
  ctx.lr = old_lr;
  ctx.fpscr.disableFlushMode();
  ctx.r3.u64 = 0;
  uint8_t channels_byte;
  std::memcpy(&channels_byte, GP(base, frame + 81), 1);
  uint32_t buffer;
  std::memcpy(&buffer, GP(base, frame + 88), 4);
  buffer = __builtin_bswap32(buffer);

  const float a = LoadBEf(base, self + 12);
  const float c_lo = LoadBEf(base, 0x821BE0F0u);
  const float c_hi = LoadBEf(base, 0x82002978u);
  const float zero = LoadBEf(base, 0x82002974u);
  const float b = LoadBEf(base, self + 16);
  if (a == zero) {
    return;
  }
  if (b == zero) {
    std::memset(GP(base, buffer), 0, 1024);
    return;
  }
  if (channels_byte == 0) {
    return;
  }
  const uint32_t channels = channels_byte;
  const uint32_t vectors = channels * (detail::kFloatsPerChannel / 4);
  thread_local float* scratch = nullptr;
  thread_local size_t scratch_floats = 0;
  const size_t need = size_t(channels) * detail::kFloatsPerChannel * 2;
  if (scratch_floats < need) {
    delete[] scratch;
    scratch = new float[need];
    scratch_floats = need;
  }
  float* x = scratch;
  float* y = scratch + size_t(channels) * detail::kFloatsPerChannel;

  ctx.fpscr.enableFlushModeUnconditional();
  detail::MulPhase(base, buffer, vectors, vdupq_n_f32(b), x);
  ctx.fpscr.disableFlushModeUnconditional();
  detail::ChainPhase(base, self + 20, channels, double(a), x, y);
  ctx.fpscr.enableFlushModeUnconditional();
  detail::ClampPhase(base, buffer, vectors, c_lo, c_hi, y);
  ctx.fpscr.disableFlushModeUnconditional();
}

#endif  // ME_AUDIO_DSP_NEON

}  // namespace me::audio_dsp

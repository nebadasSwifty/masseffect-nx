/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <rex/assert.h>
#include <rex/audio/downmix.h>
#include <rex/platform.h>
#include <rex/types.h>

#if REX_ARCH_ARM64
#include <arm_neon.h>
#endif

namespace rex::audio::conversion {

#if REX_ARCH_AMD64
inline void sequential_6_BE_to_interleaved_6_LE(float* output, const float* input,
                                                size_t ch_sample_count, const SurroundMix& mix,
                                                float gain) {
  // The gain pass below walks the output four floats at a time, and its weight
  // table assumes 6 channels divide evenly into those windows.
  assert_true(ch_sample_count % 2 == 0);

  const uint32_t* in = reinterpret_cast<const uint32_t*>(input);
  uint32_t* out = reinterpret_cast<uint32_t*>(output);
  const __m128i byte_swap_shuffle =
      _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  for (size_t sample = 0; sample < ch_sample_count; sample++) {
    __m128i sample0 =
        _mm_set_epi32(in[3 * ch_sample_count + sample], in[2 * ch_sample_count + sample],
                      in[1 * ch_sample_count + sample], in[0 * ch_sample_count + sample]);
    uint32_t sample1 = in[4 * ch_sample_count + sample];
    uint32_t sample2 = in[5 * ch_sample_count + sample];
    sample0 = _mm_shuffle_epi8(sample0, byte_swap_shuffle);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[sample * 6]), sample0);
    sample1 = rex::byte_swap(sample1);
    out[sample * 6 + 4] = sample1;
    sample2 = rex::byte_swap(sample2);
    out[sample * 6 + 5] = sample2;
  }

  // Second pass rather than fusing into the shuffle above, which stores as integers.
  // 6 KB per 5.33 ms frame, so skipping it at unity gain is not worth the asymmetry.
  //
  // Six interleaved channels across four-float windows repeat every 12 floats,
  // so three weight vectors cover every alignment the loop ever sees. The
  // assert above makes the sample count even, which makes the float count a
  // multiple of 12, so the cycle closes with no tail to handle.
  const float c = mix.center * gain;
  const float s = mix.surround * gain;
  const float l = mix.lfe * gain;
  const __m128 w[3] = {_mm_setr_ps(gain, gain, c, l), _mm_setr_ps(s, s, gain, gain),
                       _mm_setr_ps(c, l, s, s)};
  const __m128 lo = _mm_set1_ps(-1.0f);
  const __m128 hi = _mm_set1_ps(1.0f);
  const size_t count = ch_sample_count * 6;
  for (size_t i = 0, phase = 0; i < count; i += 4, phase = (phase + 1) % 3) {
    const __m128 v = _mm_mul_ps(_mm_loadu_ps(&output[i]), w[phase]);
    _mm_storeu_ps(&output[i], _mm_min_ps(_mm_max_ps(v, lo), hi));
  }
}

inline void sequential_6_BE_to_interleaved_2_LE(float* output, const float* input,
                                                size_t ch_sample_count, const StereoFold& fold,
                                                float gain) {
  assert_true(ch_sample_count % 4 == 0);

  const __m128i byte_swap_shuffle =
      _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  const __m128 center = _mm_set1_ps(fold.center);
  const __m128 lfe = _mm_set1_ps(fold.lfe);
  const __m128 surround = _mm_set1_ps(fold.surround);
  const __m128 scale = _mm_set1_ps(fold.scale * gain);
  const __m128 lo = _mm_set1_ps(-1.0f);
  const __m128 hi = _mm_set1_ps(1.0f);

  for (size_t sample = 0; sample < ch_sample_count; sample += 4) {
    // load 4 samples from 6 channels each
    __m128 fl = _mm_loadu_ps(&input[0 * ch_sample_count + sample]);
    __m128 fr = _mm_loadu_ps(&input[1 * ch_sample_count + sample]);
    __m128 fc = _mm_loadu_ps(&input[2 * ch_sample_count + sample]);
    __m128 lf = _mm_loadu_ps(&input[3 * ch_sample_count + sample]);
    __m128 bl = _mm_loadu_ps(&input[4 * ch_sample_count + sample]);
    __m128 br = _mm_loadu_ps(&input[5 * ch_sample_count + sample]);
    // byte swap
    fl = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(fl), byte_swap_shuffle));
    fr = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(fr), byte_swap_shuffle));
    fc = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(fc), byte_swap_shuffle));
    lf = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(lf), byte_swap_shuffle));
    bl = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(bl), byte_swap_shuffle));
    br = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(br), byte_swap_shuffle));

    // Center and LFE land on both sides.
    const __m128 mid = _mm_add_ps(_mm_mul_ps(fc, center), _mm_mul_ps(lf, lfe));
    __m128 left = _mm_mul_ps(_mm_add_ps(_mm_add_ps(fl, mid), _mm_mul_ps(bl, surround)), scale);
    __m128 right = _mm_mul_ps(_mm_add_ps(_mm_add_ps(fr, mid), _mm_mul_ps(br, surround)), scale);
    left = _mm_min_ps(_mm_max_ps(left, lo), hi);
    right = _mm_min_ps(_mm_max_ps(right, lo), hi);

    _mm_storeu_ps(&output[sample * 2], _mm_unpacklo_ps(left, right));
    _mm_storeu_ps(&output[(sample + 2) * 2], _mm_unpackhi_ps(left, right));
  }
}
#else
inline void sequential_6_BE_to_interleaved_6_LE(float* output, const float* input,
                                                size_t ch_sample_count, const SurroundMix& mix,
                                                float gain) {
  const float w[6] = {
      gain, gain, mix.center * gain, mix.lfe * gain, mix.surround * gain, mix.surround * gain};
  for (size_t sample = 0; sample < ch_sample_count; sample++) {
    for (size_t channel = 0; channel < 6; channel++) {
      const float v = rex::byte_swap(input[channel * ch_sample_count + sample]) * w[channel];
      output[sample * 6 + channel] = std::clamp(v, -1.0f, 1.0f);
    }
  }
}

inline void sequential_6_BE_to_interleaved_2_LE(float* output, const float* input,
                                                size_t ch_sample_count, const StereoFold& fold,
                                                float gain) {
  // Default 5.1 channel mapping is fl, fr, fc, lf, bl, br
  // https://docs.microsoft.com/en-us/windows/win32/xaudio2/xaudio2-default-channel-mapping
  const float scale = fold.scale * gain;
  for (size_t sample = 0; sample < ch_sample_count; sample++) {
    const float fl = rex::byte_swap(input[0 * ch_sample_count + sample]);
    const float fr = rex::byte_swap(input[1 * ch_sample_count + sample]);
    const float fc = rex::byte_swap(input[2 * ch_sample_count + sample]);
    const float lf = rex::byte_swap(input[3 * ch_sample_count + sample]);
    const float bl = rex::byte_swap(input[4 * ch_sample_count + sample]);
    const float br = rex::byte_swap(input[5 * ch_sample_count + sample]);
    // Center and LFE land on both sides.
    const float mid = fc * fold.center + lf * fold.lfe;
    output[sample * 2] = std::clamp((fl + mid + bl * fold.surround) * scale, -1.0f, 1.0f);
    output[sample * 2 + 1] = std::clamp((fr + mid + br * fold.surround) * scale, -1.0f, 1.0f);
  }
}
#endif

// Adds the stereo fold of one sequential 6-channel big-endian frame to an interleaved stereo accumulator:
// accum[i] = accum[i] + folded[i], with folded[] exactly what sequential_6_BE_to_interleaved_2_LE writes (the same
// operations in the same order; the SDK and app are built with -ffp-contract=off, so no fused multiply-add).
// One pass instead of a fold into a temporary array plus a second add loop.
inline void sequential_6_BE_fold_add_interleaved_2_LE(float* accum, const float* input, size_t ch_sample_count,
                                                      const StereoFold& fold, float gain) {
#if REX_ARCH_ARM64
  assert_true(ch_sample_count % 4 == 0);
  const float32x4_t center = vdupq_n_f32(fold.center);
  const float32x4_t lfe = vdupq_n_f32(fold.lfe);
  const float32x4_t surround = vdupq_n_f32(fold.surround);
  const float32x4_t scale = vdupq_n_f32(fold.scale * gain);
  const float32x4_t lo = vdupq_n_f32(-1.0f);
  const float32x4_t hi = vdupq_n_f32(1.0f);
  auto load_be = [](const float* p) { return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(p)))); };
  // std::clamp(v, lo, hi): v < lo ? lo : (hi < v ? hi : v); a NaN stays a NaN.
  auto clamp = [&](float32x4_t v) { return vbslq_f32(vcltq_f32(v, lo), lo, vbslq_f32(vcltq_f32(hi, v), hi, v)); };
  for (size_t sample = 0; sample < ch_sample_count; sample += 4) {
    const float32x4_t fl = load_be(&input[0 * ch_sample_count + sample]);
    const float32x4_t fr = load_be(&input[1 * ch_sample_count + sample]);
    const float32x4_t fc = load_be(&input[2 * ch_sample_count + sample]);
    const float32x4_t lf = load_be(&input[3 * ch_sample_count + sample]);
    const float32x4_t bl = load_be(&input[4 * ch_sample_count + sample]);
    const float32x4_t br = load_be(&input[5 * ch_sample_count + sample]);
    const float32x4_t mid = vaddq_f32(vmulq_f32(fc, center), vmulq_f32(lf, lfe));
    const float32x4_t left = clamp(vmulq_f32(vaddq_f32(vaddq_f32(fl, mid), vmulq_f32(bl, surround)), scale));
    const float32x4_t right = clamp(vmulq_f32(vaddq_f32(vaddq_f32(fr, mid), vmulq_f32(br, surround)), scale));
    float32x4x2_t acc = vld2q_f32(&accum[sample * 2]);
    acc.val[0] = vaddq_f32(acc.val[0], left);
    acc.val[1] = vaddq_f32(acc.val[1], right);
    vst2q_f32(&accum[sample * 2], acc);
  }
#else
  constexpr size_t kChunk = 64;
  float folded[kChunk * 2];
  for (size_t done = 0; done < ch_sample_count; done += kChunk) {
    const size_t n = std::min(kChunk, ch_sample_count - done);
    // A chunk of the frame: the channel planes stay ch_sample_count apart.
    const float scale = fold.scale * gain;
    for (size_t sample = 0; sample < n; sample++) {
      const size_t s = done + sample;
      const float fl = rex::byte_swap(input[0 * ch_sample_count + s]);
      const float fr = rex::byte_swap(input[1 * ch_sample_count + s]);
      const float fc = rex::byte_swap(input[2 * ch_sample_count + s]);
      const float lf = rex::byte_swap(input[3 * ch_sample_count + s]);
      const float bl = rex::byte_swap(input[4 * ch_sample_count + s]);
      const float br = rex::byte_swap(input[5 * ch_sample_count + s]);
      const float mid = fc * fold.center + lf * fold.lfe;
      folded[sample * 2] = std::clamp((fl + mid + bl * fold.surround) * scale, -1.0f, 1.0f);
      folded[sample * 2 + 1] = std::clamp((fr + mid + br * fold.surround) * scale, -1.0f, 1.0f);
    }
    for (size_t i = 0; i < n * 2; ++i) accum[done * 2 + i] += folded[i];
  }
#endif
}

// Output stage statistics of mix_to_s16 (counting only).
struct MixStats {
  float peak = 0.0f;        // largest |sample| before clipping (non-finite samples count as 0)
  uint32_t saturated = 0;   // samples above 1.0 before clipping
  bool nonzero = false;     // any output sample != 0
};

// float mix -> s16 PCM: non-finite -> 0, clamp to [-1, 1], * 32767, lrint (round to nearest even). `count` floats.
inline MixStats mix_to_s16(int16_t* out, const float* mix, size_t count) {
  MixStats st;
  size_t i = 0;
#if REX_ARCH_ARM64
  // Same results as the scalar loop below: vcvtnq rounds to nearest even like lrint in the default rounding mode, the
  // values fit in s16 (|x| <= 32767), and max/min see no NaN (non-finite samples are zeroed first).
  const float32x4_t inf = vdupq_n_f32(INFINITY);
  const float32x4_t one = vdupq_n_f32(1.0f);
  const float32x4_t lo = vdupq_n_f32(-1.0f);
  const float32x4_t k = vdupq_n_f32(32767.0f);
  float32x4_t peak = vdupq_n_f32(0.0f);
  uint32x4_t sat = vdupq_n_u32(0);
  int16x8_t any = vdupq_n_s16(0);
  for (; i + 8 <= count; i += 8) {
    float32x4_t v0 = vld1q_f32(mix + i);
    float32x4_t v1 = vld1q_f32(mix + i + 4);
    v0 = vreinterpretq_f32_u32(vandq_u32(vcltq_f32(vabsq_f32(v0), inf), vreinterpretq_u32_f32(v0)));
    v1 = vreinterpretq_f32_u32(vandq_u32(vcltq_f32(vabsq_f32(v1), inf), vreinterpretq_u32_f32(v1)));
    const float32x4_t m0 = vabsq_f32(v0), m1 = vabsq_f32(v1);
    peak = vmaxq_f32(peak, vmaxq_f32(m0, m1));
    sat = vsubq_u32(sat, vcgtq_f32(m0, one));
    sat = vsubq_u32(sat, vcgtq_f32(m1, one));
    const int32x4_t q0 = vcvtnq_s32_f32(vmulq_f32(vminq_f32(vmaxq_f32(v0, lo), one), k));
    const int32x4_t q1 = vcvtnq_s32_f32(vmulq_f32(vminq_f32(vmaxq_f32(v1, lo), one), k));
    const int16x8_t s = vcombine_s16(vmovn_s32(q0), vmovn_s32(q1));
    vst1q_s16(out + i, s);
    any = vorrq_s16(any, s);
  }
  st.peak = vmaxvq_f32(peak);
  st.saturated = vaddvq_u32(sat);
  st.nonzero = vmaxvq_u16(vreinterpretq_u16_s16(any)) != 0;
#endif
  for (; i < count; ++i) {
    const float value = std::isfinite(mix[i]) ? mix[i] : 0.0f;
    const float magnitude = std::fabs(value);
    st.peak = std::max(st.peak, magnitude);
    st.saturated += magnitude > 1.0f ? 1 : 0;
    out[i] = static_cast<int16_t>(std::lrint(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
    st.nonzero |= out[i] != 0;
  }
  return st;
}

}  // namespace rex::audio::conversion

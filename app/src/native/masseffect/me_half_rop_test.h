#pragma once
// The one-time ROP conversion self-test of masseffect_native_bias_life_probe (docs/vulkan-frame-time.md section 12):
// how the colour output stage of the GPU converts a float32 fragment output into an RGBA16F attachment. An output
// epilogue that must reproduce "the attachment value, times 2^-3, stored again" (the exponent-bias resolve folded
// into the draw that produced its source) is exact only if it models that conversion; the Vulkan specification
// leaves the rounding mode of such conversions to the implementation, and NAK's OpQuantizeToF16 flushes half
// denormals, so the model is measured on the console instead of assumed.
//
// The test writes known float32 bit patterns into an RGBA32F image, runs the existing exponent-bias resolve pass
// (texelFetch * 2^bias, written through an RGBA16F colour attachment) with bias 0 and with bias -3, reads both
// targets back and compares every value with candidate conversion models. Header-only and free of Vulkan
// (tests/cpu/test_native_half_rop.cpp checks the models on the host).
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace me::native {

enum HalfRopModel : uint32_t {
  kHalfRneDenormals = 0,   // IEEE 754: round to nearest even, half denormals kept, overflow to infinity
  kHalfRneFlush,           // round to nearest even, results below 2^-14 flushed to signed zero
  kHalfRtzDenormals,       // round toward zero, denormals kept, overflow to the largest finite value
  kHalfRtzFlush,           // round toward zero, results below 2^-14 flushed to signed zero
  kHalfRneSaturate,        // round to nearest even, denormals kept, overflow to the largest finite value
  kHalfModelCount
};

inline uint32_t HalfRopBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
inline float HalfRopFloat(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// float32 -> float16 bits under a model. NaN inputs give a quiet NaN (the payload is not compared).
inline uint16_t HalfRopConvert(float value, uint32_t model) {
  const uint32_t u = HalfRopBits(value);
  const uint16_t sign = uint16_t((u >> 16) & 0x8000u);
  const uint32_t magnitude = u & 0x7FFFFFFFu;
  if (magnitude > 0x7F800000u) return uint16_t(sign | 0x7E00u);
  const bool rne = model == kHalfRneDenormals || model == kHalfRneFlush || model == kHalfRneSaturate;
  const bool flush = model == kHalfRneFlush || model == kHalfRtzFlush;
  const bool saturate = model == kHalfRtzDenormals || model == kHalfRtzFlush || model == kHalfRneSaturate;
  if (magnitude == 0x7F800000u) return uint16_t(sign | (saturate ? 0x7BFFu : 0x7C00u));
  // Exact value as an integer multiple of 2^-149 is not needed: work in double, every float32 is exact there.
  const double a = std::fabs(double(value));
  const double min_normal = std::ldexp(1.0, -14);
  // Quantum of the half grid at this magnitude: 2^-24 below 2^-14, else 2^(e - 10).
  int e = 0;
  std::frexp(a, &e);  // a = m * 2^e, m in [0.5, 1)
  const int exponent = e - 1;  // a in [2^exponent, 2^(exponent + 1))
  const double quantum = a < min_normal ? std::ldexp(1.0, -24) : std::ldexp(1.0, exponent - 10);
  const double scaled = a / quantum;  // exact (power of two)
  double integral = std::floor(scaled);
  if (rne) {
    const double rest = scaled - integral;
    if (rest > 0.5 || (rest == 0.5 && std::fmod(integral, 2.0) != 0.0)) integral += 1.0;
  }
  double result = integral * quantum;
  if (result >= 65520.0 || (!rne && result > 65504.0)) {
    // Overflow: RNE rounds 65520 and above to infinity; the saturating models stop at 65504.
    return uint16_t(sign | (saturate ? 0x7BFFu : 0x7C00u));
  }
  if (result > 65504.0) result = 65504.0;  // RTZ grid above 65504 cannot happen below 65520 except this guard
  if (flush && result < min_normal) return sign;
  if (result == 0.0) return sign;
  // Encode.
  if (result < min_normal) return uint16_t(sign | uint16_t(result / std::ldexp(1.0, -24)));
  int re = 0;
  const double rm = std::frexp(result, &re);  // result = rm * 2^re, rm in [0.5, 1)
  const uint32_t biased = uint32_t(re - 1 + 15);
  const uint32_t mantissa = uint32_t((rm * 2.0 - 1.0) * 1024.0);
  return uint16_t(sign | (biased << 10) | mantissa);
}

inline bool HalfRopIsNaN(uint16_t h) { return (h & 0x7C00u) == 0x7C00u && (h & 0x3FFu); }

// The test inputs: float32 bit patterns, 4 per texel, kHalfRopTestWidth x kHalfRopTestHeight texels.
inline constexpr uint32_t kHalfRopTestWidth = 64;
inline constexpr uint32_t kHalfRopTestHeight = 80;

inline std::vector<uint32_t> HalfRopTestInputs() {
  std::vector<uint32_t> v;
  v.reserve(size_t(kHalfRopTestWidth) * kHalfRopTestHeight * 4);
  // Block A: every half below 2^-11 (codes 0x0000-0x0FFF): the value, the midpoint to the next half, and the
  // midpoint plus and minus one float32 ulp. This is where an epilogue needs the attachment's rounding.
  for (uint32_t code = 0; code < 0x1000; ++code) {
    const auto half_value = [](uint32_t c) {
      const uint32_t exponent = (c >> 10) & 31u, mantissa = c & 0x3FFu;
      return exponent ? std::ldexp(1.0 + mantissa / 1024.0, int(exponent) - 15) : std::ldexp(double(mantissa), -24);
    };
    const double lo = half_value(code), hi = half_value(code + 1);
    const float mid = float((lo + hi) * 0.5);  // exact in float32
    v.push_back(HalfRopBits(float(lo)));
    v.push_back(HalfRopBits(mid));
    v.push_back(HalfRopBits(mid) + 1);
    v.push_back(mid > 0.0f ? HalfRopBits(mid) - 1 : 0x80000000u);
  }
  // Block B: random normal-range values with full float32 mantissas, both signs; values around the overflow
  // boundary; infinities, NaNs and zeros; random values over a wide exponent range.
  uint64_t state = 0x9E3779B97F4A7C15ull;
  const auto next = [&]() {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return uint32_t(state >> 32);
  };
  for (uint32_t i = 0; i < 1024; ++i) {
    const uint32_t exponent = 127 - 14 + next() % 18;  // [2^-14, 2^4)
    v.push_back((exponent << 23) | (next() & 0x7FFFFFu));
  }
  for (uint32_t i = 0; i < 1024; ++i) {
    const uint32_t exponent = 127 - 14 + next() % 18;
    v.push_back(0x80000000u | (exponent << 23) | (next() & 0x7FFFFFu));
  }
  for (const float f : {65504.0f, 65505.0f, 65519.0f, 65519.996f, 65520.0f, 65521.0f, 65536.0f, 1.0e6f, -65520.0f,
                        -65504.0f, 32768.0f, 32784.0f, 32800.0f, 2048.0f, 2049.0f, 2050.0f, 2051.0f})
    v.push_back(HalfRopBits(f));
  for (const uint32_t bits : {0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7F800001u, 0xFFC00001u, 0x00000000u,
                              0x80000000u, 0x387FFFFFu, 0x38800000u, 0x33000000u, 0x33000001u, 0x32FFFFFFu,
                              0x337FFFFFu, 0x33800000u, 0x33C00000u})
    v.push_back(bits);
  while (v.size() < size_t(kHalfRopTestWidth) * kHalfRopTestHeight * 4) {
    const uint32_t exponent = 127 - 30 + next() % 50;  // [2^-30, 2^20)
    v.push_back((next() & 0x80000000u) | (exponent << 23) | (next() & 0x7FFFFFu));
  }
  return v;
}

struct HalfRopVerdict {
  std::array<uint64_t, kHalfModelCount> mismatches{};
  uint64_t compared = 0;
  uint64_t skipped = 0;  // inputs whose scaled value is a float32 denormal (the GPU may flush it before the ROP)
  uint32_t best = 0;
  // First mismatches of the best model: input bits, expected half, GPU half.
  std::vector<std::array<uint32_t, 3>> examples;
};

// `gpu` holds the RGBA16F results (one half per input, same order), the pass multiplied each input by 2^bias.
inline HalfRopVerdict HalfRopEvaluate(const std::vector<uint32_t>& inputs, const uint16_t* gpu, int bias) {
  HalfRopVerdict verdict;
  for (size_t i = 0; i < inputs.size(); ++i) {
    const float x = HalfRopFloat(inputs[i]);
    const float y = float(double(x) * std::ldexp(1.0, bias));  // exact for these inputs (no float32 underflow)
    if (std::isfinite(y) && y != 0.0f && std::fabs(y) < std::ldexp(1.0f, -126)) {
      ++verdict.skipped;
      continue;
    }
    ++verdict.compared;
    for (uint32_t m = 0; m < kHalfModelCount; ++m) {
      const uint16_t expected = HalfRopConvert(y, m);
      const bool equal = HalfRopIsNaN(expected) ? HalfRopIsNaN(gpu[i]) : expected == gpu[i];
      if (!equal) ++verdict.mismatches[m];
    }
  }
  for (uint32_t m = 1; m < kHalfModelCount; ++m)
    if (verdict.mismatches[m] < verdict.mismatches[verdict.best]) verdict.best = m;
  for (size_t i = 0; i < inputs.size() && verdict.examples.size() < 4; ++i) {
    const float x = HalfRopFloat(inputs[i]);
    const float y = float(double(x) * std::ldexp(1.0, bias));
    if (std::isfinite(y) && y != 0.0f && std::fabs(y) < std::ldexp(1.0f, -126)) continue;
    const uint16_t expected = HalfRopConvert(y, verdict.best);
    const bool equal = HalfRopIsNaN(expected) ? HalfRopIsNaN(gpu[i]) : expected == gpu[i];
    if (!equal) verdict.examples.push_back({inputs[i], expected, gpu[i]});
  }
  return verdict;
}

}  // namespace me::native

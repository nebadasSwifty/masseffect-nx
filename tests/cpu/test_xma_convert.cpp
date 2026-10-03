// Host test: the NEON XmaContext::ConvertFrame path (src/audio/xma_context.cpp in the SDK) against the scalar
// loop it replaces, on random floats (NaN, inf, out of range, denormals, +-0). Both bodies are copies of the SDK
// code. Build: clang++ -std=c++23 -O2 test_xma_convert.cpp && ./a.out
#include <arm_neon.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

constexpr uint32_t kSamplesPerFrame = 512;

static inline float clamp_float(float value, float min_value, float max_value) {
  float clamped_to_min = std::isgreater(value, min_value) ? value : min_value;
  return std::isless(clamped_to_min, max_value) ? clamped_to_min : max_value;
}
static inline int16_t byte_swap16(int16_t v) { return int16_t(__builtin_bswap16(uint16_t(v))); }

static void Scalar(const uint8_t** samples, bool is_two_channel, uint8_t* output_buffer) {
  constexpr float scale = (1 << 15) - 1;
  auto out = reinterpret_cast<int16_t*>(output_buffer);
  uint32_t o = 0;
  for (uint32_t i = 0; i < kSamplesPerFrame; i++) {
    for (uint32_t j = 0; j <= uint32_t(is_two_channel); j++) {
      auto in = reinterpret_cast<const float*>(samples[j]);
      float scaled_sample = clamp_float(in[i], -1.0f, 1.0f) * scale;
      auto sample = static_cast<int16_t>(scaled_sample);
      out[o++] = byte_swap16(sample);
    }
  }
}

static void Neon(const uint8_t** samples, bool is_two_channel, uint8_t* output_buffer) {
  constexpr float scale = (1 << 15) - 1;
  auto out = reinterpret_cast<int16_t*>(output_buffer);
  const float32x4_t lo = vdupq_n_f32(-1.0f);
  const float32x4_t hi = vdupq_n_f32(1.0f);
  const float32x4_t scale_v = vdupq_n_f32(scale);
  const auto convert4 = [&](const float* in) {
    float32x4_t x = vld1q_f32(in);
    x = vbslq_f32(vcgtq_f32(x, lo), x, lo);
    x = vbslq_f32(vcltq_f32(x, hi), x, hi);
    return vmovn_s32(vcvtq_s32_f32(vmulq_f32(x, scale_v)));
  };
  const auto in_channel_0 = reinterpret_cast<const float*>(samples[0]);
  if (is_two_channel) {
    const auto in_channel_1 = reinterpret_cast<const float*>(samples[1]);
    for (uint32_t i = 0; i < kSamplesPerFrame; i += 4) {
      const int16x4x2_t z = vzip_s16(convert4(&in_channel_0[i]), convert4(&in_channel_1[i]));
      const int16x8_t both = vcombine_s16(z.val[0], z.val[1]);
      vst1q_u8(reinterpret_cast<uint8_t*>(&out[i * 2]), vrev16q_u8(vreinterpretq_u8_s16(both)));
    }
  } else {
    for (uint32_t i = 0; i < kSamplesPerFrame; i += 4) {
      vst1_u8(reinterpret_cast<uint8_t*>(&out[i]), vrev16_u8(vreinterpret_u8_s16(convert4(&in_channel_0[i]))));
    }
  }
}

int main() {
  std::mt19937_64 rng(7);
  std::vector<float> a(kSamplesPerFrame), b(kSamplesPerFrame);
  std::vector<uint8_t> o1(kSamplesPerFrame * 4), o2(kSamplesPerFrame * 4);
  int fails = 0;
  for (int it = 0; it < 20000; ++it) {
    for (auto* v : {&a, &b}) {
      for (auto& f : *v) {
        uint32_t bits;
        switch (rng() % 10) {
          case 0: bits = uint32_t(rng() % 0x00800000) | (uint32_t(rng() % 2) << 31); break;   // denormal
          case 1: bits = 0x7F800000 | (uint32_t(rng() % 2) << 31); break;                      // inf
          case 2: bits = 0x7FC00000 | uint32_t(rng() % 0x400000) | (uint32_t(rng() % 2) << 31); break;  // NaN
          case 3: bits = uint32_t(rng() % 2) << 31; break;
          case 4: { const float v2 = (float(rng() % 4000001) / 1000000.0f - 2.0f); std::memcpy(&bits, &v2, 4); break; }
          default: { const float v2 = (float(rng() % 2000001) / 1000000.0f - 1.0f) * 1.2f; std::memcpy(&bits, &v2, 4); }
        }
        std::memcpy(&f, &bits, 4);
      }
    }
    const uint8_t* ch[2] = {reinterpret_cast<const uint8_t*>(a.data()), reinterpret_cast<const uint8_t*>(b.data())};
    for (bool stereo : {false, true}) {
      std::memset(o1.data(), 0xAA, o1.size());
      std::memset(o2.data(), 0xAA, o2.size());
      Scalar(ch, stereo, o1.data());
      Neon(ch, stereo, o2.data());
      if (std::memcmp(o1.data(), o2.data(), o1.size()) != 0) {
        if (fails++ < 5) std::printf("MISMATCH iteration %d stereo %d\n", it, int(stereo));
      }
    }
  }
  std::printf("ConvertFrame NEON vs scalar: 20000 iterations x 2 layouts, %d mismatches\n", fails);
  return fails != 0;
}

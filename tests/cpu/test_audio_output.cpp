// Host test: the NEON output stage of the Switch audio system (sdk/include/rex/audio/conversion.h:
// sequential_6_BE_fold_add_interleaved_2_LE and mix_to_s16) against the scalar code it replaced in
// sdk/src/audio/switch/switch_audio_system.cpp (copied below), on random frames (NaN, inf, denormals, +-0, out of range).
// Build: clang++ -std=c++23 -O2 -ffp-contract=off -I sdk/include test_audio_output.cpp && ./a.out
#include <rex/audio/conversion.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace {

constexpr size_t kChannelSamples = 256;

// ---- the replaced scalar code --------------------------------------------------------------------------------
void RefFold(float* output, const float* input, size_t n, const rex::audio::StereoFold& fold, float gain) {
  const float scale = fold.scale * gain;
  for (size_t sample = 0; sample < n; sample++) {
    const float fl = rex::byte_swap(input[0 * n + sample]);
    const float fr = rex::byte_swap(input[1 * n + sample]);
    const float fc = rex::byte_swap(input[2 * n + sample]);
    const float lf = rex::byte_swap(input[3 * n + sample]);
    const float bl = rex::byte_swap(input[4 * n + sample]);
    const float br = rex::byte_swap(input[5 * n + sample]);
    const float mid = fc * fold.center + lf * fold.lfe;
    output[sample * 2] = std::clamp((fl + mid + bl * fold.surround) * scale, -1.0f, 1.0f);
    output[sample * 2 + 1] = std::clamp((fr + mid + br * fold.surround) * scale, -1.0f, 1.0f);
  }
}
void RefMixFrameInto(float* stereo_out, const float* frame, const rex::audio::StereoFold& fold, float gain) {
  float folded[kChannelSamples * 2];
  RefFold(folded, frame, kChannelSamples, fold, gain);
  for (size_t i = 0; i < kChannelSamples * 2; ++i) stereo_out[i] += folded[i];
}
rex::audio::conversion::MixStats RefQuantize(int16_t* samples, const float* mix, size_t count) {
  rex::audio::conversion::MixStats st;
  for (size_t i = 0; i < count; ++i) {
    const float value = std::isfinite(mix[i]) ? mix[i] : 0.0f;
    const float magnitude = std::fabs(value);
    st.peak = std::max(st.peak, magnitude);
    st.saturated += magnitude > 1.0f ? 1 : 0;
    samples[i] = static_cast<int16_t>(std::lrint(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
    st.nonzero |= samples[i] != 0;
  }
  return st;
}

std::mt19937_64 rng(77);
uint32_t R(uint32_t n) { return uint32_t(rng() % n); }
float RandomFloat(bool wild) {
  uint32_t bits;
  switch (wild ? R(12) : 6) {
    case 0: bits = R(0x00800000) | (R(2) << 31); break;                 // denormal
    case 1: bits = 0x7F800000 | (R(2) << 31); break;                    // inf
    case 2: bits = 0x7FC00000 | R(0x400000) | (R(2) << 31); break;      // NaN
    case 3: bits = R(2) << 31; break;                                   // +-0
    case 4: bits = 0x7F7FFFFF - R(1000); break;                         // huge
    case 5: { const float v = float(R(2001)) / 1000.0f - 1.0f; std::memcpy(&bits, &v, 4); break; }
    default: {
      const float v = (float(R(2000001)) / 1000000.0f - 1.0f) * (R(6) == 0 ? 3.0f : 0.8f);
      std::memcpy(&bits, &v, 4);
    }
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
// Equal bit patterns, or both NaN (the payload of a NaN result depends on operand order the compiler may commute).
bool SameF(float a, float b) {
  if (std::isnan(a) && std::isnan(b)) return true;
  return std::memcmp(&a, &b, 4) == 0;
}

}  // namespace

int main() {
  int fails = 0;
  const int kIters = 4000;
  std::vector<float> frame(6 * kChannelSamples), ref(kChannelSamples * 2), nat(kChannelSamples * 2);
  for (int it = 0; it < kIters; ++it) {
    const bool wild = R(4) == 0;
    rex::audio::StereoFold fold;
    if (R(3) == 0) {
      fold.center = RandomFloat(false);
      fold.surround = RandomFloat(false);
      fold.lfe = R(2) ? 0.0f : RandomFloat(false);
      fold.scale = RandomFloat(false);
    }
    const float gain = R(3) ? 1.0f : RandomFloat(R(10) == 0);
    for (auto& v : frame) {
      const float f = RandomFloat(wild);
      v = rex::byte_swap(f);  // the guest frame is big-endian
    }
    // the accumulator: 1..3 earlier clients already mixed in
    for (size_t i = 0; i < ref.size(); ++i) ref[i] = nat[i] = R(3) ? 0.0f : RandomFloat(false) * 0.5f;
    RefMixFrameInto(ref.data(), frame.data(), fold, gain);
    rex::audio::conversion::sequential_6_BE_fold_add_interleaved_2_LE(nat.data(), frame.data(), kChannelSamples, fold,
                                                                      gain);
    for (size_t i = 0; i < ref.size(); ++i) {
      if (!SameF(ref[i], nat[i])) {
        if (fails++ < 10) std::printf("fold mismatch iter %d i %zu: ref %a native %a\n", it, i, ref[i], nat[i]);
        break;
      }
    }
    // the quantizer, on the accumulated mix (and on wild values directly); odd lengths exercise the scalar tail
    std::vector<float> mix(ref.size() * 4);
    for (size_t i = 0; i < mix.size(); ++i) mix[i] = (R(2) ? ref[i % ref.size()] : RandomFloat(wild)) * (R(8) == 0 ? 1.5f : 1.0f);
    const size_t count = R(5) == 0 ? R(uint32_t(mix.size())) : mix.size();
    std::vector<int16_t> qa(mix.size(), 0x5555), qb(mix.size(), 0x5555);
    const auto sa = RefQuantize(qa.data(), mix.data(), count);
    const auto sb = rex::audio::conversion::mix_to_s16(qb.data(), mix.data(), count);
    if (qa != qb || std::memcmp(&sa.peak, &sb.peak, 4) != 0 || sa.saturated != sb.saturated || sa.nonzero != sb.nonzero) {
      if (fails++ < 10)
        std::printf("quantize mismatch iter %d (count %zu): peak %a/%a saturated %u/%u nonzero %d/%d pcm %s\n", it, count,
                    sa.peak, sb.peak, sa.saturated, sb.saturated, int(sa.nonzero), int(sb.nonzero),
                    qa == qb ? "same" : "differs");
    }
  }
  std::printf("audio output stage: %d iterations, %d failures\n", kIters, fails);
  return fails ? 1 : 0;
}

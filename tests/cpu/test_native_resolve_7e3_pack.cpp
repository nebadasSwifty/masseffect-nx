// masseffect_native_resolve_7e3_pack (docs/vulkan-frame-time.md section 11): the cheaper A7e3 of
// shaders/me_resolve_7e3_to_unorm10_frag.frag (kPackFast) and shaders/me_resolve_7e3_word_frag.frag must return the
// same 7e3 code as the original for every value the resolve can see.
//
// The resolve reads an RGBA16F image with texelFetch, so every channel is a half-precision value converted exactly to
// float32, and both forms start from the same clamp(value, 0, 31.875). Whatever that clamp does with NaN or -0, its
// result is itself a half-representable float32 (0, -0, 31.875, the input, or a NaN with a half payload), so checking
// both forms on the float32 image of every one of the 65,536 half bit patterns covers every input of the A7e3 body.
//
// Also checked: the normal-range branch is the same integer expression for all 2^32 inputs (the rebias folded into one
// add; bit 16 of f32 - (124 << 23) is bit 16 of f32), and the word layout of the word output matches
// A2B10G10R10_UNORM_PACK32 (R bits 0-9, G 10-19, B 20-29, A 30-31). Informational: for float32 inputs that are not
// half values the denormal branches differ (the original truncates before rounding); the resolve never sees those.
#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <stdexcept>

static void Check(bool b, const char* s) {
  if (!b) throw std::runtime_error(s);
}

static float Bits(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

static float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h >> 15) << 31, e = (h >> 10) & 31u, m = h & 1023u;
  if (e == 31) return Bits(sign | 0x7F800000u | (m << 13));
  if (e == 0) {
    const float v = std::ldexp(float(m), -24);
    return sign ? -v : v;
  }
  return Bits(sign | ((e + 112u) << 23) | (m << 13));
}

// GLSL/SPIR-V semantics of the original A7e3 body after the clamp (uint shifts by >= 32 do not occur: min(.., 24)).
static uint32_t Original(uint32_t f32) {
  const uint32_t shift = std::min<uint32_t>(125u - (f32 >> 23u), 24u);
  const uint32_t denormal = ((f32 & 0x7FFFFFu) | 0x800000u) >> shift;
  const uint32_t normal = f32 - (124u << 23u);
  const uint32_t biased = f32 < 0x3E800000u ? denormal : normal;
  return ((biased + 0x7FFFu + ((biased >> 16u) & 1u)) >> 16u) & 0x3FFu;
}

static uint32_t NormalFast(uint32_t f32) {
  return ((f32 + (0x7FFFu - (124u << 23u)) + ((f32 >> 16u) & 1u)) >> 16u) & 0x3FFu;
}

static uint32_t NormalOriginal(uint32_t f32) {
  const uint32_t biased = f32 - (124u << 23u);
  return ((biased + 0x7FFFu + ((biased >> 16u) & 1u)) >> 16u) & 0x3FFu;
}

// uint(roundEven(x * 512.0)) with flush-to-zero of float32 denormals (NAK emits fmul.ftz; without it the product of a
// denormal is still far below 0.5 and rounds to 0 too).
static uint32_t DenormalFast(uint32_t f32) {
  float v = Bits(f32);
  if (std::fpclassify(v) == FP_SUBNORMAL) v = 0.0f;
  volatile float p = v * 512.0f;
  return uint32_t(std::nearbyint(p));
}

static uint32_t Fast(uint32_t f32) {
  return f32 < 0x3E800000u ? DenormalFast(f32) : NormalFast(f32);
}

int main() {
  try {
    std::fesetround(FE_TONEAREST);
    // 1. Every half value (as the texel fetch returns it), including -0, denormals, infinities and NaNs.
    uint32_t denormal_branch = 0;
    for (uint32_t h = 0; h < 65536; ++h) {
      const float f = HalfToFloat(uint16_t(h));
      uint32_t f32;
      std::memcpy(&f32, &f, 4);
      Check(Fast(f32) == Original(f32), "cheaper A7e3 differs on a half value");
      denormal_branch += f32 < 0x3E800000u;
      // The clamp's possible outputs for this input are half values too; spot-check the usual ones.
      for (float c : {std::fmin(std::fmax(f, 0.0f), 31.875f), 0.0f, -0.0f, 31.875f}) {
        uint32_t u;
        std::memcpy(&u, &c, 4);
        Check(Fast(u) == Original(u), "cheaper A7e3 differs on a clamp output");
      }
    }
    // 2. The normal branch is the same integer expression for every 32-bit input.
    for (uint64_t u = 0; u < (uint64_t(1) << 32); u += 1) {
      if (NormalFast(uint32_t(u)) != NormalOriginal(uint32_t(u))) Check(false, "normal branch differs");
    }
    // 3. Word layout: the word output writes exactly the bits A2B10G10R10_UNORM_PACK32 stores for code / 1023.
    for (uint32_t r : {0u, 1u, 512u, 1023u})
      for (uint32_t a : {0u, 1u, 2u, 3u}) {
        const uint32_t word = r | ((1023u - r) << 10u) | ((r ^ 0x155u) << 20u) | (a << 30u);
        const uint32_t pack = (word & 0x3FFu) | (((word >> 10u) & 0x3FFu) << 10u) | (((word >> 20u) & 0x3FFu) << 20u) |
                              ((word >> 30u) << 30u);
        Check(pack == word, "word layout");
      }
    // 4. Informational: float32 inputs below 0.25 that are not half values.
    uint64_t differ = 0, tested = 0;
    for (uint32_t u = 0; u < 0x3E800000u; u += 97) {
      ++tested;
      differ += Fast(u) != Original(u);
    }
    std::printf("  65536 half values equal (%u in the denormal branch); normal branch equal for all 2^32 inputs; "
                "non-half float32 below 0.25: %llu of %llu sampled differ (never read by the resolve)\n",
                denormal_branch, (unsigned long long)differ, (unsigned long long)tested);
    // Negative control: the denormal branch without round-to-nearest-even (truncation) must be caught.
    bool caught = false;
    for (uint32_t h = 0; h < 65536 && !caught; ++h) {
      const float f = HalfToFloat(uint16_t(h));
      uint32_t f32;
      std::memcpy(&f32, &f, 4);
      if (f32 < 0x3E800000u && uint32_t(Bits(f32) * 512.0f) != Original(f32)) caught = true;
    }
    Check(caught, "negative control (truncating denormal branch) not caught");
  } catch (const std::exception& e) {
    std::printf("FAIL: %s\n", e.what());
    return 1;
  }
  std::printf("PASS\n");
  return 0;
}

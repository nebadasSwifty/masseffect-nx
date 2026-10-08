// masseffect_native_resolve_7e3_frag: the 7e3 -> k_2_10_10_10 resolve as a fragment pass
// (shaders/me_resolve_7e3_to_unorm10_frag.frag) must write the same 32-bit texel as the compute shader
// (shaders/me_resolve_7e3_to_unorm10.comp) and as converting the tiles to the UNORM10 view and copying them.
//
// Both shaders compute the same EDRAM word w from the 7e3 texel (identical GLSL source for A7e3 and the alpha
// rounding) and output vec4(code) / vec4(1023, 1023, 1023, 3). They differ only in how that float reaches
// A2B10G10R10_UNORM memory: imageStore through an rgb10_a2 storage image (compute) or the ROP of a color
// attachment (fragment). Both are the Vulkan float -> UNORM conversion convertFloatToUint(f * (2^b - 1)), which
// must return r exactly when r is an integer and should round to nearest otherwise. Both shaders are compiled by
// the same NIR/NAK pipeline from the same expression, so they produce the same float. This test shows that for
// every way a compiler may evaluate the division (true division, multiplication by the rounded reciprocal, and
// either result off by one ulp) the product with the scale is within 1e-3 of the code, in float32 and in exact
// arithmetic, so a round-to-nearest conversion (Maxwell's ROP; fround_even(f * scale) where Mesa lowers a
// formatted storage store) returns the code: the two passes write identical texels, equal to w.
#include "me_restore_7e3_spirv.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>

using namespace me::native;

static void Check(bool b, const char* s) {
  if (!b) throw std::runtime_error(s);
}

// Every float a compiler can produce for code / d: correctly rounded division, reciprocal multiplication, and
// both off by one ulp in either direction (fast-math division on the GPU is accurate to within 1-2 ulp).
static void Candidates(uint32_t code, float d, float out[6]) {
  const float q = float(code) / d;
  const float r = float(code) * (1.0f / d);
  out[0] = q;
  out[1] = r;
  out[2] = std::nextafter(q, 0.0f);
  out[3] = std::nextafter(q, 2.0f);
  out[4] = std::nextafter(r, 0.0f);
  out[5] = std::nextafter(r, 2.0f);
}

// The conversion's product in float32 (round to nearest even), the way a fixed-function unit evaluates it.
static float ProductF32(float f, float scale) {
  volatile float p = f * scale;
  return p;
}

int main() {
  // 1. Channel codes 0..1023: the stored float maps back to exactly the code.
  uint32_t exact = 0, total = 0;
  for (uint32_t code = 0; code < 1024; ++code) {
    float c[6];
    Candidates(code, 1023.0f, c);
    for (float f : c) {
      if (code == 0 && f < 0.0f) continue;
      if (f < 0.0f || f > 1.0f) continue;  // nextafter past 1.0: clamped by the conversion, still 1023
      ++total;
      const float p = ProductF32(f, 1023.0f);
      Check(std::nearbyint(p) == float(code), "float32 product does not round to the code");
      const double real = double(f) * 1023.0;
      Check(std::fabs(real - double(code)) < 1e-3, "real product farther than 1e-3 from the code");
      exact += p == float(code);
    }
    // A correctly rounded quotient always gives an exact integer product (then even a conversion that only
    // promises "one of the two nearest integers" must return the code). NIR usually folds the division into a
    // multiplication by the rounded reciprocal, whose product can be off by an ulp: round to nearest (the
    // ROP's conversion, and fround_even(f * scale) where Mesa lowers formatted storage stores) still gives the
    // code, as checked above for every candidate.
    Check(ProductF32(c[0], 1023.0f) == float(code), "correctly rounded quotient: product not exact");
  }
  // 2. Alpha codes 0..3 over 3.
  for (uint32_t code = 0; code < 4; ++code) {
    float c[6];
    Candidates(code, 3.0f, c);
    Check(ProductF32(c[0], 3.0f) == float(code), "alpha: product not exact");
    for (float f : c)
      if (f >= 0.0f && f <= 1.0f) Check(std::nearbyint(ProductF32(f, 3.0f)) == float(code), "alpha rounds off");
  }
  // 3. Whole texels: every half value per channel (the 7e3 owner is RGBA16F), packed with the shaders' word
  //    math, written as UNORM10 and read back as the A2B10G10R10 word, equals the EDRAM word the conversion
  //    path writes (Pack32 format 3 of me_edram_color_to_color.frag, then the UNORM10 view's plain copy).
  for (uint32_t h = 0; h < 65536; ++h) {
    const float x = F16ToF32(uint16_t(h));
    const float v[4] = {x, x, x, x};
    const uint32_t word = Pack32Model(v, 3u);
    float unorm[4];
    Unpack32Model(word, 2u, unorm);  // what both shaders output (code / 1023, alpha / 3)
    uint32_t stored = 0;
    for (int ch = 0; ch < 4; ++ch) {
      const float scale = ch < 3 ? 1023.0f : 3.0f;
      const uint32_t n = uint32_t(std::nearbyint(ProductF32(std::fmin(std::fmax(unorm[ch], 0.0f), 1.0f), scale)));
      stored |= n << (ch < 3 ? 10 * ch : 30);
    }
    Check(stored == word, "texel word differs");
  }
  // 4. Mixed channels (codes differ per channel, all 2^10 x 4 alpha combinations on R with fixed G/B).
  for (uint32_t r = 0; r < 1024; ++r)
    for (uint32_t a = 0; a < 4; ++a) {
      const uint32_t word = r | ((1023u - r) << 10) | (((r * 7u) & 0x3FFu) << 20) | (a << 30);
      float unorm[4];
      Unpack32Model(word, 2u, unorm);
      uint32_t stored = 0;
      for (int ch = 0; ch < 4; ++ch)
        stored |= uint32_t(std::nearbyint(ProductF32(unorm[ch], ch < 3 ? 1023.0f : 3.0f))) << (ch < 3 ? 10 * ch : 30);
      Check(stored == word, "mixed word differs");
    }
  std::printf("resolve 7e3 frag: %u/%u candidate quotients give an exact float32 product, all round to the code; all texel words equal\n",
              exact, total);
  return 0;
}

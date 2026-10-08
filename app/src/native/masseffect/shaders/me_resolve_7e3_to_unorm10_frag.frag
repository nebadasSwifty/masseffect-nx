#version 450
// The resolve of me_resolve_7e3_to_unorm10.comp as a fragment pass (masseffect_native_resolve_7e3_frag): the
// k_2_10_10_10 (UNORM10) view of a surface whose EDRAM tiles all belong to the k_2_10_10_10_FLOAT (7e3) image is
// read straight from that image. The 32-bit EDRAM word is packed from the 7e3 values exactly as in the compute
// shader (Pack32 format 3 of me_edram_color_to_color.frag) and written through an A2B10G10R10_UNORM color
// attachment as code / 1023 (alpha code / 3), the same float the compute shader stores through its rgb10_a2
// storage image; both go through the same float -> UNORM10 conversion, which returns the code
// (tests/cpu/test_native_resolve_7e3_frag.cpp). The viewport is the destination rectangle and the render pass
// loads the attachment, so the texels outside it keep their values.
layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Resolve {
  ivec2 source_offset;
  ivec2 destination_offset;
  int unused;
} resolve;
layout(location = 0) out vec4 output_value;

// masseffect_native_resolve_7e3_pack >= 1 (variant 4): the cheaper form of A7e3 below, the same code for every value
// an RGBA16F texel can hold (tests/cpu/test_native_resolve_7e3_pack.cpp). Off: the original form.
layout(constant_id = 0) const bool kPackFast = false;

// Same word math as me_resolve_7e3_to_unorm10.comp and me_edram_color_to_color.frag (Pack32 format 3).
uint A7e3(float value) {
  uint f32 = floatBitsToUint(clamp(value, 0.0, 31.875));
  if (kPackFast) {
    // Normal 7e3 (>= 0.25): the original's rounding at bit 16 with the exponent rebias folded into one add (bit 16
    // of f32 - (124 << 23) is bit 16 of f32). Denormal 7e3 (< 0.25): round-to-nearest-even of value * 512, which
    // the original computes with integer shifts; equal for half-precision inputs. The branch test is the original
    // integer compare, so -0 and NaN take the same branch as before.
    uint normal = ((f32 + (0x7FFFu - (124u << 23u)) + ((f32 >> 16u) & 1u)) >> 16u) & 0x3FFu;
    uint denormal = uint(roundEven(uintBitsToFloat(f32) * 512.0));
    return f32 < 0x3E800000u ? denormal : normal;
  }
  uint denormal = ((f32 & 0x7FFFFFu) | 0x800000u) >> min(125u - (f32 >> 23u), 24u);
  uint normal = f32 - (124u << 23u);
  uint biased = f32 < 0x3E800000u ? denormal : normal;
  return ((biased + 0x7FFFu + ((biased >> 16u) & 1u)) >> 16u) & 0x3FFu;
}

void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy) - resolve.destination_offset + resolve.source_offset;
  vec4 v = texelFetch(source, pixel, 0);
  uint word = A7e3(v.r) | (A7e3(v.g) << 10u) | (A7e3(v.b) << 20u) |
              (uint(round(clamp(v.a, 0.0, 1.0) * 3.0)) << 30u);
  output_value = vec4(float(word & 0x3FFu), float((word >> 10u) & 0x3FFu), float((word >> 20u) & 0x3FFu),
                      float(word >> 30u)) / vec4(1023.0, 1023.0, 1023.0, 3.0);
}

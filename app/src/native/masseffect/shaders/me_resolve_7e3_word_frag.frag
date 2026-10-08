#version 450
// masseffect_native_resolve_7e3_pack = 2: me_resolve_7e3_to_unorm10_frag.frag with the cheaper A7e3 form, writing
// the 32-bit EDRAM word itself through an R32_UINT view of the A2B10G10R10 texture (created with
// VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; same 32-bit compatibility class). A2B10G10R10_UNORM_PACK32 keeps R in bits
// 0-9, G in 10-19, B in 20-29 and A in 30-31, the layout of the word, so the texel holds exactly the code the
// float path stores through the UNORM conversion (tests/cpu/test_native_resolve_7e3_frag.cpp), without the four
// int -> float conversions and multiplies. The viewport is the destination rectangle and the render pass loads the
// attachment, so the texels outside it keep their values.
layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Resolve {
  ivec2 source_offset;
  ivec2 destination_offset;
  int unused;
} resolve;
layout(location = 0) out uint output_word;

// The cheaper A7e3 of me_resolve_7e3_to_unorm10_frag.frag (kPackFast), see tests/cpu/test_native_resolve_7e3_pack.cpp.
uint A7e3(float value) {
  uint f32 = floatBitsToUint(clamp(value, 0.0, 31.875));
  uint normal = ((f32 + (0x7FFFu - (124u << 23u)) + ((f32 >> 16u) & 1u)) >> 16u) & 0x3FFu;
  uint denormal = uint(roundEven(uintBitsToFloat(f32) * 512.0));
  return f32 < 0x3E800000u ? denormal : normal;
}

void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy) - resolve.destination_offset + resolve.source_offset;
  vec4 v = texelFetch(source, pixel, 0);
  output_word = A7e3(v.r) | (A7e3(v.g) << 10u) | (A7e3(v.b) << 20u) |
                (uint(round(clamp(v.a, 0.0, 1.0) * 3.0)) << 30u);
}

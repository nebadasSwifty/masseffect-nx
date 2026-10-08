#version 450
// Xenos resolve of a k_16_16 render target (masseffect_native_velocity_16_16, me_fixed16_spirv.h) as a fragment
// pass. The source R16G16_UNORM image holds the EDRAM word itself (texel = word / 65535): every 16-bit channel is
// a signed fixed-point number, value = max(int16 * 32 / 32767, -32) (xenia XeUnpackR16G16Edram). The resolve
// multiplies the value by 2^copy_dest_exp_bias and packs it into the destination format by copy_dest_number
// (xenia XePackFixed, 16 bits): 0 unsigned fraction, 1 signed fraction, 2 unsigned integer, 3 signed integer.
// pack16 != 0: the destination is R16G16_UNORM and the packed 16-bit pattern is written exactly (word + 0.25) /
// 65535, which a UNORM16 attachment turns back into `word` whether it rounds to nearest or toward zero.
// pack16 == 0: any other normalized destination (number 0 only); the saturated value is written and the attachment
// quantizes it. Channels z and w are 0 (a two-channel source, as in xenia's resolve). The viewport is the
// destination rectangle and the render pass loads the attachment, so the texels outside it keep their values.
layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Resolve {
  ivec2 source_offset;
  ivec2 destination_offset;
  int exponent_bias;
  uint number;
  uint pack16;
} resolve;
layout(location = 0) out vec4 output_value;

float Decode(float texel) {
  uint word = uint(roundEven(clamp(texel, 0.0, 1.0) * 65535.0));
  int value = int(word << 16) >> 16;
  return max(float(value) * (32.0 / 32767.0), -32.0);
}

float Pack16(float v) {
  if (resolve.number == 1u) {
    float c = clamp(v, -1.0, 1.0);
    return float(uint(int(c * 32767.0 + (v >= 0.0 ? 0.5 : -0.5))) & 0xFFFFu);
  }
  if (resolve.number == 2u) return float(uint(clamp(v, 0.0, 65535.0) + 0.5));
  if (resolve.number == 3u) {
    float c = clamp(v, -32768.0, 32767.0);
    return float(uint(int(c + (v >= 0.0 ? 0.5 : -0.5))) & 0xFFFFu);
  }
  return float(uint(clamp(v, 0.0, 1.0) * 65535.0 + 0.5));
}

void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy) - resolve.destination_offset + resolve.source_offset;
  vec2 texel = texelFetch(source, pixel, 0).xy;
  vec2 value = vec2(Decode(texel.x), Decode(texel.y)) * exp2(float(resolve.exponent_bias));
  if (resolve.pack16 != 0u) {
    output_value = vec4((vec2(Pack16(value.x), Pack16(value.y)) + 0.25) * (1.0 / 65535.0), 0.0, 0.0);
  } else {
    output_value = vec4(clamp(value, 0.0, 1.0), 0.0, 0.0);
  }
}

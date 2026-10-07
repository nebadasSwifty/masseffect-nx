#version 450
// Xenos convert-resolve with an exponent bias (the HDR chain asks for -3 = 1/8) as a fragment pass: the same math
// as me_resolve_exp_bias.comp (every component times 2^bias, no other change), written through a color attachment
// instead of storage images (masseffect_native_resolve_bias_frag). The viewport is the destination rectangle and
// the render pass loads the attachment, so the texels outside it keep their values.
layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Resolve {
  ivec2 source_offset;
  ivec2 destination_offset;
  int exponent_bias;
} resolve;
layout(location = 0) out vec4 output_value;
void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy) - resolve.destination_offset + resolve.source_offset;
  output_value = texelFetch(source, pixel, 0) * exp2(float(resolve.exponent_bias));
}

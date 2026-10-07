#version 450

// Mode-4 EDRAM transfer between two 32-bit color views (RGBA8 / UNORM10 / 7e3 classes) as a fragment pass.
// Same word math as masseffect_edram_7e3_to_rgba8 / masseffect_edram_rgba8_to_7e3 / masseffect_edram_16f_to_16f (compute), but
// driven by the destination texel: no storage-image traffic, no 3D <-> compute engine switch. The viewport
// and scissor are the run's tile rectangle; fragments outside the run's tile range discard (loadOp = LOAD
// keeps them), exactly like the compute version leaves them untouched.
layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Constants {
  uvec2 source_size;
  uvec2 target_size;
  uint pitch_tiles_source;
  uint pitch_tiles_target;
  uint tile_source_start;
  uint tile_target_start;
  uint tiles_count;
  uint source_format;   // class (0 RGBA8, 2 UNORM10, 3 7e3), bit 16 = the host view is RGBA8, bit 17 = same layout
  uint target_format;  // idem
  uint source_64bpp;     // always 0 here
  uint target_64bpp;    // always 0 here
  uint source_msaa_x, source_msaa_y;
  uint target_msaa_x, target_msaa_y;
} c;
layout(location = 0) out vec4 output_value;

uint A7e3(float value) {
  uint f32 = floatBitsToUint(clamp(value, 0.0, 31.875));
  uint denormal = ((f32 & 0x7FFFFFu) | 0x800000u) >> min(125u - (f32 >> 23u), 24u);
  uint normal = f32 - (124u << 23u);
  uint biased = f32 < 0x3E800000u ? denormal : normal;
  return ((biased + 0x7FFFu + ((biased >> 16u) & 1u)) >> 16u) & 0x3FFu;
}

float From7e3(uint f10) {
  f10 &= 0x3FFu;
  if (f10 == 0u) return 0.0;
  uint mantissa = f10 & 0x7Fu;
  uint exponent = f10 >> 7u;
  if (exponent == 0u) {
    uint displacement = 7u - uint(findMSB(mantissa));
    exponent = 1u - displacement;
    mantissa = (mantissa << displacement) & 0x7Fu;
  }
  return uintBitsToFloat(((exponent + 124u) << 23u) | (mantissa << 16u));
}

uint Pack32(vec4 v, uint format) {
  if (format == 2u) {
    uvec4 n = uvec4(round(clamp(v, 0.0, 1.0) * vec4(1023.0, 1023.0, 1023.0, 3.0)));
    return n.r | (n.g << 10u) | (n.b << 20u) | (n.a << 30u);
  }
  return A7e3(v.r) | (A7e3(v.g) << 10u) | (A7e3(v.b) << 20u) |
         (uint(round(clamp(v.a, 0.0, 1.0) * 3.0)) << 30u);
}

vec4 Unpack32(uint word, uint format) {
  if (format == 2u) {
    return vec4(float(word & 0x3FFu), float((word >> 10u) & 0x3FFu),
                float((word >> 20u) & 0x3FFu), float(word >> 30u)) /
           vec4(1023.0, 1023.0, 1023.0, 3.0);
  }
  return vec4(From7e3(word), From7e3(word >> 10u), From7e3(word >> 20u),
              float(word >> 30u) * (1.0 / 3.0));
}

void main() {
  uvec2 pixel = uvec2(gl_FragCoord.xy);
  if (any(greaterThanEqual(pixel, c.target_size))) discard;
  // The physical sample this host texel stands for (the one with zero sub-sample bits, the compute's writer).
  uvec2 physical = pixel << uvec2(c.target_msaa_x, c.target_msaa_y);
  uint tile_target = (physical.y / 16u) * c.pitch_tiles_target + physical.x / 80u;
  if (tile_target < c.tile_target_start || tile_target - c.tile_target_start >= c.tiles_count) discard;
  uvec2 p;
  if ((c.source_format & 0x20000u) != 0u) {
    // Same pitch, sample layout and tile offset on both sides (the host sets bit 17): the source texel is the
    // target texel, without the per-pixel division by the source pitch.
    p = pixel;
  } else {
    uint tile_source = c.tile_source_start + (tile_target - c.tile_target_start);
    uint word_local = physical.x % 80u;
    p = uvec2((tile_source % c.pitch_tiles_source) * 80u + word_local,
              (tile_source / c.pitch_tiles_source) * 16u + physical.y % 16u) >>
        uvec2(c.source_msaa_x, c.source_msaa_y);
  }
  uint word = 0u;
  if (!any(greaterThanEqual(p, c.source_size))) {
    vec4 v = texelFetch(source, ivec2(p), 0);
    if ((c.source_format & 0x10000u) != 0u) {
      uvec4 bytes = uvec4(round(v * 255.0));
      word = bytes.r | (bytes.g << 8u) | (bytes.b << 16u) | (bytes.a << 24u);
    } else {
      word = Pack32(v, c.source_format & 0xFFFFu);
    }
  }
  if ((c.target_format & 0x10000u) != 0u) {
    uvec4 bytes = uvec4(word, word >> 8u, word >> 16u, word >> 24u) & 0xFFu;
    output_value = vec4(bytes) * (1.0 / 255.0);
  } else {
    output_value = Unpack32(word, c.target_format & 0xFFFFu);
  }
}

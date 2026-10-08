#version 450
// Stencil-bit pass variant generated from me_edram_depth_to_depth_msaa2.frag: no gl_FragDepth (keeps early tests).

// Bounded production utility: actual Vulkan 2x -> 2x, guest native 2x.
// Same guest depth encoding / half-range flags preserve the raw host depth.
// Both views are depth: NO 40-word column swap, averaging or quantization.
// Caller must provide a 2x attachment / pipeline with full sample shading,
// LOAD both aspects, and the same nine-pass stencil replacement as the 1x path.
layout(set = 0, binding = 0) uniform sampler2DMS source_depth;
layout(set = 0, binding = 1) uniform usampler2DMS source_stencil;
// Exact 72-byte ABI of me_edram_depth_to_depth.frag.
layout(push_constant) uniform Constants {
  uvec2 source_size;
  uvec2 target_size;
  uint pitch_tiles_source;
  uint pitch_tiles_target;
  uint tile_source_start;
  uint tile_target_start;
  uint tiles_count;
  uint source_format;
  uint target_format;
  uint source_64bpp;
  uint target_64bpp;
  uint source_msaa_x, source_msaa_y;
  uint target_msaa_x, target_msaa_y;
  uint stencil_mask;
} c;

// EDRAM address math without IMUL (docs/edram-shader-imul.md; exactness: tests/cpu/test_native_edram_imul_free.cpp).
// NAK on SM50 emits the microcoded, variable-latency IMUL for every 32-bit multiply that is not by a power of two,
// IMUL.HI for a division by a constant and a long IMUL sequence for a division by a register. Integers below 2^24
// are exact in float, so these use FMUL/FFMA and conversions instead:
// - DivPitch: tile < 2^16, 1 <= pitch <= 2048; the fraction of (tile + 0.5) / pitch is at least 0.5 / pitch away
//   from an integer, far more than the error of the reciprocal, so the float quotient floors to the integer one;
// - MulSmall: a * b < 2^24 (row * pitch, quotient * pitch);
// - Div80 / Mod80: x < 2^20; Mul80 is two shifts and an add.
uint DivPitch(uint tile, uint pitch) { return uint((float(tile) + 0.5) / float(pitch)); }
uint MulSmall(uint a, uint b) { return uint(float(a) * float(b)); }
uint ModPitch(uint tile, uint pitch) { return tile - MulSmall(DivPitch(tile, pitch), pitch); }
uint Div80(uint x) { return uint(fma(float(x), 0.0125, 0.00625)); }
uint Mul80(uint x) { return (x << 6u) + (x << 4u); }
uint Mod80(uint x) { return x - Mul80(Div80(x)); }

void main() {
  // Only D24S8(0), D24FS8(1), or half-range D24FS8(257). Metadata cannot
  // silently request mixed encoding, grid X, collapsed Y or raw 64bpp.
  if ((c.source_format != 0u && c.source_format != 1u && c.source_format != 257u) ||
      c.source_format != c.target_format || c.source_64bpp != 0u || c.target_64bpp != 0u ||
      c.source_msaa_x != 0u || c.target_msaa_x != 0u ||
      c.source_msaa_y != 1u || c.target_msaa_y != 1u ||
      c.pitch_tiles_source == 0u || c.pitch_tiles_target == 0u ||
      c.pitch_tiles_source > 2048u || c.pitch_tiles_target > 2048u ||
      c.tiles_count == 0u || c.tiles_count > 2048u ||
      c.tile_source_start >= 2048u || c.tile_target_start >= 2048u ||
      c.tiles_count > 2048u - c.tile_source_start ||
      c.tiles_count > 2048u - c.tile_target_start ||
      (c.stencil_mask != 0u &&
       (c.stencil_mask > 128u || (c.stencil_mask & (c.stencil_mask - 1u)) != 0u))) discard;
  if (textureSamples(source_depth) != 2 || textureSamples(source_stencil) != 2 ||
      any(notEqual(uvec2(textureSize(source_depth)), c.source_size)) ||
      any(notEqual(uvec2(textureSize(source_stencil)), c.source_size))) discard;
  uvec2 pixel = uvec2(gl_FragCoord.xy);
  if (any(greaterThanEqual(pixel, c.target_size))) discard;
  // Vulkan standard 2x: guest top sample0 = host1, bottom sample1 = host0.
  uint sample_host = uint(gl_SampleID);
  if (sample_host > 1u) discard;
  uvec2 physical = uvec2(pixel.x, pixel.y * 2u + (sample_host ^ 1u));
  uint tile = MulSmall(physical.y >> 4u, c.pitch_tiles_target) + Div80(physical.x);
  if (tile < c.tile_target_start || tile - c.tile_target_start >= c.tiles_count) discard;
  uint source_tile = c.tile_source_start + tile - c.tile_target_start;
  uvec2 source_physical = uvec2(Mul80(ModPitch(source_tile, c.pitch_tiles_source)) + Mod80(physical.x),
                               DivPitch(source_tile, c.pitch_tiles_source) * 16u + physical.y % 16u);
  ivec2 source_pixel = ivec2(source_physical.x, source_physical.y / 2u);
  if (any(greaterThanEqual(uvec2(source_pixel), c.source_size))) discard;
  int source_sample = int((source_physical.y & 1u) ^ 1u);
  uint stencil = texelFetch(source_stencil, source_pixel, source_sample).r & 255u;
  if (c.stencil_mask != 0u && (stencil & c.stencil_mask) == 0u) discard;
}

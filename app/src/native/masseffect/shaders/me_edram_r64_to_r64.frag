#version 450

// Mode-4 EDRAM transfer between two 64-bit (FP16 x4) color views as a fragment pass: raw UINT texels are copied
// (all half-float bit patterns survive, NaN payloads included), only the tile addressing changes. Same mapping
// as me_edram_raw64_to_raw64.comp driven by the destination texel; fragments outside the run's tile range
// discard (loadOp = LOAD keeps them). Source: R16G16B16A16_UINT view; destination: R16G16B16A16_UINT view
// of the destination image as a color attachment.
layout(set = 0, binding = 0) uniform usampler2D source;
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
  uint source_64bpp;     // always 1 here
  uint target_64bpp;    // always 1 here
  uint source_msaa_x, source_msaa_y;
  uint target_msaa_x, target_msaa_y;
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
// 64-bit views: 40 pixels per tile row; x < 2^20.
uint Div40(uint x) { return uint(fma(float(x), 0.025, 0.0125)); }
uint Mul40(uint x) { return (x << 5u) + (x << 3u); }
uint Mod40(uint x) { return x - Mul40(Div40(x)); }
layout(location = 0) out uvec4 output_value;

void main() {
  uvec2 pixel = uvec2(gl_FragCoord.xy);
  if (any(greaterThanEqual(pixel, c.target_size))) discard;
  // 64-bit tiles are 40 pixels wide (80 words); the compute writer of a texel is the physical sample with zero
  // sub-sample bits.
  uvec2 physical = pixel << uvec2(c.target_msaa_x, c.target_msaa_y);
  uint tile_target = MulSmall(physical.y >> 4u, c.pitch_tiles_target) + Div40(physical.x);
  if (tile_target < c.tile_target_start || tile_target - c.tile_target_start >= c.tiles_count) discard;
  uint tile_source = c.tile_source_start + (tile_target - c.tile_target_start);
  uvec2 p = uvec2(Mul40(ModPitch(tile_source, c.pitch_tiles_source)) + Mod40(physical.x),
                  DivPitch(tile_source, c.pitch_tiles_source) * 16u + physical.y % 16u) >>
            uvec2(c.source_msaa_x, c.source_msaa_y);
  output_value = any(greaterThanEqual(p, c.source_size)) ? uvec4(0u) : texelFetch(source, ivec2(p), 0);
}

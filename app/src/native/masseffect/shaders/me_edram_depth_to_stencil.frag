#version 450

// Stencil-bit pass of a depth->depth EDRAM import (one bit per draw, stencil write mask = bit,
// reference = bit): no depth fetch, no gl_FragDepth, so the pass keeps early tests. Generated from
// me_edram_depth_to_depth.frag; keep the addressing identical.
// Depth->depth EDRAM transfer. Same guest encoding preserves host depth
// precision (SDK host-target contract), including across sample layouts.
// Different guest formats reinterpret the packed 24-bit depth + stencil word.
// Both source and destination are depth: do NOT swap the 40-word tile columns.
layout(set = 0, binding = 0) uniform sampler2D source_depth;
layout(set = 0, binding = 1) uniform usampler2D source_stencil;
layout(push_constant) uniform Constants {
  uvec2 source_size;
  uvec2 target_size;
  uint pitch_tiles_source;
  uint pitch_tiles_target;
  uint tile_source_start;
  uint tile_target_start;
  uint tiles_count;
  uint source_format; // Depth: 0 D24S8, 1 D24FS8.
  uint target_format;
  uint source_64bpp;
  uint target_64bpp;
  uint source_msaa_x, source_msaa_y;
  uint target_msaa_x, target_msaa_y;
  uint stencil_mask;
} c;


void main() {
  if (c.source_64bpp != 0u || c.target_64bpp != 0u || c.tiles_count == 0u ||
      c.pitch_tiles_source == 0u || c.pitch_tiles_target == 0u ||
      (c.source_format & 255u) > 1u || (c.target_format & 255u) > 1u) discard;
  uvec2 pixel = uvec2(gl_FragCoord.xy);
  if (any(greaterThanEqual(pixel, c.target_size))) discard;
  uvec2 physical = pixel << uvec2(c.target_msaa_x, c.target_msaa_y);
  uint tile = (physical.y / 16u) * c.pitch_tiles_target + physical.x / 80u;
  if (tile < c.tile_target_start || tile - c.tile_target_start >= c.tiles_count) discard;
  uint source_tile = c.tile_source_start + tile - c.tile_target_start;
  uvec2 source_pixel = uvec2((source_tile % c.pitch_tiles_source) * 80u + physical.x % 80u,
                            (source_tile / c.pitch_tiles_source) * 16u + physical.y % 16u) >>
                       uvec2(c.source_msaa_x, c.source_msaa_y);
  if (any(greaterThanEqual(source_pixel, c.source_size))) discard;
  uint stencil = texelFetch(source_stencil, ivec2(source_pixel), 0).r & 255u;
  if ((stencil & c.stencil_mask) == 0u) discard;
}

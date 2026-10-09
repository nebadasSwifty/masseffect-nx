// Mass Effect EDRAM mode 4: the buffer-to-image regions of a copy-engine stencil import (CopyStencilEDRAM4 in
// masseffect_native_targets.cpp). The compute pass writes one byte per destination pixel of `rect` into a buffer
// whose rows are rect_width bytes apart; the regions then copy the tiles [target_start, +count) of a 1x depth view
// (80x16-pixel tiles, `pitch` tiles per row) from that buffer into the STENCIL aspect.
//
// merge_rows (masseffect_native_edram4_stencil_copy_rows): a region that continues the previous one (same columns,
// the next texel rows) extends it instead of being added. On NVK every region of a D32S8 stencil copy is two
// copy-engine launches (the second one non-pipelined), so a 720-tile (1280x720) import goes from 45 regions to 1.
// tests/cpu/test_native_stencil_copy_regions.cpp shows that both plans write the same buffer byte into every texel.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace me::native {

struct StencilCopyRegion {
  uint64_t buffer_offset = 0;  // bytes; buffer rows are rect_width bytes apart (bufferRowLength = rect_width)
  uint32_t x = 0, y = 0, width = 0, height = 0;  // destination texels
};

// Fills `regions` (cleared first, capacity kept: the ring thread passes a reused vector).
inline void PlanStencilCopyRegions(std::vector<StencilCopyRegion>& regions, uint32_t pitch, uint32_t target_width,
                                   uint32_t target_height, uint32_t rect_x, uint32_t rect_y, uint32_t rect_width,
                                   uint32_t target_start, uint32_t count, bool merge_rows,
                                   uint64_t* merged = nullptr) {
  regions.clear();
  if (!pitch) return;
  for (uint32_t t = 0; t < count;) {
    const uint32_t tile = target_start + t, row = tile / pitch, col = tile % pitch;
    const uint32_t n = std::min(count - t, pitch - col);
    const uint32_t x = col * 80u, y = row * 16u;
    t += n;
    if (x >= target_width || y >= target_height) continue;
    StencilCopyRegion r;
    r.buffer_offset = uint64_t(y - rect_y) * rect_width + (x - rect_x);
    r.x = x;
    r.y = y;
    r.width = std::min(n * 80u, target_width - x);
    r.height = std::min(16u, target_height - y);
    if (merge_rows && !regions.empty()) {
      StencilCopyRegion& last = regions.back();
      if (last.x == r.x && last.width == r.width && last.y + last.height == r.y &&
          r.buffer_offset == last.buffer_offset + uint64_t(last.height) * rect_width) {
        last.height += r.height;
        if (merged) ++*merged;
        continue;
      }
    }
    regions.push_back(r);
  }
}

inline std::vector<StencilCopyRegion> PlanStencilCopyRegions(uint32_t pitch, uint32_t target_width,
                                                             uint32_t target_height, uint32_t rect_x,
                                                             uint32_t rect_y, uint32_t rect_width,
                                                             uint32_t target_start, uint32_t count,
                                                             bool merge_rows, uint64_t* merged = nullptr) {
  std::vector<StencilCopyRegion> regions;
  PlanStencilCopyRegions(regions, pitch, target_width, target_height, rect_x, rect_y, rect_width, target_start,
                         count, merge_rows, merged);
  return regions;
}

}  // namespace me::native

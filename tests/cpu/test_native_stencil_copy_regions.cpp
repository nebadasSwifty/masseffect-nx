// masseffect_native_edram4_stencil_copy_rows (docs/vulkan-frame-time.md section 10): the copy-engine stencil
// import may write consecutive whole tile rows with one buffer-to-image region instead of one region per row.
//
// vkCmdCopyBufferToImage with bufferRowLength L writes, for a region at texel (x0, y0) of extent w x h starting
// at buffer offset o (1 byte per texel), texel (x, y) <- buffer[o + (y - y0) * L + (x - x0)]. This test builds both
// plans with the function CopyStencilEDRAM4 uses (me_stencil_copy_regions.h) for every tile range of several
// pitches and target sizes, applies them to a per-row span map and checks that every texel receives the same buffer
// byte (and that the same texels are written), that no texel is written twice, and that the merged plan of a whole
// 1280x720 view (pitch 16, 720 tiles) is a single region.
#include "me_stencil_copy_regions.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

using namespace me::native;

static void Check(bool b, const char* s) {
  if (!b) throw std::runtime_error(s);
}

// The import's buffer rectangle for tiles [start, +count): the scissor ImportColorDepthEDRAM4 computes.
static void Rect(uint32_t pitch, uint32_t w, uint32_t h, uint32_t start, uint32_t count, uint32_t& rx,
                 uint32_t& ry, uint32_t& rw, uint32_t& rh) {
  const uint32_t first_row = start / pitch, last_row = (start + count - 1) / pitch;
  rx = first_row == last_row ? (start % pitch) * 80u : 0;
  ry = first_row * 16u;
  const uint32_t right = first_row == last_row ? std::min(w, ((start + count - 1) % pitch + 1) * 80u) : w;
  const uint32_t bottom = std::min(h, (last_row + 1) * 16u);
  rw = right > rx ? right - rx : 0;
  rh = bottom > ry ? bottom - ry : 0;
}

// Per texel row, the written spans (x begin, x end, buffer byte of x begin), sorted and joined where the bytes
// continue: two plans write the same byte into every texel exactly when these lists are equal. false if a texel
// is written twice.
struct Span {
  uint32_t x0, x1;
  uint64_t byte;
  bool operator==(const Span& o) const { return x0 == o.x0 && x1 == o.x1 && byte == o.byte; }
};
static bool Apply(const std::vector<StencilCopyRegion>& plan, uint32_t w, uint32_t h, uint32_t row_length,
                  std::vector<std::vector<Span>>& rows) {
  rows.assign(h, {});
  for (const auto& r : plan) {
    Check(r.x + r.width <= w && r.y + r.height <= h, "region outside the image");
    for (uint32_t y = r.y; y < r.y + r.height; ++y)
      rows[y].push_back({r.x, r.x + r.width, r.buffer_offset + uint64_t(y - r.y) * row_length});
  }
  for (auto& spans : rows) {
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.x0 < b.x0; });
    std::vector<Span> joined;
    for (const Span& s : spans) {
      if (!joined.empty() && s.x0 < joined.back().x1) return false;
      if (!joined.empty() && s.x0 == joined.back().x1 && s.byte == joined.back().byte + (s.x0 - joined.back().x0))
        joined.back().x1 = s.x1;
      else
        joined.push_back(s);
    }
    spans.swap(joined);
  }
  return true;
}

int main() {
  try {
    struct Shape { uint32_t pitch, width, height; };
    const Shape shapes[] = {{16, 1280, 720}, {16, 1280, 1280}, {12, 960, 544}, {12, 960, 960},
                            {11, 880, 880}, {10, 800, 800}, {4, 320, 182}};
    uint64_t ranges = 0, merged_total = 0;
    for (const Shape& s : shapes) {
      const uint32_t rows = (s.height + 15) / 16, tiles = s.pitch * rows;
      for (uint32_t start = 0; start < tiles; ++start)
        for (uint32_t count = 1; start + count <= tiles; count += (count < 40 ? 1 : 7)) {
          uint32_t rx, ry, rw, rh;
          Rect(s.pitch, s.width, s.height, start, count, rx, ry, rw, rh);
          if (!rw || !rh || rw % 4) continue;  // CopyStencilEDRAM4 refuses these rectangles
          uint64_t merged = 0;
          const auto a = PlanStencilCopyRegions(s.pitch, s.width, s.height, rx, ry, rw, start, count, false);
          const auto b = PlanStencilCopyRegions(s.pitch, s.width, s.height, rx, ry, rw, start, count, true, &merged);
          Check(b.size() + merged == a.size(), "merge count");
          std::vector<std::vector<Span>> ma, mb;
          Check(Apply(a, s.width, s.height, rw, ma), "a texel written twice (per-row plan)");
          Check(Apply(b, s.width, s.height, rw, mb), "a texel written twice (merged plan)");
          Check(ma == mb, "the merged plan writes another byte into some texel");
          // Every written byte lies inside the buffer the compute pass filled (rw x rh).
          for (const auto& spans : mb)
            for (const Span& sp : spans)
              Check(sp.byte + (sp.x1 - sp.x0) <= uint64_t(rw) * rh, "byte outside the buffer");
          ++ranges;
          merged_total += merged;
        }
    }
    // The 1280x720 case of the cockpit (720 tiles from tile 0): 45 regions become 1.
    uint32_t rx, ry, rw, rh;
    Rect(16, 1280, 1280, 0, 720, rx, ry, rw, rh);
    const auto a = PlanStencilCopyRegions(16, 1280, 1280, rx, ry, rw, 0, 720, false);
    const auto b = PlanStencilCopyRegions(16, 1280, 1280, rx, ry, rw, 0, 720, true);
    Check(a.size() == 45 && b.size() == 1 && b[0].width == 1280 && b[0].height == 720, "1280x720 plan");
    // Negative control: a merge that ignored the buffer row stride would be caught by the comparison.
    {
      auto bad = a;
      bad[1].buffer_offset += 1;
      std::vector<std::vector<Span>> ma, mb;
      Apply(a, 1280, 1280, rw, ma);
      Apply(bad, 1280, 1280, rw, mb);
      Check(ma != mb, "negative control not detected");
    }
    std::printf("stencil copy regions: %llu tile ranges, %llu regions merged, plans identical texel for texel\n",
                static_cast<unsigned long long>(ranges), static_cast<unsigned long long>(merged_total));
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}

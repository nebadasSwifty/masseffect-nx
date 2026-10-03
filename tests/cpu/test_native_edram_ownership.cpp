#include "me_edram_ownership.h"
#include <cassert>
#include <cstdio>

using namespace me::native;

int main() {
  EdramBoundRequest r;
  r.pitch_pixels = 1280;
  r.msaa = 1;
  r.used_height_pixels = 512;
  r.depth_used = true;
  r.normalized_color_mask = 15;
  r.views[1].base = 0x400;
  auto p = PlanEdramBoundRanges(r);
  assert(p.valid && p.active_mask == 3 && p.pitch_tiles_at_32bpp == 16);
  assert(p.length_tiles[0] == 1024 && p.length_tiles[1] == 1024);
  r.views[1].wide = true;
  p = PlanEdramBoundRanges(r);
  assert(p.length_tiles[0] == 512 && p.length_tiles[1] == 1024);

  r = {};
  r.pitch_pixels = 640;
  r.used_height_pixels = 512;
  r.depth_used = true;
  r.views[0].base = 0x2D0;
  p = PlanEdramBoundRanges(r);
  assert(p.valid && p.active_mask == 1 && p.length_tiles[0] == 256);
  r.msaa = 1;
  assert(PlanEdramBoundRanges(r).length_tiles[0] == 512);
  r.msaa = 2;
  assert(PlanEdramBoundRanges(r).length_tiles[0] == 1024);

  // Same-base depth loses to color0; color1 also loses to color0 even with a
  // different width. A masked-off color0 must not suppress enabled color1.
  r = {};
  r.pitch_pixels = 1280;
  r.used_height_pixels = 512;
  r.depth_used = true;
  r.normalized_color_mask = 0xFF;
  r.views[0].base = r.views[1].base = r.views[2].base = 0x400;
  r.views[2].wide = true;
  p = PlanEdramBoundRanges(r);
  assert(p.active_mask == 2 && p.suppressed_mask == 5 && p.length_tiles[1] == 512);
  r.normalized_color_mask = 0x10;  // one live channel suffices
  p = PlanEdramBoundRanges(r);
  assert(p.active_mask == 4 && p.suppressed_mask == 1 && p.length_tiles[2] == 1024);

  // Sorted attachment order and wrapped next-base distance, independent of
  // slots: [2000, 2048) + [0, 100) is 148 tiles, not 48.
  r = {};
  r.pitch_pixels = 1280;
  r.used_height_pixels = 8192;
  r.normalized_color_mask = 0xFF;
  r.views[1].base = 2000;
  r.views[2].base = 100;
  p = PlanEdramBoundRanges(r);
  assert(p.sorted_slots[0] == 2 && p.sorted_slots[1] == 1);
  assert(p.length_tiles[1] == 148 && p.length_tiles[2] == 148);
  r.clamp_to_min = false;
  p = PlanEdramBoundRanges(r);
  assert(p.length_tiles[1] == 148 && p.length_tiles[2] == 1900);

  // SDK mixed 32/64bpp example: 1200px pitch, 720px height.
  r = {};
  r.pitch_pixels = 1200;
  r.used_height_pixels = 720;
  r.depth_used = true;
  r.normalized_color_mask = 15;
  r.views[1] = {675, true};
  p = PlanEdramBoundRanges(r);
  assert(p.length_tiles[0] == 675 && p.length_tiles[1] == 1350);

  // Exhaustively ensure each valid split contains exactly length tiles and
  // no tile twice, including nonzero relative offset and a full period.
  for (uint32_t base : {0u, 100u, 0x2D0u, 2000u, 2047u}) {
    for (uint32_t offset : {0u, 31u, 2047u}) {
      for (uint32_t length = 0; length <= 2048; ++length) {
        const auto w = WrapEdramRange(base, offset, length);
        assert(w.valid);
        std::array<bool, 2048> seen{};
        uint32_t total = 0;
        for (uint32_t span = 0; span < w.count; ++span) {
          assert(w.spans[span].end <= 2048 && w.spans[span].start < w.spans[span].end);
          for (uint32_t tile = w.spans[span].start; tile < w.spans[span].end; ++tile) {
            assert(!seen[tile]);
            seen[tile] = true;
            ++total;
          }
        }
        assert(total == length);
        for (uint32_t i = 0; i < length; ++i) assert(seen[(base + offset + i) & 2047]);
      }
    }
  }
  assert(WrapEdramRange(7, 2048, 0).valid);
  assert(!WrapEdramRange(7, 2048, 1).valid);
  assert(!WrapEdramRange(2048, 0, 1).valid);
  assert(!WrapEdramRange(0, 0, 2049).valid);
  r.msaa = 3;
  assert(!PlanEdramBoundRanges(r).valid);
  r.msaa = 0;
  r.pitch_pixels = 0;
  assert(PlanEdramBoundRanges(r).valid && !PlanEdramBoundRanges(r).active_mask);
  EdramClearRequest c;
  c.depth_base = 0;
  c.color_base = 0x400;
  c.pitch_tiles_at_32bpp = 16;
  c.msaa = 1;
  c.clear_depth = c.clear_color = true;
  c.width_pixels = 1280;
  c.height_pixels = 720;
  auto cp = PlanEdramClearRanges(c);
  assert(cp.valid && cp.depth.start == 0 && cp.color.start == 0);
  assert(cp.depth.length == 1024 && cp.color.length == 1024);
  c.height_pixels = 512;
  cp = PlanEdramClearRanges(c);
  assert(cp.depth.length == 1024 && cp.color.length == 1024);
  auto cr = PlanEdramClearRectangles(cp.depth, 16, 1, false, 1280, 1280,
      {0, 0, 1280, 720});
  assert(cr.valid && cr.count == 1 && cr.rectangles[0].width == 1280 &&
      cr.rectangles[0].height == 512 && !cr.whole(1280, 1280));
  c.y_pixels = 512;
  c.height_pixels = 208;
  cp = PlanEdramClearRanges(c);
  assert(cp.depth.start == 1024 && cp.color.start == 1024);
  assert(cp.depth.length == 416 && cp.color.length == 416);
  cr = PlanEdramClearRectangles(cp.depth, 16, 1, false, 1280, 1280,
      {0, 512, 1280, 208});
  assert(cr.valid && cr.count == 1 && cr.rectangles[0].y == 512 &&
      cr.rectangles[0].height == 208);

  // Short rows: the SDK ownership span includes gaps, not just the one tile
  // actually cleared per row. Nonzero X/Y are applied before 64bpp scaling.
  c = {};
  c.pitch_tiles_at_32bpp = 16;
  c.clear_color = true;
  c.x_pixels = 80;
  c.y_pixels = 16;
  c.width_pixels = 8;
  c.height_pixels = 32;
  cp = PlanEdramClearRanges(c);
  assert(cp.valid && cp.color.start == 17 && cp.color.length == 17);
  cr = PlanEdramClearRectangles(cp.color, 16, 0, false, 1280, 1280,
      {80, 16, 8, 32});
  assert(cr.valid && cr.count == 2 && cr.rectangles[0].x == 80 &&
      cr.rectangles[0].width == 8 && cr.rectangles[0].height == 16 &&
      cr.rectangles[1].y == 32 && cr.rectangles[1].width == 8);
  c.color_wide = true;
  cp = PlanEdramClearRanges(c);
  assert(cp.color.start == 34 && cp.color.length == 34);
  cr = PlanEdramClearRectangles(cp.color, 32, 0, true, 1280, 1280,
      {80, 16, 8, 32});
  assert(cr.valid && cr.count == 2 && cr.rectangles[0].x == 80 && cr.rectangles[1].y == 32);
  assert(!PlanEdramClearRectangles({0, 16}, 16, 0, false, 1279, 1280,
      {0, 0, 1279, 16}).valid);
  assert(!PlanEdramClearRectangles({16, 16}, 16, 0, false, 1280, 31,
      {0, 16, 1280, 15}).valid);

  // Mutual clamp uses WRAPPED starts, not base order or relative start order.
  c = {};
  c.depth_base = 1900;
  c.color_base = 300;
  c.pitch_tiles_at_32bpp = 16;
  c.clear_depth = c.clear_color = true;
  c.y_pixels = 160;
  c.width_pixels = 1280;
  c.height_pixels = 160;
  cp = PlanEdramClearRanges(c);
  assert(cp.depth.start == 160 && cp.color.start == 160);
  assert(cp.depth.length == 160 && cp.color.length == 160);
  c.depth_base = c.color_base;
  cp = PlanEdramClearRanges(c);
  assert(!cp.depth.length && !cp.color.length);
  // Wide color starts at twice the 32bpp relative address: same bases alone
  // don't imply coincident wrapped starts.
  c.color_wide = true;
  cp = PlanEdramClearRanges(c);
  assert(cp.depth.length == 160 && cp.color.length == 320);
  c.color_base = (c.depth_base - 160) & 2047;
  cp = PlanEdramClearRanges(c);
  assert(!cp.depth.length && !cp.color.length);

  c = {};
  c.pitch_tiles_at_32bpp = 16;
  c.clear_depth = c.clear_color = true;
  c.color_wide = true;
  c.y_pixels = 1024;
  c.width_pixels = 1280;
  c.height_pixels = 2048;
  cp = PlanEdramClearRanges(c);
  assert(cp.depth.start == 1024 && cp.depth.length == 1024);
  assert(cp.color.start == 2048 && !cp.color.length);
  c.y_pixels = 2048;
  cp = PlanEdramClearRanges(c);
  assert(cp.depth.start == 2048 && !cp.depth.length && !cp.color.length);
  c.msaa = 3;
  assert(!PlanEdramClearRanges(c).valid);
  c.msaa = 0;
  c.width_pixels = 1281;
  assert(!PlanEdramClearRanges(c).valid);
  // Independent literal SDK uint32 range formulas on hardware-bounded inputs.
  uint32_t rng = 0x4D5307E6;
  const auto next = [&]() { rng = rng * 1664525u + 1013904223u; return rng; };
  for (uint32_t trial = 0; trial < 10000; ++trial) {
    c = {};
    c.msaa = next() % 3;
    c.pitch_tiles_at_32bpp = 1 + next() % 100;
    c.depth_base = next() & 2047;
    c.color_base = next() & 2047;
    c.clear_depth = (next() >> 8) & 1;
    c.clear_color = (next() >> 8) & 1;
    c.color_wide = (next() >> 8) & 1;
    const uint32_t mx = c.msaa >= 2, my = c.msaa >= 1;
    const uint32_t width = c.pitch_tiles_at_32bpp * (80 >> mx);
    c.x_pixels = next() % width;
    c.width_pixels = 1 + next() % (width - c.x_pixels);
    c.y_pixels = next() % 4096;
    c.height_pixels = 1 + next() % 2048;
    const uint32_t start32 = ((c.y_pixels << my) / 16) * c.pitch_tiles_at_32bpp +
        (c.x_pixels << mx) / 80;
    const uint32_t length32 = (((c.y_pixels + c.height_pixels - 1) << my) / 16) *
        c.pitch_tiles_at_32bpp + ((c.x_pixels + c.width_pixels - 1) << mx) / 80 + 1 - start32;
    uint32_t ds = 0, dl = 0, cs = 0, cl = 0;
    if (c.clear_depth) {
      ds = std::min(start32, 2048u);
      dl = std::min(start32 + length32, 2048u) - ds;
    }
    if (c.clear_color) {
      cs = std::min(start32 << c.color_wide, 2048u);
      cl = std::min((start32 + length32) << c.color_wide, 2048u) - cs;
    }
    if (dl && cl) {
      const uint32_t dw = (c.depth_base + ds) & 2047;
      const uint32_t cw = (c.color_base + cs) & 2047;
      dl = std::min(dl, (cw < dw ? 2048u : 0u) + cw - dw);
      cl = std::min(cl, (dw < cw ? 2048u : 0u) + dw - cw);
    }
    cp = PlanEdramClearRanges(c);
    assert(cp.valid && cp.depth.start == ds && cp.depth.length == dl &&
        cp.color.start == cs && cp.color.length == cl);
    std::array<bool, 2048> physical_depth{};
    for (uint32_t tile = ds; tile < ds + dl; ++tile)
      physical_depth[(c.depth_base + tile) & 2047] = true;
    for (uint32_t tile = cs; tile < cs + cl; ++tile)
      assert(!physical_depth[(c.color_base + tile) & 2047]);
  }
  std::puts("EDRAM ownership: SDK range clamp, priorities, MSAA and wrapped ranges passed");
}

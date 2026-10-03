#include "me_draw_extent.h"
#include "me_edram_ownership.h"

#include <cassert>
#include <cstdio>
#include <limits>

using namespace me::native;

static uint32_t Bits(float f) { return std::bit_cast<uint32_t>(f); }
static uint32_t Y(int32_t y) { return (uint32_t(y) & 0x7FFFu) << 16; }

static DrawExtentRegisters Shadow() {
  DrawExtentRegisters r;
  r.window_scissor_br = Y(720);
  r.screen_scissor_br = Y(8192);
  r.vertex_control = 1;
  r.vte_control = 12;
  r.viewport_y_scale = Bits(-220.0f);
  r.viewport_y_offset = Bits(220.0f);
  return r;
}

int main() {
  auto r = Shadow();
  assert(EstimateDrawMaxY(r) == 440);
  assert(EstimateDrawUsedHeight(r, 440, 2, 2048) == 440);
  EdramBoundRequest request;
  request.depth_used = true;
  request.pitch_pixels = 440;
  request.msaa = 2;
  request.views[0].base = 0x5A0;
  request.used_height_pixels = EstimateDrawUsedHeight(r, 440, 2, 2048);
  auto plan = PlanEdramBoundRanges(request);
  assert(plan.pitch_tiles_at_32bpp == 11 && plan.length_tiles[0] == 605);
  assert(0x5A0 + plan.length_tiles[0] == 2045);
  r.clip_control = 0x10000;
  request.used_height_pixels = EstimateDrawUsedHeight(r, 440, 2, 2048);
  assert(request.used_height_pixels == 720);
  assert(PlanEdramBoundRanges(request).length_tiles[0] == 990);
  assert(((0x5A0 + 989) & 2047) == 381);

  r = Shadow();
  r.screen_scissor_br = Y(400);
  assert(EstimateDrawMaxY(r) == 400);
  request.used_height_pixels = EstimateDrawMaxY(r);
  assert(PlanEdramBoundRanges(request).length_tiles[0] == 550);
  r.screen_scissor_br = Y(-1);
  assert(EstimateDrawMaxY(r) == 0);

  // Scissor and viewport window offset enable bits are independent.
  r = Shadow();
  r.window_offset = Y(-20);
  assert(EstimateDrawMaxY(r) == 440);
  r.sc_mode_control = 0x10000;
  assert(EstimateDrawMaxY(r) == 420);
  r.window_scissor_tl = 0x80000000;
  assert(EstimateDrawMaxY(r) == 420);
  r.clip_control = 0x10000;
  assert(EstimateDrawMaxY(r) == 720);
  r.window_scissor_tl = 0;
  assert(EstimateDrawMaxY(r) == 700);
  r.window_offset = Y(25);
  assert(EstimateDrawMaxY(r) == 745);

  r = Shadow();
  r.viewport_y_scale = Bits(220.0f);
  assert(EstimateDrawMaxY(r) == 440);
  r.viewport_y_offset = Bits(219.25f);
  assert(EstimateDrawMaxY(r) == 439);
  r.vertex_control = 0;
  assert(EstimateDrawMaxY(r) == 439);  // 439.75 truncates, not ceil.
  r.viewport_y_offset = Bits(219.75f);
  assert(EstimateDrawMaxY(r) == 440);  // Half-pixel changes the integer result.
  r.vte_control = 0;
  assert(EstimateDrawMaxY(r) == 1);
  r.vertex_control = 1;
  r.vte_control = 8;
  r.viewport_y_offset = Bits(220.0f);
  assert(EstimateDrawMaxY(r) == 221);
  r.vte_control = 4;
  assert(EstimateDrawMaxY(r) == 220);

  r = Shadow();
  r.viewport_y_offset = Bits(std::numeric_limits<float>::quiet_NaN());
  assert(EstimateDrawMaxY(r) == 0);
  r.viewport_y_offset = Bits(-std::numeric_limits<float>::infinity());
  assert(EstimateDrawMaxY(r) == 0);
  r.viewport_y_offset = Bits(std::numeric_limits<float>::infinity());
  assert(EstimateDrawMaxY(r) == 720);
  r.viewport_y_offset = Bits(-221.0f);
  assert(EstimateDrawMaxY(r) == 0);

  // Capacity ceil versus limit floor; both are essential SDK rules.
  static_assert(DrawRenderTargetHeight(11, 2, 8192) == 1496);
  static_assert(DrawRenderTargetHeight(11, 0, 8192) == 2992);
  static_assert(DrawRenderTargetHeight(16, 1, 8192) == 1024);
  static_assert(DrawRenderTargetHeight(11, 2, 439) == 432);
  static_assert(DrawRenderTargetHeight(11, 0, 439) == 432);
  static_assert(DrawRenderTargetHeight(11, 2, 2048, 2) == 1024);
  static_assert(DrawRenderTargetHeight(1, 0, 100000) == 8192);
  static_assert(DrawRenderTargetHeight(0, 0, 2048) == 0);
  static_assert(DrawRenderTargetHeight(11, 3, 2048) == 0);
  r = Shadow();
  r.clip_control = 0x10000;
  assert(EstimateDrawUsedHeight(r, 0, 2, 2048) == 0);
  assert(EstimateDrawUsedHeight(r, 8193, 2, 2048) == 0);
  assert(EstimateDrawUsedHeight(r, 440, 3, 2048) == 0);

  // All sample counts and wide-color ranges use the same guest height estimate.
  for (uint32_t msaa = 0; msaa <= 2; ++msaa) {
    request = {};
    request.pitch_pixels = 440;
    request.msaa = msaa;
    request.used_height_pixels = EstimateDrawUsedHeight(Shadow(), 440, msaa, 2048);
    request.normalized_color_mask = 15;
    const auto narrow = PlanEdramBoundRanges(request);
    request.views[1].wide = true;
    const auto wide = PlanEdramBoundRanges(request);
    assert(wide.length_tiles[1] == 2 * narrow.length_tiles[1]);
  }
  std::puts("native draw extent: PASS");
}

#include "me_edram_layout.h"
#include "me_msaa_raster_grid.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using me::native::EdramLayout;
using me::native::GetMsaaRasterGrid;

// Exactly representable binary planar gradient prevents floating-point noise
// from disguising the addressing error. No Vulkan, shaders or GPU results here.
static float Plane(uint32_t physical_x, bool increasing) {
  const float slope = 1.0f / 65536.0f;
  return 0.5f + (increasing ? slope : -slope) * float(physical_x);
}

static void CheckLayouts(uint32_t source_msaa, uint32_t destination_msaa, bool increasing) {
  const EdramLayout source(source_msaa, false), destination(destination_msaa, false);
  constexpr uint32_t physical_width = 1280;
  const uint32_t source_width = physical_width >> source.x_log2;
  const uint32_t destination_width = physical_width >> destination.x_log2;
  const uint32_t pitch_source = source.pitch_tiles(source_width);
  const uint32_t pitch_destination = destination.pitch_tiles(destination_width);
  assert(pitch_source == 16 && pitch_destination == 16);
  const auto grid = GetMsaaRasterGrid(true, source_msaa, true);
  assert(grid.valid && grid.grid_y == 1);
  std::vector<float> collapsed(source_width), preserved(source_width * grid.grid_x);
  for (uint32_t x = 0; x < source_width; ++x)
    collapsed[x] = Plane(x << source.x_log2, increasing);
  for (uint32_t raster_x = 0; raster_x < preserved.size(); ++raster_x) {
    // Inverse grid scale: raster X -> guest logical X + retained horizontal
    // sample. For 4x, floor(logicalX) alone would lose raster_x & 1.
    const uint32_t logical_x = raster_x / grid.grid_x;
    const uint32_t sub_sample_x = raster_x % grid.grid_x;
    const uint32_t physical_x = (logical_x << source.x_log2) + sub_sample_x;
    preserved[raster_x] = Plane(physical_x, increasing);
  }
  uint32_t baseline_wrong_depth = 0, baseline_rejected = 0, preserved_rejected = 0;
  for (uint32_t x = 0; x < destination_width; ++x) {
    const uint32_t destination_sample_x = x << destination.x_log2;
    const uint32_t tile = destination_sample_x / 80;
    const uint32_t local_word = destination_sample_x % 80;
    const uint32_t source_physical_x = (tile % pitch_source) * 80 + local_word;
    const uint32_t collapsed_index = source_physical_x >> source.x_log2;
    // Invert the source's logical sample collapse while retaining its chosen
    // raster-grid expansion. This is the crucial shift for 4x->2x: 1-1=0.
    const uint32_t grid_x_log2 = grid.grid_x == 2 ? 1 : 0;
    const uint32_t preserved_index = source_physical_x >> (source.x_log2 - grid_x_log2);
    assert(collapsed_index < collapsed.size() && preserved_index < preserved.size());
    const float current = Plane(destination_sample_x, increasing);
    const float old_depth = collapsed[collapsed_index];
    const float retained_depth = preserved[preserved_index];
    baseline_wrong_depth += old_depth != current;
    baseline_rejected += !(current >= old_depth);  // Guest zfunc6 GREATER_EQUAL.
    preserved_rejected += !(current >= retained_depth);
    assert(retained_depth == current);
    const bool lost_x_sample = source.x_log2 > destination.x_log2 && (x & 1);
    assert((old_depth != current) == lost_x_sample);
    if (source_msaa == 2 && destination_msaa == 1 && !increasing)
      assert((current >= old_depth) == ((x & 1) == 0));
  }
  const uint32_t expected_loss = source.x_log2 > destination.x_log2 ? destination_width / 2 : 0;
  assert(baseline_wrong_depth == expected_loss);
  assert(baseline_rejected == (increasing ? 0 : expected_loss));
  assert(preserved_rejected == 0);
}

int main() {
  static_assert(GetMsaaRasterGrid(true, 2, true).grid_x == 2);
  static_assert(GetMsaaRasterGrid(true, 2, false).grid_x == 1);
  static_assert(GetMsaaRasterGrid(false, 2, true).grid_x == 1);
  static_assert(!GetMsaaRasterGrid(true, 3, true).valid);
  assert(!GetMsaaRasterGrid(false, UINT32_MAX, false).valid);
  for (uint32_t msaa = 0; msaa < 3; ++msaa) {
    const auto disabled = GetMsaaRasterGrid(false, msaa, true);
    const auto color = GetMsaaRasterGrid(true, msaa, false);
    const auto depth = GetMsaaRasterGrid(true, msaa, true);
    assert(disabled.valid && disabled.grid_x == 1 && disabled.grid_y == 1);
    assert(color.valid && color.grid_x == 1 && color.grid_y == 1);
    assert(depth.valid && depth.grid_x == (msaa == 2 ? 2u : 1u) && depth.grid_y == 1);
    // The policy intentionally does not expand vertical MSAA sample positions.
    assert(EdramLayout(msaa, false).y_log2 == uint32_t(msaa >= 1));
    for (uint32_t destination = 0; destination < 3; ++destination) {
      CheckLayouts(msaa, destination, false);
      CheckLayouts(msaa, destination, true);
    }
  }
  std::puts("MSAA depth CPU model: all 9 layouts/both slopes; collapsed 4x640->2x1280 GEQUAL odd holes reproduced; horizontal grid retains depth. NOT GPU conformance.");
}

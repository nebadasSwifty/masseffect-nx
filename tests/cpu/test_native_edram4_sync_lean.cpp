// Host test of masseffect_native_edram4_sync_lean (masseffect_native_targets.cpp, SynchronizeEDRAM4): inside the tile
// range that RangeTilesEDRAM4 computes for an area, and over exactly the tiles the sync walk visits (rows ty0..ty1,
// columns tx0..tx1, one contiguous run when the range spans the whole pitch, from `start`, below `tiles`), the
// per-tile test TileUsedEDRAM4 is always true. The lean walk skips that test when it does not need the whole-tile
// flag (no clear overwrite, no cutout). The three functions below are copies of the ones in the targets code
// (same arithmetic); random images (any pitch, all tile shapes), areas (negative, partial, beyond the image),
// starts and limits.
//   clang++ -std=c++20 -O2 tests/cpu/test_native_edram4_sync_lean.cpp -o /tmp/t && /tmp/t
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>

struct Rect {
  int32_t x, y;
  uint32_t w, h;
};
struct Image {
  uint32_t width, height, pitch_tiles;
  bool b64;
  uint32_t msaa_x, msaa_y;
};

static bool TileUsed(const Image& image, uint32_t tile, const Rect& area) {
  const uint32_t pitch_tiles = image.pitch_tiles;
  if (!pitch_tiles || !area.w || !area.h) return false;
  const uint32_t tile_width = 80u >> (uint32_t(image.b64) + image.msaa_x);
  const uint32_t tile_height = 16u >> image.msaa_y;
  const uint32_t x = (tile % pitch_tiles) * tile_width;
  const uint32_t y = (tile / pitch_tiles) * tile_height;
  const int64_t x0 = std::max<int64_t>(0, area.x);
  const int64_t y0 = std::max<int64_t>(0, area.y);
  const int64_t x1 = int64_t(area.x) + area.w;
  const int64_t y1 = int64_t(area.y) + area.h;
  return x < image.width && y < image.height && x < x1 && y < y1 && int64_t(x + tile_width) > x0 &&
         int64_t(y + tile_height) > y0;
}

struct Range {
  uint32_t pitch = 0, tx0 = 0, tx1 = 0, ty0 = 0, ty1 = 0;
};
static bool RangeTiles(const Image& image, const Rect& area, Range& r) {
  r.pitch = image.pitch_tiles;
  if (!r.pitch || !area.w || !area.h) return false;
  const uint32_t tile_width = 80u >> (uint32_t(image.b64) + image.msaa_x);
  const uint32_t tile_height = 16u >> image.msaa_y;
  const int64_t x0 = std::max<int64_t>(0, area.x), y0 = std::max<int64_t>(0, area.y);
  const int64_t x1 = std::min<int64_t>(int64_t(area.x) + area.w, image.width);
  const int64_t y1 = std::min<int64_t>(int64_t(area.y) + area.h, image.height);
  if (x1 <= x0 || y1 <= y0) return false;
  r.tx0 = uint32_t(x0 / tile_width);
  r.tx1 = std::min<uint32_t>(r.pitch, uint32_t((x1 + tile_width - 1) / tile_width));
  r.ty0 = uint32_t(y0 / tile_height);
  r.ty1 = uint32_t((y1 + tile_height - 1) / tile_height);
  return r.tx1 > r.tx0;
}

int main() {
  std::mt19937_64 rng(12345);
  uint64_t visited = 0, failures = 0, cases = 0;
  for (int iteration = 0; iteration < 400000; ++iteration) {
    Image image{};
    image.b64 = rng() & 1;
    image.msaa_x = rng() & 1;
    image.msaa_y = rng() & 1;
    image.width = 1 + uint32_t(rng() % 1400);
    image.height = 1 + uint32_t(rng() % 1000);
    const uint32_t tile_width = 80u >> (uint32_t(image.b64) + image.msaa_x);
    const uint32_t natural = (image.width + tile_width - 1) / tile_width;
    switch (rng() % 3) {  // the natural pitch, a wider one, or any
      case 0: image.pitch_tiles = natural; break;
      case 1: image.pitch_tiles = natural + uint32_t(rng() % 4); break;
      default: image.pitch_tiles = 1 + uint32_t(rng() % 40); break;
    }
    Rect area{int32_t(rng() % 1600) - 200, int32_t(rng() % 1200) - 200, uint32_t(rng() % 1600),
              uint32_t(rng() % 1200)};
    const uint32_t tile_height = 16u >> image.msaa_y;
    const uint32_t rows = (image.height + tile_height - 1) / tile_height;
    const uint32_t limit = (rng() % 4) ? 2048u : uint32_t(rng() % 2048);
    const uint32_t tiles = std::min({image.pitch_tiles * rows, limit, 2048u});
    const uint32_t start = (rng() % 4) ? 0u : uint32_t(rng() % 2048);
    Range range;
    const bool has_range = RangeTiles(image, area, range);
    ++cases;
    // The walk's loop bounds (SynchronizeEDRAM4, phase 1).
    const bool complete_width = has_range && range.tx0 == 0 && range.tx1 == range.pitch;
    const uint32_t ty_end = complete_width ? range.ty0 + 1 : range.ty1;
    for (uint32_t ty = has_range ? range.ty0 : 0; has_range && ty < ty_end && ty * range.pitch < tiles; ++ty) {
      const uint32_t row_end =
          std::min(complete_width ? range.ty1 * range.pitch : ty * range.pitch + range.tx1, tiles);
      for (uint32_t tile = std::max(ty * range.pitch + range.tx0, start); tile < row_end; ++tile) {
        ++visited;
        if (!TileUsed(image, tile, area)) {
          if (++failures <= 10)
            std::printf("FAIL: image %ux%u pitch %u b64 %d msaa %u/%u area %d,%d+%ux%u tile %u\n", image.width,
                        image.height, image.pitch_tiles, image.b64, image.msaa_x, image.msaa_y, area.x, area.y,
                        area.w, area.h, tile);
        }
      }
    }
  }
  std::printf("%llu cases, %llu visited tiles, %llu where the area test was false\n",
              (unsigned long long)cases, (unsigned long long)visited, (unsigned long long)failures);
  if (failures) return 1;
  std::printf("PASS\n");
  return 0;
}

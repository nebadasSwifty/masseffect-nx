#include "me_edram_layout.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

int main() {
  using me::native::EdramLayout;
  static_assert(EdramLayout(0, false).pitch_tiles(1280) == 16);
  static_assert(EdramLayout(1, false).tile_height() == 8);
  static_assert(EdramLayout(2, true).tile_width() == 20);
  for (unsigned samples = 0; samples < 3; ++samples) {
    for (bool wide : {false, true}) {
      const EdramLayout l(samples, wide);
      for (unsigned pitch : {200u, 400u, 640u, 880u, 1280u}) {
        const unsigned tiles = l.pitch_tiles(pitch);
        for (unsigned y = 0; y < 512; ++y) {
          for (unsigned x = 0; x < pitch; ++x) {
            const unsigned tile = (y / l.tile_height()) * tiles + x / l.tile_width();
            const unsigned local_word = (x % l.tile_width()) << (l.words_log2 + l.x_log2);
            const unsigned sample_y = (y % l.tile_height()) << l.y_log2;
            const unsigned restored_x = ((tile % tiles) * (80 >> l.words_log2) +
                (local_word >> l.words_log2)) >> l.x_log2;
            const unsigned restored_y = ((tile / tiles) * 16 + sample_y) >> l.y_log2;
            assert(restored_x == x && restored_y == y);
            assert(local_word < 80 && sample_y < 16);
          }
        }
      }
    }
  }
  std::puts("EDRAM layout: all 1x/2x/4x, 32/64-bpp, five pitches round-trip passed");
}

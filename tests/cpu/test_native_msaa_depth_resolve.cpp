#include "me_msaa_depth_resolve.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>

int main() {
  // Explicit independent table from SDK depth/native2x semantics.
  constexpr std::array<uint32_t, 8> guest{0, 1, 0, 1, 0, 0, 0, 0};
  constexpr std::array<uint32_t, 8> host{1, 0, 1, 0, 1, 1, 1, 1};
  for (uint32_t raw = 0; raw < guest.size(); ++raw) {
    const auto selected = me::native::SanitizeNative2xDepthCopySampleSelect(raw);
    if (!selected || *selected != guest[raw] ||
        me::native::Native2xDepthHostSample(*selected) != host[raw]) return EXIT_FAILURE;
    // RB_COPY_CONTROL sample field occupies exactly bits4..6; other bits
    // cannot change the field passed by the production resolve caller.
    for (uint32_t unrelated : {0u, 0xFFFFFF8Fu, 0x8123450Fu}) {
      const uint32_t control = (unrelated & ~0x70u) | (raw << 4);
      if (me::native::SanitizeNative2xDepthCopySampleSelect((control >> 4) & 7u) != selected)
        return EXIT_FAILURE;
    }
  }
  for (uint32_t invalid : {8u, 9u, 255u, std::numeric_limits<uint32_t>::max()})
    if (me::native::SanitizeNative2xDepthCopySampleSelect(invalid)) return EXIT_FAILURE;
  for (uint32_t invalid : {2u, 3u, 4u, 7u, 8u, std::numeric_limits<uint32_t>::max()})
    if (me::native::Native2xDepthHostSample(invalid)) return EXIT_FAILURE;
  std::puts("native2x depth resolve selectors: all8 raw / invalid fields / exact host mapping PASS");
}

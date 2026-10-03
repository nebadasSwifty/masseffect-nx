#include "me_resolve.h"
#include <cmath>
#include <cstdio>
#include <initializer_list>

int main() {
  using namespace me::native;
  constexpr auto tile0 = ClipClearRectangle(0, 0, 1280, 512, 1280, 1280);
  constexpr auto tile1 = ClipClearRectangle(0, 0, 1280, 208, 1280, 1280);
  static_assert(tile0.height == 512 && !tile0.whole(1280, 1280));
  static_assert(tile1.height == 208 && !tile1.whole(1280, 1280));
  static_assert(ClipClearRectangle(-8, -8, 1300, 1400, 1280, 1280).whole(1280, 1280));
  static_assert(ClipClearRectangle(1500, 1400, 1600, 1500, 1280, 1280).width == 0);
  static_assert(ClipClearRectangle(10, 20, 5, 4, 1280, 1280).height == 0);
  static_assert(ResolveExponentBias(0x003DF001) == -3);
  static_assert(ColorResolveExponentBias(0, 0x003DF001) == -3);
  for (int32_t bias = -32; bias <= 31; ++bias) {
    const uint32_t word = (uint32_t(bias) & 63) << 16;
    for (uint32_t unrelated : {0u, 0xFFC0FFFFu}) {
      if (ResolveExponentBias(word | unrelated) != bias ||
          ColorResolveExponentBias(1, word | unrelated) != bias ||
          ColorResolveExponentBias(0, word | unrelated) != bias ||
          ColorResolveExponentBias(3, word | unrelated) != 0) return 1;
    }
  }
  if (std::ldexp(8.0f, ResolveExponentBias(0x003DF001)) != 1.0f) return 2;
  std::puts("resolve exponent: all 64 signed values, unrelated fields and Raw/Convert passed");
}

#include "me_raster_state.h"
#include <cassert>
#include <cstdio>
using namespace me::native;
static_assert(XenosDepthClamp(0x10000, true));
static_assert(!XenosDepthClamp(0x80000, true));
static_assert(!XenosDepthClamp(0x10000, false));
static_assert(XenosDepthClamp(0, true, true));
static_assert(!XenosDepthClamp(0, false, true));
static_assert(StaticRasterKey(0x3F) == kNativeDepthClamp);
static_assert(StaticRasterKey(0x7F) == (kNativeDepthClamp | kNativeMsaa2PhaseProbe));
static_assert(StaticRasterKey(0xFF) ==
              (kNativeDepthClamp | kNativeMsaa2PhaseProbe | kNativeMsaa2PhaseMode2));
static_assert(StaticRasterKey(kNativeMsaa2PhaseProbe) !=
              StaticRasterKey(kNativeMsaa2PhaseProbe | kNativeMsaa2PhaseMode2));
static_assert(GetMsaa2PhaseProbe(0, 1, false, 0).valid && !GetMsaa2PhaseProbe(0, 1, false, 0).active);
static_assert(GetMsaa2PhaseProbe(1, 1, false, 0x43F).geometry_y == .25f);
static_assert(GetMsaa2PhaseProbe(2, 1, false, 0x43F).geometry_y == -.25f);
static_assert(!GetMsaa2PhaseProbe(2, 0, false, 0x43F).active);
static_assert(!GetMsaa2PhaseProbe(2, 2, false, 0x43F).active);
static_assert(!GetMsaa2PhaseProbe(2, 1, true, 0x300).active);
static_assert(!GetMsaa2PhaseProbe(2, 1, false, 0x300).valid);
static_assert(!GetMsaa2PhaseProbe(3, 1, false, 0x43F).valid);
int main() {
  for (uint32_t state = 0; state < 32; ++state) {
    assert(StaticRasterKey(state) == 0);
    assert(StaticRasterKey(state | kNativeDepthClamp) == kNativeDepthClamp);
    assert(StaticRasterKey(state | kNativeMsaa2PhaseProbe) == kNativeMsaa2PhaseProbe);
    assert(StaticRasterKey(state | kNativeMsaa2PhaseProbe | kNativeMsaa2PhaseMode2) ==
           (kNativeMsaa2PhaseProbe | kNativeMsaa2PhaseMode2));
  }
  std::puts("Raster state: clip-disable depth clamp and static EDS key separation passed");
}

// Mass Effect - hot guest hooks: every native replacement compiled a second time with the shadow accessors, for the
// self-check guard (me_hot_guest.cpp Check; design in me_hot_shadow.h, per-hook table in docs/hot-guard.md).
//
// The n_*.h headers are expanded inside namespace me_hot_shadow_copy with ME_HOT_SHADOW_ACCESSORS defined, so their
// me::hot::Raw / St* resolve to the shadow copy of me_hot_common.h (accessors redirected to the private copy of the
// declared write ranges). Nothing here is used on the unchecked path: those calls run the normal build of the natives,
// compiled in me_hot_guest.cpp. Every system / SDK header the natives include must be included above the wrapper.
// The Russian edition uses this file unchanged: its overlay replaces the n_*.h headers and me_hot_call.h it includes.
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include <simde/x86/sse4.1.h>

#include <rex/platform.h>
#include <rex/ppc/context.h>
#include <rex/ppc/indirect_dispatch.h>

#include "me_hot_shadow.h"

#define ME_HOT_SHADOW_ACCESSORS 1
namespace me_hot_shadow_copy {
#include "me_hot_common.h"
#include "hot/hot_all.h"
#include "hot/n_8245FF18.h"  // TFieldIterator<UProperty>::IterateToNext
#include "hot/n_8264E178.h"  // UE3 sprite emitter Render with prefetch
}  // namespace me_hot_shadow_copy
#undef ME_HOT_SHADOW_ACCESSORS

// One line per hooked native (the hooks of me_hot_guest.cpp, both editions use the English namespace names).
ME_HOT_SHADOW_NATIVE(n_82219258)
ME_HOT_SHADOW_NATIVE(n_824F9D10)
ME_HOT_SHADOW_NATIVE(n_82219360)
ME_HOT_SHADOW_NATIVE(n_826545D0)
ME_HOT_SHADOW_NATIVE(n_82654030)
ME_HOT_SHADOW_NATIVE(n_822631E8)
ME_HOT_SHADOW_NATIVE(n_8256A1A0)
ME_HOT_SHADOW_NATIVE(n_8256AFD8)
ME_HOT_SHADOW_NATIVE(n_82270C78)
ME_HOT_SHADOW_NATIVE(n_822E3158)
ME_HOT_SHADOW_NATIVE(n_822B9200)
ME_HOT_SHADOW_NATIVE(n_8230D5F0)
ME_HOT_SHADOW_NATIVE(n_8267C000)
ME_HOT_SHADOW_NATIVE(n_8225CA80)
ME_HOT_SHADOW_NATIVE(n_824DD848)
ME_HOT_SHADOW_NATIVE(n_8264C7C0)
ME_HOT_SHADOW_NATIVE(n_8262CFC0)
ME_HOT_SHADOW_NATIVE(n_82BAFF58)
ME_HOT_SHADOW_NATIVE(n_82BB0748)
ME_HOT_SHADOW_NATIVE(n_82210970)
ME_HOT_SHADOW_NATIVE(n_8230F620)
ME_HOT_SHADOW_NATIVE(n_82B5F0E8)
ME_HOT_SHADOW_NATIVE(n_82AC4520)
ME_HOT_SHADOW_NATIVE(n_82AC3790)
ME_HOT_SHADOW_NATIVE(n_826EAF70)
ME_HOT_SHADOW_NATIVE(n_8264ADA0)
ME_HOT_SHADOW_NATIVE(n_8245FF18)
ME_HOT_SHADOW_NATIVE(n_8264E178)
ME_HOT_SHADOW_NATIVE(n_827D2A00)
ME_HOT_SHADOW_NATIVE(n_82AAFB90)
ME_HOT_SHADOW_NATIVE(n_82AAFE20)
ME_HOT_SHADOW_NATIVE(n_82AC4A50)

// CRT memcpy: dedicated shadow version (a source inside the destination range, see me_hot_shadow.h ShadowMemcpy).
namespace me::hot::n_82AC4AF0 {
bool ShadowNative(PPCContext& c, uint8_t* b) { return ::me::hot::ShadowMemcpy(c, b); }
}  // namespace me::hot::n_82AC4AF0

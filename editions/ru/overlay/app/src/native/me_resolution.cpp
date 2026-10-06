// Mass Effect - render the frame at a lower internal resolution (masseffect_scene_width x _height).
//
// WHY
//   On the stock Switch GPU the frame is GPU-bound. The game draws everything at 1280x720 whatever the video mode says
//   (VdQueryVideoMode only feeds the D3D scaler); our video mode 960x540 only scaled the finished frame.
//
// WHERE 1280x720 COMES FROM (static analysis of the recompiled code)
//   - Viewport: UGameEngine::Init (sub_825E1AD0) calls XenonClient::CreateViewportFrame (sub_826E5858) with
//     r6/r7 = StartupResolutionX/Y of the client (defaults 1280/720 in sub_823C68C0; ResX=/ResY= on the
//     command line override them). FXenonViewport stores them and the scene view rect follows them.
//   - Scene render targets (GSceneRenderTargets at 0x82EC287C, Allocate = sub_823D0CE8): buffer size
//     +20/+24 raised to at least 1280x720, quarter buffer +32/+36 = size/4 + 2.
//   - D3D device present parameters (CreateDevice sub_82234D98, pp+0/+4), the game's back buffer
//     (sub_82224698 called from 0x826E62B4) and front buffer (sub_82224528 from 0x826E62E4), all in the D3D
//     init sub_826E5960. Present resolves the whole back buffer into the front buffer; the SDK scales the
//     front buffer to the screen, so a smaller front buffer is upscaled at no extra cost.
//   - The Scaleform quad in sub_8223D088 (sub_823CFCA8 called from 0x8223CE7C).
//   EDRAM bases (sub_826ED020) are laid out for 1280x720, so W <= 1280 and H <= 720.
//
// Off by default (0 x 0). Not changed: the GFx viewport struct of sub_82238FE8 (UI stage), Bink movies.

#include "me_native_system.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

REXCVAR_DEFINE_INT32(masseffect_scene_width, 0, "Mass Effect",
                     "Internal render width (viewport, scene targets, back/front buffer); 0 = the game's 1280")
    .range(0, 1280)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_scene_height, 0, "Mass Effect",
                     "Internal render height (viewport, scene targets, back/front buffer); 0 = the game's 720")
    .range(0, 720)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_scene_parts, 0xFF, "Mass Effect",
                     "Which parts of the internal-resolution change apply (bisection): 1 viewport, 2 scene "
                     "targets, 4 device present parameters, 8 back buffer, 16 front buffer, 32 Scaleform quad")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(masseffect_exclusive_core, -1, "Mass Effect",
                     "Switch: pin the hottest guest thread (the game's main thread) to this core alone and move "
                     "every other thread off it (profiler, switch_perf.cpp); 10 + core = the same, light guest threads "
                     "(< 35 % of a core) allowed on it too; 20 + core = the render thread raised to priority 0x2B; 30 + core = both; -1 = off")
    .range(-1, 32)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(masseffect_cpu_boost, 0, "Mass Effect",
                     "Switch: appletSetCpuBoostMode (0 off, 1 FastLoad: CPU 1785 MHz but the GPU throttled to its "
                     "minimum by the system, 2 Type2); diagnostic, see RexSwitchApmCpuBoost")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern "C" void RexSwitchPerfExclusiveCore(int core);

// Scaleform viewport size read by the patched sub_82238FE8 (tools/pch_ui_viewport.py).
extern "C" volatile int64_t g_me_ui_width = 1280, g_me_ui_height = 720;
// HUD world-to-screen Y factor read by the patched __fast_sub_827C07F0 (tools/pch_ui_world_to_screen.py).
extern "C" volatile double g_me_ui_y_scale = 1.0;

REXCVAR_DEFINE_BOOL(masseffect_scene_ui, true, "Mass Effect",
                    "With an internal resolution set, lay the Scaleform UI stage out at that size too (viewport "
                    "of sub_82238FE8) instead of 1280x720")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v = 0;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

struct Size {
  uint32_t w = 0, h = 0, parts = 0;
};

const Size& Configured() {
  static const Size size = [] {
    Size s;
    s.w = uint32_t(REXCVAR_GET(masseffect_scene_width));
    s.h = uint32_t(REXCVAR_GET(masseffect_scene_height));
    s.parts = uint32_t(REXCVAR_GET(masseffect_scene_parts));
    if (!s.w || !s.h) s.parts = 0;
    if (s.parts && REXCVAR_GET(masseffect_scene_ui)) {
      g_me_ui_width = s.w;
      g_me_ui_height = s.h;
      g_me_ui_y_scale = 720.0 / double(s.h);
    }
    if (s.parts)
      REXLOG_INFO("[native] internal resolution {}x{} (parts {:#x})", s.w, s.h, s.parts);
    return s;
  }();
  return size;
}

inline bool Part(uint32_t bit) { return (Configured().parts & bit) != 0; }

}  // namespace

// 1. XenonClient::CreateViewportFrame(r4 client, r5 name, r6 SizeX, r7 SizeY, r8 fullscreen).
REX_EXTERN(__imp__sub_826E5858);
REX_HOOK_RAW(sub_826E5858) {
  // Not about resolution: the engine is initialised and the cvars are read by now.
  RexSwitchPerfExclusiveCore(REXCVAR_GET(masseffect_exclusive_core));
  if (Part(1)) {
    REXLOG_INFO("[native] internal resolution: viewport {}x{} -> {}x{}", ctx.r6.u32, ctx.r7.u32, Configured().w,
                Configured().h);
    ctx.r6.u64 = Configured().w;
    ctx.r7.u64 = Configured().h;
  }
  __imp__sub_826E5858(ctx, base);
}

// 2. FSceneRenderTargets::Allocate: the same as the original with W x H instead of the 1280x720 minimum.
REX_EXTERN(__imp__sub_823D0CE8);
REX_EXTERN(sub_823D20F0);  // GSceneRenderTargets vtable slot 5 (release the RHI resources)
REX_EXTERN(sub_823D2030);  // slot 4 (create them)
REX_HOOK_RAW(sub_823D0CE8) {
  if (!Part(2)) {
    __imp__sub_823D0CE8(ctx, base);
    return;
  }
  constexpr uint32_t kTargets = 0x82EC287C;
  // Same dispatch as the original (bctrl through the object's vtable); check it is the class we expect.
  const uint32_t vtable = Load32(base, kTargets);
  const uint32_t release = Load32(base, vtable + 20), init = Load32(base, vtable + 16);
  if (release != 0x823D20F0 || init != 0x823D2030) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXLOG_WARN("[native] internal resolution: unexpected vtable {:08X} ({:08X} {:08X}), left unchanged", vtable,
                  release, init);
    }
    __imp__sub_823D0CE8(ctx, base);
    return;
  }
  const uint32_t w = Configured().w, h = Configured().h;
  uint32_t x = Load32(base, kTargets + 20), y = Load32(base, kTargets + 24);
  if (x >= w && y >= h) return;
  x = std::max(x, w);
  y = std::max(y, h);
  REXLOG_INFO("[native] internal resolution: scene render targets {}x{} (requested {}x{})", x, y, ctx.r4.u32,
              ctx.r5.u32);
  Store32(base, kTargets + 20, x);
  Store32(base, kTargets + 24, y);
  Store32(base, kTargets + 28, 4);
  Store32(base, kTargets + 32, (x >> 2) + 2);
  Store32(base, kTargets + 36, (y >> 2) + 2);
  const uint32_t r3 = ctx.r3.u32;
  ctx.r3.u64 = kTargets;
  sub_823D20F0(ctx, base);
  ctx.r3.u64 = kTargets;
  sub_823D2030(ctx, base);
  ctx.r3.u64 = r3;
}

// 3-5. CreateDevice (r4 = present parameters: +0 BackBufferWidth, +4 BackBufferHeight), the game's back buffer
// (render target r3 x r4, created from 0x826E62B4) and the front buffer texture (r3 x r4, from 0x826E62E4)
// the back buffer is resolved into at Present. src/me_d3d_trace.cpp already hooks these three functions, so
// its wrappers call this before the original.
void MeResolutionBefore(uint32_t address, PPCContext& ctx, uint8_t* base) {
  if (address == 0x82234B88 && Part(4) && ctx.r4.u32) {
    REXLOG_INFO("[native] internal resolution: device {}x{} -> {}x{}", Load32(base, ctx.r4.u32),
                Load32(base, ctx.r4.u32 + 4), Configured().w, Configured().h);
    Store32(base, ctx.r4.u32 + 0, Configured().w);
    Store32(base, ctx.r4.u32 + 4, Configured().h);
  } else if ((address == 0x82224488 && Part(8) && ctx.lr == 0x826E62B4) ||
             (address == 0x82224318 && Part(16) && ctx.lr == 0x826E62E4)) {
    REXLOG_INFO("[native] internal resolution: {} {}x{} -> {}x{}",
                address == 0x82224488 ? "back buffer" : "front buffer", ctx.r3.u32, ctx.r4.u32, Configured().w,
                Configured().h);
    ctx.r3.u64 = Configured().w;
    ctx.r4.u64 = Configured().h;
  }
}

// 6. Full-screen quad of the Scaleform render path (sub_8223D088): r5 x r6.
REX_EXTERN(__imp__sub_823CFCA8);
REX_HOOK_RAW(sub_823CFCA8) {
  if (Part(32) && ctx.lr == 0x8223CE7C) {
    ctx.r5.u64 = Configured().w;
    ctx.r6.u64 = Configured().h;
  }
  __imp__sub_823CFCA8(ctx, base);
}

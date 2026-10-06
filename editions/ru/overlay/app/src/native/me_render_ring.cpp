// Mass Effect - a larger render command ring buffer (fixes the random black start).
//
// UE3 keeps the game thread -> rendering thread commands in one ring buffer (GRenderCommandBuffer), created by a
// static initializer with 256 KB (sub_82D3BDF0: appMalloc 0x40000, then start/end/read/write pointers and a
// 16-byte alignment at 0x82EC1254). While the loading movie plays, its thread owns the Direct3D device and the
// rendering thread waits for that ownership (sub_822FDEA0) without reading commands. If the game thread enqueues
// more than 256 KB before the movie stops, it spins forever on the full buffer (sub_822107C0) and never stops the
// movie: the screen stays black after the logo movies (about 1 launch in 3 on the Switch, found with the hang
// watchdog). A bigger buffer gives the loading the room it needs; the buffer logic itself is unchanged.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

REXCVAR_DEFINE_INT32(masseffect_render_ring_kb, 4096, "Mass Effect",
                     "Size of the game's render command ring buffer in KB (the game's own is 256). A larger one "
                     "avoids the black start deadlock while the loading movie owns the device. 256 = original")
    .range(256, 16384)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
constexpr uint32_t kRing = 0x82EC1254;  // FRingBuffer: +0 start, +4 end, +8 write, +12 end of the last write, +20 read,
                                        // +24 alignment
void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}
}  // namespace

REX_EXTERN(__imp__sub_82D3BDF0);
REX_EXTERN(sub_823BF1F0);  // appMalloc(size)
REX_HOOK_RAW(sub_82D3BDF0) {
  const uint32_t kb = uint32_t(std::clamp(REXCVAR_GET(masseffect_render_ring_kb), 256, 16384));
  if (kb <= 256) {
    __imp__sub_82D3BDF0(ctx, base);
    return;
  }
  const uint32_t size = kb * 1024u;
  const uint64_t r3 = ctx.r3.u64;
  ctx.r3.u64 = size;
  sub_823BF1F0(ctx, base);
  const uint32_t buffer = ctx.r3.u32;
  if (!buffer) {
    REXLOG_WARN("[native] render command buffer: {} KB not available, the game's 256 KB is used", kb);
    ctx.r3.u64 = r3;
    __imp__sub_82D3BDF0(ctx, base);
    return;
  }
  Store32(base, kRing + 0, buffer);
  Store32(base, kRing + 8, buffer);
  Store32(base, kRing + 20, buffer);
  Store32(base, kRing + 4, buffer + size);
  Store32(base, kRing + 12, buffer + size);
  Store32(base, kRing + 24, 16);
  REXLOG_INFO("[native] render command buffer: {} KB at {:08X} (the game's own is 256 KB)", kb, buffer);
}

// Mass Effect - hot guest function replacements (REX_HOOK_RAW hooks behind cvars, default off).
//
// Every hook replaces one recompiled function by an exact native version (src/native/hot/n_<addr>.h) and checks itself:
// for the first masseffect_hot_guard_calls calls (and every masseffect_hot_guard_period-th call afterwards) the native
// version runs on a copy of the registers and on a private copy of the guest ranges it writes (its shadow build,
// me_hot_shadow.cpp / me_hot_shadow.h: nothing is written to guest memory), then the original (__imp__sub_X) runs for
// real, and registers, written ranges and the FPU mode are compared. A difference logs "[hot] DIFFERENCE ..." and
// switches that hook off for the rest of the run (the original already produced the correct state). Other guest threads
// only ever see the original's stores (2026-10-09: the previous guard wrote the native result into guest memory and
// rolled it back, a race with threads sharing those bytes; per-hook notes in docs/hot-guard.md).
#include "me_hot_guest.h"
#include "me_hot_shadow.h"

#include <rex/logging.h>

#include <algorithm>
#include <string>
#include <vector>

#include "hot/hot_all.h"
#include "hot/n_8245FF18.h"  // TFieldIterator<UProperty>::IterateToNext
#include "hot/n_8264E178.h"  // UE3 sprite emitter Render with prefetch

REXCVAR_DEFINE_BOOL(masseffect_hot_guest, false, "Mass Effect",
                    "Enable every hot guest function replacement (src/native/hot); each also has its own "
                    "masseffect_hot_* switch")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_hot_guard_calls, 256, "Mass Effect",
                     "Hot guest hooks: the first N calls of each hook are checked against the recompiled original")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_hot_guard_period, 4096, "Mass Effect",
                     "Hot guest hooks: after the first N calls every P-th call is checked (rounded to a power of two)")
    .range(1, 1 << 24)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace me::hot {

void InitHook(Hook& h, bool own, bool umbrella) {
  // May run on several threads at once for the first call: every writer stores the same values.
  const bool on = own || (umbrella && REXCVAR_GET(masseffect_hot_guest));
  const uint32_t guard_calls = uint32_t(std::max(0, REXCVAR_GET(masseffect_hot_guard_calls)));
  const uint32_t period = uint32_t(std::max(1, REXCVAR_GET(masseffect_hot_guard_period)));
  uint32_t p2 = 1;
  while (p2 < period) p2 <<= 1;
  h.guard_calls = guard_calls;
  h.period_mask = p2 - 1;
  if (on) REXLOG_INFO("[hot] {}: native replacement ON (guard: first {} calls, then 1 in {})", h.name, guard_calls, p2);
  h.state.store(on ? kOn : kOff, std::memory_order_release);
}

namespace {

const PPCRegister* Gpr(const PPCContext& c, int n) {
  switch (n) {
#define G(i) case i: return &c.r##i;
    G(0) G(1) G(2) G(3) G(4) G(5) G(6) G(7) G(8) G(9) G(10) G(11) G(12) G(13) G(14) G(15) G(16) G(17) G(18) G(19)
    G(20) G(21) G(22) G(23) G(24) G(25) G(26) G(27) G(28) G(29) G(30) G(31)
#undef G
  }
  return nullptr;
}
const PPCRegister* Fpr(const PPCContext& c, int n) {
  switch (n) {
#define F(i) case i: return &c.f##i;
    F(0) F(1) F(2) F(3) F(4) F(5) F(6) F(7) F(8) F(9) F(10) F(11) F(12) F(13) F(14) F(15) F(16) F(17) F(18) F(19)
    F(20) F(21) F(22) F(23) F(24) F(25) F(26) F(27) F(28) F(29) F(30) F(31)
#undef F
  }
  return nullptr;
}
const PPCVRegister* Vr(const PPCContext& c, int n) {
  switch (n) {
#define V(i) case i: return &c.v##i;
    V(0) V(1) V(2) V(3) V(4) V(5) V(6) V(7) V(8) V(9) V(10) V(11) V(12) V(13) V(14) V(15) V(16) V(17) V(18) V(19)
    V(20) V(21) V(22) V(23) V(24) V(25) V(26) V(27) V(28) V(29) V(30) V(31)
#undef V
  }
  return nullptr;
}

// First difference between the native result (a) and the original (b), or an empty string.
std::string CompareRegs(const Cmp& cmp, const PPCContext& a, const PPCContext& b) {
  for (int i = 0; i < 32; ++i) {
    const bool volat = (i == 0 || (i >= 3 && i <= 12));
    const bool want = volat ? ((cmp.gpr >> i) & 1) != 0 : (i == 1 || i >= 13);
    if (!want) continue;
    if (Gpr(a, i)->u64 != Gpr(b, i)->u64)
      return fmt::format("r{} native={:#018x} original={:#018x}", i, Gpr(a, i)->u64, Gpr(b, i)->u64);
  }
  for (int i = 0; i < 32; ++i) {
    const bool want = i < 14 ? ((cmp.fpr >> i) & 1) != 0 : true;
    if (!want) continue;
    if (Fpr(a, i)->u64 != Fpr(b, i)->u64)
      return fmt::format("f{} native={:#018x} original={:#018x}", i, Fpr(a, i)->u64, Fpr(b, i)->u64);
  }
  for (int i = 0; i < 32; ++i) {
    const bool want = i < 14 ? ((cmp.vr >> i) & 1) != 0 : true;
    if (!want) continue;
    if (std::memcmp(Vr(a, i)->u8, Vr(b, i)->u8, 16) != 0) {
      const auto* x = Vr(a, i)->u32;
      const auto* y = Vr(b, i)->u32;
      return fmt::format("v{} native={:08x}{:08x}{:08x}{:08x} original={:08x}{:08x}{:08x}{:08x}", i, x[0], x[1], x[2],
                         x[3], y[0], y[1], y[2], y[3]);
    }
  }
  if (a.fpscr.csr != b.fpscr.csr)
    return fmt::format("fpscr.csr native={:#x} original={:#x}", a.fpscr.csr, b.fpscr.csr);
  return {};
}

// Slack pages of the guard's private copy (me_hot_shadow.h ShadowPageFn): on the Switch through the always-mapped shadow
// alias of a committed page (no fault, no commit, no watched-page read); elsewhere none (the slack stays in the declared
// pages).
#if defined(__SWITCH__)
extern "C" void* RexGmShadowFor(uint64_t window_address);
const uint8_t* GuardPage(const uint8_t* base, uint32_t page) {
  return static_cast<const uint8_t*>(RexGmShadowFor(reinterpret_cast<uint64_t>(base + page)));
}
#else
const uint8_t* GuardPage(const uint8_t*, uint32_t) { return nullptr; }
#endif

}  // namespace

void Check(Hook& h, PPCContext& ctx, uint8_t* base) {
  Writes w;
  h.writes(ctx, base, w);
  if (w.overflow) {  // cannot verify: do not trust the native version for this call
    h.orig(ctx, base);
    return;
  }
  // Private copy of the declared ranges; guest memory is only read here.
  ShadowView view;
  std::vector<uint8_t> storage;
  if (!ShadowBuild(view, w, base, ctx.r1.u32, storage, &GuardPage)) {  // a range wraps around 4 GB: cannot verify
    h.orig(ctx, base);
    return;
  }
  const uint32_t hw_before = ctx.fpscr.getcsr();
  const uint32_t csr_before = ctx.fpscr.csr;
  const PPCContext in = ctx;  // inputs for the report
  PPCContext nat = ctx;
  ShadowView* const outer = t_shadow;  // a guarded call reached from guest code that a shadow native called
  t_shadow = &view;
  const bool handled = h.shadow(nat, base);
  t_shadow = outer;
  const uint32_t hw_native = nat.fpscr.getcsr();
  ctx.fpscr.setcsr(hw_before);
  ctx.fpscr.csr = csr_before;
  h.orig(ctx, base);  // the call for real: the only writer of guest memory
  if (!handled) return;  // declined: the original ran, nothing to compare
  const uint32_t hw_orig = ctx.fpscr.getcsr();

  std::string diff = CompareRegs(h.cmp, nat, ctx);
  if (diff.empty() && hw_native != hw_orig) diff = fmt::format("hardware FPCR native={:#x} original={:#x}", hw_native, hw_orig);
  if (diff.empty()) {
    const ShadowDiff d = ShadowCompare(view, w, base);
    if (d.kind == ShadowDiff::kMemory) {
      diff = fmt::format("memory {:#010x} (+{} of range {:#010x}+{}) native={:#04x} original={:#04x}", d.addr, d.offset,
                         w.r[d.range].addr, w.r[d.range].len, d.native, d.original);
    } else if (d.kind == ShadowDiff::kStore) {
      diff = fmt::format("native stored to {:#010x}, outside its declared write ranges (store dropped)", d.addr);
    } else if (d.kind == ShadowDiff::kSlack) {
      diff = fmt::format("native wrote {:#010x} next to its declared write ranges (native={:#04x} before={:#04x})", d.addr,
                         d.native, d.original);
    }
  }
  const uint64_t n = h.checks.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!diff.empty()) {
    h.failed.store(true, std::memory_order_relaxed);
    REXLOG_ERROR(
        "[hot] DIFFERENCE {}: {}; inputs r3={:#x} r4={:#x} r5={:#x} r6={:#x} r7={:#x} r8={:#x} r9={:#x} r10={:#x}; "
        "native replacement switched off for the rest of the run",
        h.name, diff, in.r3.u64, in.r4.u64, in.r5.u64, in.r6.u64, in.r7.u64,
        in.r8.u64, in.r9.u64, in.r10.u64);
  } else if (n == h.guard_calls || (n > h.guard_calls && ((n - h.guard_calls) & 63) == 0)) {
    REXLOG_INFO("[hot] {}: guard OK after {} checks", h.name, n);
  }
}

}  // namespace me::hot

// ---------------------------------------------------------------------------------------------------------------
// Hooks. Each hook: own cvar (default off), or masseffect_hot_guest = true for all of them.

ME_HOT_CVAR(masseffect_hot_d3d_cbcopy,
            "Native sub_82219258 (D3D: copy N vectors from an unaligned source into the constant block, dirty mask)");
ME_HOT_HOOK(82219258, masseffect_hot_d3d_cbcopy, me::hot::n_82219258)

ME_HOT_CVAR(masseffect_hot_d3d_setconst4,
            "Native sub_824F9D10 (D3D: write one float4 constant, dirty bit)");
ME_HOT_HOOK(824F9D10, masseffect_hot_d3d_setconst4, me::hot::n_824F9D10)

ME_HOT_CVAR(masseffect_hot_d3d_cbcopy_ps,
            "Native sub_82219360 (D3D: pixel-shader side of the constant block copy)");
ME_HOT_HOOK(82219360, masseffect_hot_d3d_cbcopy_ps, me::hot::n_82219360)

// Verified natives (differentially fuzzed on the host, tests/hot_fuzz).
ME_HOT_CVAR(masseffect_hot_skel_bonemat, "Native sub_826545D0 (skeletal mesh bone matrices)");
ME_HOT_HOOK(826545D0, masseffect_hot_skel_bonemat, me::hot::n_826545D0)
ME_HOT_CVAR(masseffect_hot_sprite_sort, "Native sub_82654030 (UE3 particle sprite quicksort)");
ME_HOT_HOOK(82654030, masseffect_hot_sprite_sort, me::hot::n_82654030)
ME_HOT_CVAR(masseffect_hot_mat4_inverse, "Native sub_822631E8 (4x4 matrix inverse)");
ME_HOT_HOOK(822631E8, masseffect_hot_mat4_inverse, me::hot::n_822631E8)
ME_HOT_CVAR(masseffect_hot_ray_slab, "Native sub_8256A1A0 (ray vs projected-vertex slab clip)");
ME_HOT_HOOK(8256A1A0, masseffect_hot_ray_slab, me::hot::n_8256A1A0)
ME_HOT_CVAR(masseffect_hot_ray_sphere, "Native sub_8256AFD8 (ray clip with sqrt/divide)");
ME_HOT_HOOK(8256AFD8, masseffect_hot_ray_sphere, me::hot::n_8256AFD8)
ME_HOT_CVAR(masseffect_hot_cast_82270C78, "Native sub_82270C78 (Cast<T> class-chain check)");
ME_HOT_HOOK(82270C78, masseffect_hot_cast_82270C78, me::hot::n_82270C78)
ME_HOT_CVAR(masseffect_hot_cast_822E3158, "Native sub_822E3158 (Cast<T> class-chain check)");
ME_HOT_HOOK(822E3158, masseffect_hot_cast_822E3158, me::hot::n_822E3158)
ME_HOT_CVAR(masseffect_hot_cast_822B9200, "Native sub_822B9200 (Cast<T> class-chain check)");
ME_HOT_HOOK(822B9200, masseffect_hot_cast_822B9200, me::hot::n_822B9200)
ME_HOT_CVAR(masseffect_hot_hash_lookup, "Native sub_8230D5F0 (hash table lookup)");
ME_HOT_HOOK(8230D5F0, masseffect_hot_hash_lookup, me::hot::n_8230D5F0)
ME_HOT_CVAR(masseffect_hot_interval_test, "Native sub_8267C000 (swept interval test)");
ME_HOT_HOOK(8267C000, masseffect_hot_interval_test, me::hot::n_8267C000)
ME_HOT_CVAR(masseffect_hot_iter_advance, "Native sub_8225CA80 (iterator advance)");
ME_HOT_HOOK(8225CA80, masseffect_hot_iter_advance, me::hot::n_8225CA80)

// Hooks added with the second round of natives (re-verified against the diet / nal generated code).
ME_HOT_CVAR(masseffect_hot_distribution, "Native sub_824DD848 (particle distribution float table lookup + lerp / random)");
ME_HOT_HOOK(824DD848, masseffect_hot_distribution, me::hot::n_824DD848)
ME_HOT_CVAR(masseffect_hot_sprite_vertices, "Native sub_8264C7C0 (sprite emitter: dynamic vertex + index buffer fill)");
ME_HOT_HOOK(8264C7C0, masseffect_hot_sprite_vertices, me::hot::n_8264C7C0)
ME_HOT_CVAR(masseffect_hot_plane_reject, "Native sub_8262CFC0 (box vs plane groups reject test, VMX)");
ME_HOT_HOOK(8262CFC0, masseffect_hot_plane_reject, me::hot::n_8262CFC0)
ME_HOT_CVAR(masseffect_hot_hash_find, "Native sub_82BAFF58 (chained hash table index lookup)");
ME_HOT_HOOK(82BAFF58, masseffect_hot_hash_find, me::hot::n_82BAFF58)
ME_HOT_CVAR(masseffect_hot_hash_find_entry, "Native sub_82BB0748 (hash table find-entry wrapper)");
ME_HOT_HOOK(82BB0748, masseffect_hot_hash_find_entry, me::hot::n_82BB0748)
ME_HOT_CVAR(masseffect_hot_obj_iter_advance, "Native sub_82210970 (object array iterator advance with class filter)");
ME_HOT_HOOK(82210970, masseffect_hot_obj_iter_advance, me::hot::n_82210970)
ME_HOT_CVAR(masseffect_hot_object_lookup, "Native sub_8230F620 (object hash lookup by owner / key / class)");
ME_HOT_HOOK(8230F620, masseffect_hot_object_lookup, me::hot::n_8230F620)
ME_HOT_CVAR(masseffect_hot_volume_overlap, "Native sub_82B5F0E8 (bounding volume overlap test, two virtual getters)");
ME_HOT_HOOK(82B5F0E8, masseffect_hot_volume_overlap, me::hot::n_82B5F0E8)
ME_HOT_CVAR(masseffect_hot_crt_memcpy, "Native sub_82AC4AF0 (CRT memcpy)");
ME_HOT_HOOK(82AC4AF0, masseffect_hot_crt_memcpy, me::hot::n_82AC4AF0)
ME_HOT_CVAR(masseffect_hot_crt_wcscmp, "Native sub_82AC4520 (CRT wcscmp)");
ME_HOT_HOOK(82AC4520, masseffect_hot_crt_wcscmp, me::hot::n_82AC4520)
ME_HOT_CVAR(masseffect_hot_crt_wcsicmp, "Native sub_82AC3790 (CRT wcsicmp)");
ME_HOT_HOOK(82AC3790, masseffect_hot_crt_wcsicmp, me::hot::n_82AC3790)
ME_HOT_CVAR(masseffect_hot_object_hash, "Native sub_826EAF70 (object hash: CRC table + Jenkins mix)");
ME_HOT_HOOK(826EAF70, masseffect_hot_object_hash, me::hot::n_826EAF70)
ME_HOT_CVAR(masseffect_hot_skin_rebind, "Native sub_8264ADA0 (skin cache rebind: bone matrices 4x4 -> 3x4 copy)");
ME_HOT_HOOK(8264ADA0, masseffect_hot_skin_rebind, me::hot::n_8264ADA0)
// Fuzzed against the English generated code on 2026-10-08 (tests/hot_fuzz/cases/case_8245FF18.inc, case_8264E178.inc).
ME_HOT_CVAR(masseffect_hot_field_iter, "Native sub_8245FF18 (TFieldIterator<UProperty>::IterateToNext)");
ME_HOT_HOOK(8245FF18, masseffect_hot_field_iter, me::hot::n_8245FF18)
ME_HOT_CVAR(masseffect_hot_sprite_render,
            "Native sub_8264E178 (UE3 sprite emitter Render: particle sort keys with prefetch, sort, DrawRichMesh)");
ME_HOT_HOOK(8264E178, masseffect_hot_sprite_render, me::hot::n_8264E178)

// Not switched on by masseffect_hot_guest (own cvar only): added 2026-10-08, pending a console A/B.
ME_HOT_CVAR(masseffect_hot_lzo,
            "Native sub_827D2A00 (LZO1X decompressor of the package streaming threads; not part of masseffect_hot_guest)");
ME_HOT_HOOK_OWN(827D2A00, masseffect_hot_lzo, me::hot::n_827D2A00)

// Audio: the XAudio voice resamplers, mono and stereo (own cvars only, pending a console A/B; fuzzed on EN and RU,
// tests/hot_fuzz).
ME_HOT_CVAR(masseffect_hot_audio_resampler,
            "Native sub_82AAFB90 (XAudio voice resampler: s16 -> float, linear interpolation, volume ramp; NEON; not part "
            "of masseffect_hot_guest)");
ME_HOT_HOOK_OWN(82AAFB90, masseffect_hot_audio_resampler, me::hot::n_82AAFB90)
ME_HOT_CVAR(masseffect_hot_audio_resampler_stereo,
            "Native sub_82AAFE20 (XAudio voice resampler, stereo variant: interleaved s16 -> two float planes; NEON; not "
            "part of masseffect_hot_guest)");
ME_HOT_HOOK_OWN(82AAFE20, masseffect_hot_audio_resampler_stereo, me::hot::n_82AAFE20)

// CRT memset: the mixer clears its buffers with it (~6 % of the audio thread in combat); own cvar only, pending a
// console A/B.
ME_HOT_CVAR(masseffect_hot_crt_memset, "Native sub_82AC4A50 (CRT memset; not part of masseffect_hot_guest)");
ME_HOT_HOOK_OWN(82AC4A50, masseffect_hot_crt_memset, me::hot::n_82AC4A50)

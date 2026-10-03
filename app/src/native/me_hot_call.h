// Mass Effect - hot guest function replacements: calling guest code from a native replacement (see me_hot_common.h).
//
// CallIndirect() is the REX_CALL_INDIRECT_FUNC macro of the generated code (masseffect_pch.h)
// without the pch: the per-module dispatch table lookup for targets inside the recompiled code range, the slow path
// rex::runtime::ResolveIndirectFunction otherwise (or for an unregistered slot), the same ctx.last_indirect_target
// bookkeeping. The constants are the ones of the pch (REX_IMAGE_BASE / REX_IMAGE_SIZE / REX_CODE_BASE / REX_CODE_SIZE /
// REX_THUNK_RESERVE_SIZE of this recompilation: Mass Effect 1 xex; they only change if the module is re-lifted with a
// different layout, in which case the hot-guest self-check guard would also be re-validated).
#pragma once

#include "me_hot_common.h"

namespace me::hot {

inline constexpr uint64_t kImageBase = 0x82000000ull;
inline constexpr uint64_t kImageSize = 0xFC0000ull;
inline constexpr uint64_t kCodeBase = 0x82210000ull;
inline constexpr uint64_t kCodeSize = 0xB7E8A8ull;
inline constexpr uint64_t kThunkReserve = 0x10000ull;

// The caller sets ctx.lr / r1 / the argument registers exactly as the original function does before the `bctrl`.
inline void CallIndirect(PPCContext& ctx, uint8_t* base, uint32_t target) {
  PPCFunc* fn;
  if (uint32_t(target - uint32_t(kCodeBase)) < kCodeSize + kThunkReserve) [[likely]] {
    fn = *reinterpret_cast<PPCFunc**>(base + (kImageBase + kImageSize + (uint64_t(target) - kCodeBase) * 2));
  } else {
    fn = nullptr;
  }
  if (!fn) [[unlikely]] {
    ctx.last_indirect_target = target;
    fn = rex::runtime::ResolveIndirectFunction(target);
  }
  fn(ctx, base);
}

}  // namespace me::hot

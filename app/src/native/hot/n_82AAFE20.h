// sub_82AAFE20 - XAudio voice resampler, stereo variant of sub_82AAFB90 (RU edition: sub_82B2D780): interleaved L/R
// 16-bit PCM -> two float planes (left at r5, right at r5 + 1024), the same 32.32 position, linear interpolation and
// volume ramp. The code is shared with the mono version (n_82AAFB90.h, detail::Run<true>), including its .rdata
// constants (the RU overlay copy of n_82AAFB90.h carries the RU addresses, so this header needs no RU copy).
// Exactness notes: see n_82AAFB90.h; the stereo store order is reproduced (right plane first, then left [4..7], [0..3]).
#pragma once

#include "n_82AAFB90.h"

namespace me::hot::n_82AAFE20 {

inline constexpr Cmp kCmp = n_82AAFB90::kCmp;

inline bool Native(PPCContext& ctx, uint8_t* base) { return n_82AAFB90::detail::Run<true>(ctx, base); }
inline void Writes(const PPCContext& ctx, const uint8_t*, me::hot::Writes& w) {
  n_82AAFB90::detail::RunWrites<true>(ctx, w);
}

}  // namespace me::hot::n_82AAFE20

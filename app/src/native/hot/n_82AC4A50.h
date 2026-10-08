// sub_82AC4A50 - XDK CRT memset (RU edition: sub_829730C0, same instructions): r3 = dst, r4 = fill byte (low 8 bits),
// r5 = n (low 32 bits); r3 is returned unchanged.
//
// The original stores bytes up to 4-byte alignment (at most n), then 16-byte groups of the replicated word, then words,
// then bytes: exactly the bytes [dst, dst + n) receive the low byte of r4, in increasing address order (no read of guest
// memory, so the order is not observable). n == 0 stores nothing. It leaves r0, r4 (replicated pattern), r5, r6 and ctr as
// scratch; liveness.py: the 1204 direct call sites read no volatile register the function writes (two read r8, which it
// does not touch).
// Declined (the original runs): ranges that wrap around 4 GB, and ranges that cross 0xE0000000 on hosts with the +0x1000
// physical offset (not the Switch).
#pragma once

#include <cstring>

#include "../me_hot_common.h"

namespace me::hot::n_82AC4A50 {

inline constexpr Cmp kCmp = kCmpRet;

inline bool Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32;
  const uint32_t n = ctx.r5.u32;
  if (n == 0) return true;
  if (uint64_t(dst) + n > 0x100000000ull || PhysOff(dst) != PhysOff(dst + n - 1)) return false;
  std::memset(Raw(base, dst), ctx.r4.u8, n);
  return true;
}

inline void Writes(const PPCContext& ctx, const uint8_t*, me::hot::Writes& w) { w.Add(ctx.r3.u32, ctx.r5.u32); }

}  // namespace me::hot::n_82AC4A50

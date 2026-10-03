// sub_82AC9C30 - CRT wcsncpy: r3 = dst, r4 = src (UTF-16, big-endian units), r5 = n (32-bit count of units).
// r3 is not modified by the original (callers get the destination pointer back); nothing else is returned.
//
// Original: if n == 0 return; loop { copy one unit src -> dst; if it was 0: break; --n; if n == 0: return }; after a copied
// terminator pad the remaining (n - 1) units of dst with 0x0000 (n = units still outstanding including the terminator slot).
// Net effect: with k = index of the first zero unit of src, c = min(n, k + 1) units are copied (terminator included when
// k < n) and the rest of the first n units of dst is zero-filled; exactly the first n units of dst are written.
// All counts / addresses are 32-bit (cmplwi / s32 compares / ctr.u32); the upper half of r5 is ignored.
// Overlap: the original copies unit by unit forward, so for dst <= src (any offset) the copy part equals memmove; for
// dst > src with dst - src < 2 * n the original smears already-copied units: the native version DECLINES (returns false,
// nothing modified). Ranges that wrap around 4 GB are declined too.
// Scratch left by the original (r9, r10, r11, r4, r5): liveness.py: no direct call site reads any volatile register.
#pragma once

#include "../me_hot_common.h"

namespace me::hot::n_82AC9C30 {

inline constexpr Cmp kCmp = kCmpRet;

inline bool Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  if (n == 0) return true;
  const uint64_t bytes = uint64_t(n) * 2;
  if (uint64_t(dst) + bytes > 0x100000000ull || uint64_t(src) + bytes > 0x100000000ull) return false;
  if (uint32_t(dst - src - 1u) < uint32_t(bytes) - 1u && bytes <= 0xFFFFFFFFull) return false;  // dst > src, dst - src < 2n
  uint8_t* d = Raw(base, dst);
  const uint8_t* s = Raw(base, src);
  // c = units to copy: up to and including the first zero unit, at most n
  uint32_t c = 0;
  while (c < n) {
    uint16_t v;
    std::memcpy(&v, s + 2 * size_t(c), 2);
    ++c;
    if (v == 0) break;
  }
  if (uint32_t(src - dst) < uint32_t(c * 2u))
    std::memmove(d, s, size_t(c) * 2);
  else
    std::memcpy(d, s, size_t(c) * 2);
  if (c < n) std::memset(d + size_t(c) * 2, 0, size_t(n - c) * 2);
  return true;
}

inline void Writes(const PPCContext& ctx, const uint8_t*, me::hot::Writes& w) {
  const uint64_t bytes = uint64_t(ctx.r5.u32) * 2;
  if (bytes > me::hot::Writes::kMaxBytes) {
    w.overflow = true;
    return;
  }
  w.Add(ctx.r3.u32, uint32_t(bytes));
}

}  // namespace me::hot::n_82AC9C30

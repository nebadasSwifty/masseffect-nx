// sub_82AC3790 - CRT wcsicmp: r3 = string a, r4 = string b (UTF-16, big-endian code units), ASCII-range case folding
// (units 'A'..'Z' -> +32), returns r3 = fold(a_i) - fold(b_i) (64-bit, sign-extended) at the first unit where the folded units
// differ or fold(a_i) == 0 (then r3 = 0 - fold(b_i) if b_i is not also a zero unit, else 0).
//
// Null-pointer path: if r3 == 0 or r4 == 0 (32-bit part) the original calls sub_82ACBDA8 / sub_82ACBC70 (invalid parameter
// handler, errno = 22) and returns 0x7FFFFFFF: the native version DECLINES (returns false, nothing modified) and the original runs.
// The original's frame (stwu r1,-96(r1) + saving lr at -8(r1)) is callee-owned stack scratch below r1; r1 / lr are restored.
// Main loop (exactly as generated): fa = fold(lhz [r3]), fb = fold(lhz [r4]); r3 += 2, r4 += 2;
//   if fa == 0 or fa != fb -> r3 = fa - fb (u64 arithmetic) and return, else loop. Addresses are the 32-bit parts.
// No memory is written, the FPU state is not touched; liveness.py: callers read only r3.
//
// Speed: while both 8-byte windows stay inside one 4 KB page and the 8 bytes are identical and contain no zero unit, four
// units are skipped at once (identical bytes fold identically); everything else runs the 16-bit loop.
#pragma once

#include "../me_hot_common.h"

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "n_82AC3790.h assumes a little-endian host"
#endif

namespace me::hot::n_82AC3790 {

inline constexpr Cmp kCmp = kCmpRet;

inline uint32_t Fold(uint32_t c) { return (c - 65u <= 25u) ? c + 32u : c; }

inline bool Native(PPCContext& ctx, uint8_t* base) {
  if (ctx.r3.u32 == 0 || ctx.r4.u32 == 0) return false;
  const uint8_t* pa = Raw(base, ctx.r3.u32);
  const uint8_t* pb = Raw(base, ctx.r4.u32);
  while (((reinterpret_cast<uintptr_t>(pa) | reinterpret_cast<uintptr_t>(pb)) & 0xFFF) <= 0xFF8) {
    uint64_t x, y;
    std::memcpy(&x, pa, 8);
    std::memcpy(&y, pb, 8);
    if (x != y || ((x - 0x0001000100010001ull) & ~x & 0x8000800080008000ull)) break;
    pa += 8;
    pb += 8;
  }
  for (;;) {
    uint16_t ua, ub;
    std::memcpy(&ua, pa, 2);
    std::memcpy(&ub, pb, 2);
    const uint32_t fa = Fold(__builtin_bswap16(ua));
    const uint32_t fb = Fold(__builtin_bswap16(ub));
    if (fa == 0 || fa != fb) {
      ctx.r3.s64 = int64_t(fa) - int64_t(fb);
      return true;
    }
    pa += 2;
    pb += 2;
  }
}

inline void Writes(const PPCContext&, const uint8_t*, me::hot::Writes&) {}

}  // namespace me::hot::n_82AC3790

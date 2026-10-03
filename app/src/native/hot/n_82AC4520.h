// sub_82AC4520 - CRT wcscmp: r3 = string a, r4 = string b (UTF-16, big-endian code units), returns r3 = -1 / 0 / 1 (64-bit,
// sign-extended) from the first code unit where the unsigned 16-bit values differ; 0 when both strings end together.
//
// PPC loop: ra = lhz [r3]; rb = lhz [r4]; d = ra - rb (both < 65536, so the s32 sign of the 64-bit difference is the plain
// sign); if d != 0 -> r3 = (d < 0) ? -1 : 1; else if rb == 0 -> r3 = 0; else advance both by 2. Addresses are the 32-bit parts.
// No memory is written, the FPU state is not touched. The original leaves r10/r11 as scratch (liveness.py: only r3 is read).
//
// Speed: while both 8-byte windows stay inside one 4 KB page (so a load cannot fault past the string end: the first byte
// of the window belongs to the string) four units are compared per step; the terminating window is decoded exactly with a
// "lowest zero lane" / "lowest differing lane" test, the (rare) page-edge tail falls back to the 16-bit loop.
#pragma once

#include "../me_hot_common.h"

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "n_82AC4520.h assumes a little-endian host"
#endif

namespace me::hot::n_82AC4520 {

inline constexpr Cmp kCmp = kCmpRet;

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint8_t* pa = Raw(base, ctx.r3.u32);
  const uint8_t* pb = Raw(base, ctx.r4.u32);
  for (;;) {
    if (((reinterpret_cast<uintptr_t>(pa) | reinterpret_cast<uintptr_t>(pb)) & 0xFFF) > 0xFF8) break;  // page-edge tail
    uint64_t x, y;
    std::memcpy(&x, pa, 8);
    std::memcpy(&y, pb, 8);
    const uint64_t z = (x - 0x0001000100010001ull) & ~x & 0x8000800080008000ull;  // a zero lane in a (lowest bit exact)
    const uint64_t df = x ^ y;
    if (df == 0) {
      if (z) {
        ctx.r3.s64 = 0;  // equal up to and including a terminating zero unit
        return;
      }
      pa += 8;
      pb += 8;
      continue;
    }
    const uint32_t id = uint32_t(__builtin_ctzll(df)) >> 4;  // first differing lane
    const uint32_t iz = z ? uint32_t(__builtin_ctzll(z)) >> 4 : 4;
    if (iz < id) {  // equal zero unit before the first difference
      ctx.r3.s64 = 0;
      return;
    }
    const uint16_t ua = __builtin_bswap16(uint16_t(x >> (id * 16)));
    const uint16_t ub = __builtin_bswap16(uint16_t(y >> (id * 16)));
    ctx.r3.s64 = ua < ub ? -1 : 1;
    return;
  }
  for (;;) {
    uint16_t ua, ub;
    std::memcpy(&ua, pa, 2);
    std::memcpy(&ub, pb, 2);
    ua = __builtin_bswap16(ua);
    ub = __builtin_bswap16(ub);
    if (ua != ub) {
      ctx.r3.s64 = ua < ub ? -1 : 1;
      return;
    }
    if (ub == 0) {
      ctx.r3.s64 = 0;
      return;
    }
    pa += 2;
    pb += 2;
  }
}

inline void Writes(const PPCContext&, const uint8_t*, me::hot::Writes&) {}

}  // namespace me::hot::n_82AC4520

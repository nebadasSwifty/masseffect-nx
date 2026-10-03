// sub_82AC4058 - CRT wcslen: r3 = pointer to a UTF-16 string (big-endian code units, any 2-byte-or-odd alignment),
// returns r3 = number of code units before the first 0x0000 unit.
//
// PPC: r11 = r3; do { r10 = lhz [r11]; r11 += 2 } while (r10 != 0); r3 = (srawi((r11 - r3) as s32, 1)) - 1, all in 64 bits.
// With k = len + 1 iterations r11 - r3 = 2k, so r3 = int64(int32(2k) >> 1) - 1 (= len for every string shorter than 2^30
// units; the formula is kept as in the original). The load address is the 32-bit part of the pointer (u32 wrap).
// No memory is written, the FPU state is not touched. The original leaves r10/r11 as scratch (liveness.py: only r3 is read).
//
// Speed: after a 16-bit head up to the next 8-byte boundary the string is scanned with aligned 64-bit loads (4 units per
// load; a load never leaves the 8-byte block of a byte that is part of the string, so it cannot fault) and the classic
// "has a zero 16-bit lane" test (exact for the lowest zero lane). Odd pointers use the plain 16-bit loop.
#pragma once

#include "../me_hot_common.h"

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "n_82AC4058.h assumes a little-endian host"
#endif

namespace me::hot::n_82AC4058 {

inline constexpr Cmp kCmp = kCmpRet;

// Number of 16-bit units before the first zero unit, starting at host pointer p.
inline uint64_t Units(const uint8_t* p) {
  const uint8_t* const p0 = p;
  uint16_t v;
  if (reinterpret_cast<uintptr_t>(p) & 1) {
    for (;;) {
      std::memcpy(&v, p, 2);
      if (v == 0) return uint64_t(p - p0) >> 1;
      p += 2;
    }
  }
  while (reinterpret_cast<uintptr_t>(p) & 7) {
    std::memcpy(&v, p, 2);
    if (v == 0) return uint64_t(p - p0) >> 1;
    p += 2;
  }
  for (;;) {
    uint64_t x;
    std::memcpy(&x, p, 8);
    const uint64_t z = (x - 0x0001000100010001ull) & ~x & 0x8000800080008000ull;
    if (z) return (uint64_t(p - p0) >> 1) + (uint64_t(__builtin_ctzll(z)) >> 4);
    p += 8;
  }
}

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint64_t k = Units(Raw(base, ctx.r3.u32)) + 1;  // loop iterations of the original
  const int32_t d = static_cast<int32_t>(static_cast<uint32_t>(k << 1));
  ctx.r3.s64 = static_cast<int64_t>(d >> 1) - 1;
}

inline void Writes(const PPCContext&, const uint8_t*, me::hot::Writes&) {}

}  // namespace me::hot::n_82AC4058

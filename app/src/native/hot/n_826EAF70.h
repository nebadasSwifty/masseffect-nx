// sub_826EAF70 - object hash (leaf, read only): CRC-style table lookup over 16 bytes, then a Jenkins lookup2 mix with 3 words.
//
//   r3 = object pointer o. Step 1: crc = -1; for i in 0..15: crc = T[(byte(o + 12 + i) ^ (crc >> 24)) * 4] ^ (crc << 8)
//   (T = 256-word table at guest address 0x82E97C98, read from guest memory like the original; the byte address is a 32-bit
//   wrapping sum). Step 2: a = ~crc, lookup2 (golden ratio 0x9E3779B9) mixing the big-endian words [o + 8], [o + 4], [o + 0]
//   (in that order, three mix() rounds of 9 steps); r3 = the final c-value.
//
// Exactness: the original does the mix in 64-bit PPC registers with subf/xor/rlwinm. ~crc has all-ones upper bits and the
// subtractions borrow into the upper half, so the full 64-bit r3 is NOT zero-extended; this transliteration keeps every
// step in uint64_t (rlwinm = 32-bit rotate masked to <= 32 bits, i.e. zero-extended) and so reproduces all 64 bits.
// The mix constant is the sign-extended 0xFFFFFFFF9E3779B9 (lis -25033 / ori 31161). No guest memory is written, no FPU use.
// Only r3 is returned: the other scratch the original leaves (r5-r8, r10) is not reproduced. liveness.py reports no DIRECT
// call sites of sub_826EAF70 (all callers use the __fast_sub_826EAF70 entry, which has an r3-only return), and the function
// is otherwise reached through ABI-conforming callers, so only r3 is compared. NOTE for the integrator: callers use
// __fast_sub_826EAF70(ctx, base, r3) directly; a hook on sub_826EAF70 alone does not intercept them.
#pragma once

#include "../me_hot_common.h"

namespace me::hot::n_826EAF70 {

inline constexpr Cmp kCmp = {R(3), 0, 0};

// rlwinm helper: rotate the low 32 bits left by s (1..31); the caller masks with a <= 32-bit mask.
inline uint64_t Rot(uint64_t x, int s) {
  const uint32_t v = uint32_t(x);
  return uint32_t((v << s) | (v >> (32 - s)));
}

inline uint64_t Hash(const uint8_t* base, uint32_t o) {
  uint32_t crc = 0xFFFFFFFFu;
  const uint32_t p = o + 12;
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t idx = (uint32_t(Ld8(base, p + i)) ^ (crc >> 24)) << 2;
    crc = Ld32(base, 0x82E97C98u + idx) ^ (crc << 8);
  }
  uint64_t r3, r5, r6, r7, r8, r9, r10, r11 = crc;
  r10 = ~r11;
  r9 = Ld32(base, o + 8);
  r11 = 0xFFFFFFFF9E370000ull;
  r7 = Ld32(base, o + 4);
  r9 = r9 - r10;
  r6 = Ld32(base, o + 0);
  r8 = Rot(r10, 19) & 0x7FFFF;
  r11 = r11 | 31161u;
  r9 = r9 ^ r8;
  r8 = r11 - r9;
  r5 = Rot(r9, 8) & 0xFFFFFF00;
  r8 = r8 - r10;
  r8 = r8 ^ r5;
  r10 = r10 - r8;
  r5 = Rot(r8, 19) & 0x7FFFF;
  r10 = r10 - r9;
  r9 = r9 - r8;
  r10 = r10 ^ r5;
  r9 = r9 - r10;
  r5 = Rot(r10, 20) & 0xFFFFF;
  r9 = r9 ^ r5;
  r8 = r8 - r9;
  r5 = Rot(r9, 16) & 0xFFFF0000;
  r8 = r8 - r10;
  r8 = r8 ^ r5;
  r10 = r10 - r8;
  r5 = Rot(r8, 27) & 0x7FFFFFF;
  r10 = r10 - r9;
  r9 = r9 - r8;
  r10 = r10 ^ r5;
  r9 = r9 - r10;
  r5 = Rot(r10, 29) & 0x1FFFFFFF;
  r9 = r9 ^ r5;
  r8 = r8 - r9;
  r5 = Rot(r9, 10) & 0xFFFFFC00;
  r8 = r8 - r10;
  r8 = r8 ^ r5;
  r10 = r10 - r8;
  r8 = Rot(r8, 17) & 0x1FFFF;
  r10 = r10 - r9;
  r10 = r10 ^ r8;
  r9 = r7 - r10;
  r8 = Rot(r10, 19) & 0x7FFFF;
  r9 = r9 ^ r8;
  r8 = r11 - r9;
  r7 = Rot(r9, 8) & 0xFFFFFF00;
  r8 = r8 - r10;
  r8 = r8 ^ r7;
  r10 = r10 - r8;
  r7 = Rot(r8, 19) & 0x7FFFF;
  r10 = r10 - r9;
  r9 = r9 - r8;
  r10 = r10 ^ r7;
  r9 = r9 - r10;
  r7 = Rot(r10, 20) & 0xFFFFF;
  r9 = r9 ^ r7;
  r8 = r8 - r9;
  r7 = Rot(r9, 16) & 0xFFFF0000;
  r8 = r8 - r10;
  r8 = r8 ^ r7;
  r10 = r10 - r8;
  r7 = Rot(r8, 27) & 0x7FFFFFF;
  r10 = r10 - r9;
  r9 = r9 - r8;
  r10 = r10 ^ r7;
  r9 = r9 - r10;
  r7 = Rot(r10, 29) & 0x1FFFFFFF;
  r9 = r9 ^ r7;
  r8 = r8 - r9;
  r7 = Rot(r9, 10) & 0xFFFFFC00;
  r8 = r8 - r10;
  r8 = r8 ^ r7;
  r10 = r10 - r8;
  r8 = Rot(r8, 17) & 0x1FFFF;
  r10 = r10 - r9;
  r10 = r10 ^ r8;
  r8 = Rot(r10, 19) & 0x7FFFF;
  r9 = r6 - r10;
  r9 = r9 ^ r8;
  r8 = Rot(r9, 8) & 0xFFFFFF00;
  r11 = r11 - r9;
  r11 = r11 - r10;
  r11 = r11 ^ r8;
  r10 = r10 - r11;
  r8 = Rot(r11, 19) & 0x7FFFF;
  r10 = r10 - r9;
  r9 = r9 - r11;
  r10 = r10 ^ r8;
  r9 = r9 - r10;
  r8 = Rot(r10, 20) & 0xFFFFF;
  r9 = r9 ^ r8;
  r11 = r11 - r9;
  r8 = Rot(r9, 16) & 0xFFFF0000;
  r11 = r11 - r10;
  r11 = r11 ^ r8;
  r10 = r10 - r11;
  r8 = Rot(r11, 27) & 0x7FFFFFF;
  r10 = r10 - r9;
  r9 = r9 - r11;
  r10 = r10 ^ r8;
  r8 = Rot(r10, 29) & 0x1FFFFFFF;
  r9 = r9 - r10;
  r9 = r9 ^ r8;
  r11 = r11 - r9;
  r8 = Rot(r9, 10) & 0xFFFFFC00;
  r11 = r11 - r10;
  r11 = r11 ^ r8;
  r10 = r10 - r11;
  r11 = Rot(r11, 17) & 0x1FFFF;
  r10 = r10 - r9;
  r3 = r10 ^ r11;
  return r3;
}

inline void Native(PPCContext& ctx, uint8_t* base) { ctx.r3.u64 = Hash(base, ctx.r3.u32); }

inline void Writes(const PPCContext&, const uint8_t*, me::hot::Writes&) {}

}  // namespace me::hot::n_826EAF70

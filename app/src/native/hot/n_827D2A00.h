// sub_827D2A00 - LZO1X decompressor (the non-safe lzo1x_decompress) used by the package streaming threads to inflate
// compressed package chunks. Same function, instruction for instruction, in the Russian build (only the address of its
// switch table differs, and the native version does not use that table), so this header serves both editions.
//
//   r3 = in, r4 = in_len, r5 = out, r6 = &out_len (u32). No output capacity: like the original, nothing is bounds-checked.
//   Returns r3 = 0 (input consumed exactly), 0x00000000FFFFFFF8 (stopped before the end of the input, -8 as a 32-bit value
//   in a zero-extended register: rlwinm leaves the upper half 0) or 0xFFFFFFFFFFFFFFFC (read past the end, -4).
//   *out_len = op - out is stored at the end (and 0 at the start, after the first input byte has been read).
//
// The original is a fully unrolled state machine: switch on (state + token), state 0x100 = loop top, 0x200 = after a
// literal run of >= 4 bytes, 0 = after 1-3 trailing literals. Its behaviour, read from the generated code:
//   * first byte > 17: copy (byte - 17) literals, state = (byte - 17 < 4) ? 0 : 0x200; else state 0x100.
//   * token >= 64 (M2): dist = 1 + ((t >> 2) & 7) + (b << 3), 1 distance byte, copy (t >> 5) + 1 bytes;
//     trailing literals = t & 3 (from the token).
//   * 32 <= token < 64 (M3): len = t & 31, 0 = extended (31 + 255 per zero byte + the first non-zero byte);
//     dist = 1 + (le16 >> 2), copy len + 2; trailing literals = in[ip - 2] & 3, READ AGAIN after the copy.
//   * 16 <= token < 32 (M4): len = t & 7, 0 = extended (7 + ...); dist = le16 >> 2. Only token 17 with dist == 0 is the
//     end marker (tokens 18-23 / 16 with dist 0 copy from op - 0x4000: the original has a single end check);
//     otherwise copy len + 2 bytes from op - dist - (t & 8 ? 0x8000 : 0x4000); trailing literals as M3.
//   * token < 16: state 0x100: literal run of t + 3 bytes (t == 0: 18 + 255 per zero byte + first non-zero byte),
//     state 0x200. State 0x200: 3-byte match, dist = 2049 + (t >> 2) + (b << 2). State 0: 2-byte match,
//     dist = 1 + (t >> 2) + (b << 2). Trailing literals = t & 3 (from the token).
//   * after a match: no trailing literals -> state 0x100; else copy them, state 0.
// Every copy is byte by byte in ascending order (lbz/stb pairs), so an overlapping match repeats its period; the native
// copy reproduces that (memmove when the source is above, chunks of 8 when the distance is >= 8, bytes otherwise).
// Every guest address is 32-bit and wraps like the original's (r10/r11 are 64-bit but every access uses the low half).
// Register state: r3 is the only result; r30/r31 are locals of the generated code (the original stores them, zero, at
// -16(r1) / -8(r1): reproduced); r4-r12 scratch. liveness.py: neither direct call site reads a volatile register.
// Profile (RU, Eden Prime cutscene + open area, 2026-10-08): 10.7 and 4.4 ms per frame of self time on two streaming
// threads, the largest single guest function of the process.
#pragma once

#include "../me_hot_common.h"

namespace me::hot::n_827D2A00 {

inline constexpr Cmp kCmp = kCmpRet;

namespace detail {

// dst[i] = src[i] for i = 0 .. n-1 in ascending order (n >= 1).
inline void CopyFwd(uint8_t* base, uint32_t dst, uint32_t src, uint32_t n) {
  const uint64_t d_end = uint64_t(dst) + n, s_end = uint64_t(src) + n;
  if (d_end <= 0x100000000ull && s_end <= 0x100000000ull && PhysOff(dst) == PhysOff(src) &&
      PhysOff(dst) == PhysOff(uint32_t(d_end - 1)) && PhysOff(src) == PhysOff(uint32_t(s_end - 1))) [[likely]] {
    uint8_t* d = Raw(base, dst);
    const uint8_t* s = Raw(base, src);
    if (n < 16) {
      for (uint32_t i = 0; i < n; ++i) d[i] = s[i];
      return;
    }
    if (dst <= src || dst - src >= n) {  // ascending byte copy == memmove here
      std::memmove(d, s, n);
      return;
    }
    const uint32_t dist = dst - src;
    uint32_t i = 0;
    if (dist >= 8) {  // every 8-byte chunk reads bytes that are already final
      for (; i + 8 <= n; i += 8) {
        uint64_t v;
        std::memcpy(&v, s + i, 8);
        std::memcpy(d + i, &v, 8);
      }
    }
    for (; i < n; ++i) d[i] = s[i];
    return;
  }
  for (uint32_t i = 0; i < n; ++i) St8(base, dst + i, Ld8(base, src + i));
}

// Extended length: zero bytes add 255 each, the first non-zero byte ends it. Returns 255 * zeros + last (32-bit wrap
// like the original's counter).
inline uint32_t ExtLen(const uint8_t* base, uint32_t& ip) {
  uint32_t n = 0;
  uint8_t b;
  while ((b = Ld8(base, ip++)) == 0) n += 255u;
  return n + b;
}

enum : uint32_t { kAfterTrail = 0, kTop = 0x100, kAfterLiterals = 0x200 };

// One decode. kWrite = false: measure only (no guest writes; returns false on an implausible stream). Fills ip/op.
template <bool kWrite>
inline bool Decode(uint8_t* base, uint32_t in, uint32_t out, uint32_t& ip_out, uint32_t& op_out, uint32_t in_len) {
  uint32_t ip = in, op = out;
  // Measure mode stops at a size the guard could not snapshot anyway (or a runaway stream).
  const uint32_t max_out = 1u << 20, max_in = in_len + 0x10000u;
  auto copy = [&](uint32_t dst, uint32_t src, uint32_t n) {
    if constexpr (kWrite) CopyFwd(base, dst, src, n);
  };
  uint32_t state;
  uint32_t t = Ld8(base, ip);
  if (t > 17) {
    ++ip;
    t -= 17;
    copy(op, ip, t);
    ip += t;
    op += t;
    state = t < 4 ? kAfterTrail : kAfterLiterals;
  } else {
    state = kTop;
  }
  for (;;) {
    if constexpr (!kWrite) {
      if (op - out > max_out || ip - in > max_in) return false;
    }
    t = Ld8(base, ip++);
    uint32_t trailing;
    if (t >= 64) {  // M2
      const uint32_t b = Ld8(base, ip);
      const uint32_t src = op - (b << 3) - ((t >> 2) & 7u) - 1u;
      ++ip;
      const uint32_t n = (t >> 5) + 1u;
      copy(op, src, n);
      op += n;
      trailing = t & 3u;
    } else if (t >= 32) {  // M3
      uint32_t n = t & 31u;
      if (n == 0) n = 31u + ExtLen(base, ip);
      const uint32_t d = (uint32_t(Ld8(base, ip + 1)) << 6) + (uint32_t(Ld8(base, ip)) >> 2);
      const uint32_t src = op - d - 1u;
      ip += 2;
      copy(op, src, n + 2u);
      op += n + 2u;
      trailing = Ld8(base, ip - 2) & 3u;  // re-read after the copy, as the original
    } else if (t >= 16) {  // M4
      uint32_t n = t & 7u;
      if (n == 0) n = 7u + ExtLen(base, ip);
      const uint32_t d = (uint32_t(Ld8(base, ip + 1)) << 6) + (uint32_t(Ld8(base, ip)) >> 2);
      ip += 2;
      if (t == 17 && d == 0) break;  // end marker
      const uint32_t src = op - d - ((t & 8u) ? 0x8000u : 0x4000u);
      copy(op, src, n + 2u);
      op += n + 2u;
      trailing = Ld8(base, ip - 2) & 3u;
    } else if (state == kTop) {  // literal run
      uint32_t n = t + 3u;
      if (t == 0) n = 18u + ExtLen(base, ip);
      copy(op, ip, n);
      ip += n;
      op += n;
      state = kAfterLiterals;
      continue;
    } else {  // short match after literals
      const uint32_t b = Ld8(base, ip);
      ++ip;
      if (state == kAfterLiterals) {
        copy(op, op - (b << 2) - (t >> 2) - 2049u, 3);
        op += 3;
      } else {
        copy(op, op - (b << 2) - (t >> 2) - 1u, 2);
        op += 2;
      }
      trailing = t & 3u;
    }
    if (trailing == 0) {
      state = kTop;
    } else {
      copy(op, ip, trailing);
      ip += trailing;
      op += trailing;
      state = kAfterTrail;
    }
  }
  ip_out = ip;
  op_out = op;
  return true;
}

}  // namespace detail

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t sp = ctx.r1.u32;
  St64(base, sp - 16, 0);  // std r30,-16(r1) / std r31,-8(r1): zeroed locals in the generated code
  St64(base, sp - 8, 0);
  const uint32_t in = ctx.r3.u32, out = ctx.r5.u32, out_len = ctx.r6.u32;
  const uint32_t ip_end = uint32_t(ctx.r3.u64 + ctx.r4.u64);
  (void)Ld8(base, in);  // the original reads the first byte before it clears *out_len
  St32(base, out_len, 0);
  uint32_t ip = 0, op = 0;
  detail::Decode<true>(base, in, out, ip, op, ctx.r4.u32);
  St32(base, out_len, op - out);
  if (ip == ip_end) {
    ctx.r3.u64 = 0;
  } else if (ip > ip_end) {
    ctx.r3.u64 = 0xFFFFFFFFFFFFFFFCull;
  } else {
    ctx.r3.u64 = 0x00000000FFFFFFF8ull;
  }
}

// The written ranges come from a measuring decode (control flow depends only on the input bytes). Not verifiable (the
// guard then runs the original and does not compare): output over 1 MB, a runaway stream, or an output range that
// overlaps the input it was decoded from (the copies would change later tokens).
inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) {
  const uint32_t in = ctx.r3.u32, out = ctx.r5.u32;
  uint32_t ip = 0, op = 0;
  if (!detail::Decode<false>(const_cast<uint8_t*>(base), in, out, ip, op, ctx.r4.u32)) {
    w.overflow = true;
    return;
  }
  const uint64_t in_lo = in, in_hi = uint64_t(in) + (ip - in), out_lo = out, out_hi = uint64_t(out) + (op - out);
  if (op - out != 0 && in_hi > in_lo && out_lo < in_hi && in_lo < out_hi) {
    w.overflow = true;
    return;
  }
  w.Add(out, op - out);
  w.Add(ctx.r6.u32, 4);
  w.Add(ctx.r1.u32 - 16, 16);
}

}  // namespace me::hot::n_827D2A00

// sub_82654030 - UE3 TArray<{u32 payload, float key}>::Sort: in-place quicksort of 8-byte elements by the float at +4,
// DESCENDING (largest key first), with an explicit work stack and a selection sort for ranges of <= 8 elements.
//
//   r3 = first element (any alignment), r4 = element count (signed 32 bit; < 2 => nothing happens, not even the FPU mode).
//   An element is two big-endian words {payload, key}; elements are only ever moved as 8 raw bytes.
//
// The original keeps its work stack (31 {lo, hi} pairs) and the zeroing of it (sub_82AC4A50 = memset of 248 bytes, called with r4 = 0)
// in its own stack frame below r1: callee-owned scratch, so the native version keeps the stack in a local array. The sort itself
// is reproduced step by step (the permutation of equal / NaN keys must match): the pivot is the first element after it was swapped
// with the middle one, the partition scans stop at key < pivot (ascending index) and key < pivot (descending index) with the
// ordered-compare semantics of fcmpu (a NaN key is never "less"),
// the smaller partition is processed first, the larger one is pushed (this order is unobservable in the result - the ranges are
// disjoint - but it bounds the stack depth).
//
// FPU: every recompiled `lfs` is preceded by ctx.fpscr.disableFlushMode(); every path with n >= 2 reads a key, so the native version
// clears the flush mode once on entry (idempotent). The key compare is a float compare: the original widens to double (exact) and
// compares as doubles, which is the same ordering for floats, with the flush mode cleared (denormals are not flushed).
// The one exception: when the address range lo + 8 * n wraps 2^32 (cannot happen for guest arrays), no compare may happen at all;
// that case uses a tracking instantiation (Sort<true>) that clears the flush mode just before the first key compare (fuzzed by
// forcing it for every input).
//
// Register state: void function. The original leaves garbage in r3-r11 / f0, f13 (no direct call site reads a volatile register,
// tests/hot_fuzz/liveness.py), so kCmp compares nothing but the always-compared set (r1, FPSCR, ...).
#pragma once

#include "../me_hot_common.h"

namespace me::hot::n_82654030 {

inline constexpr Cmp kCmp = {0, 0, 0};

namespace detail {

inline uint64_t Elem(const uint8_t* base, uint32_t a) {
  uint64_t v;
  std::memcpy(&v, Raw(base, a), 8);
  return v;
}
inline void SetElem(uint8_t* base, uint32_t a, uint64_t v) { std::memcpy(Raw(base, a), &v, 8); }
// the sort key of the element at a: the big-endian float at a + 4
inline float Key(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Raw(base, a + 4), 4);
  v = __builtin_bswap32(v);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
// swap two elements (a == b is a no-op)
inline void Swap(uint8_t* base, uint32_t a, uint32_t b) {
  const uint64_t t = Elem(base, a);
  SetElem(base, a, Elem(base, b));
  SetElem(base, b, t);
}

inline void Touch(PPCContext& ctx, bool& touched) {
  if (!touched) {
    touched = true;
    ctx.fpscr.disableFlushMode();
  }
}

// kTrack: clear the flush mode lazily, right before the first key is read (the range-wraps corner, where no key may be read at all);
// otherwise the caller has cleared it already.
template <bool kTrack>
inline void Sort(PPCContext& ctx, uint8_t* base, uint32_t lo, uint32_t hi) {
  uint32_t stk[2 * 40];  // pending {lo, hi} ranges; the original has 31 slots, the depth is <= log2(n)
  int sp = 0;
  stk[0] = lo;
  stk[1] = hi;
  bool touched = false;
  for (;;) {
    lo = stk[2 * sp];
    hi = stk[2 * sp + 1];
    for (;;) {
      // L194: size in elements (signed 32-bit arithmetic as the recompiled srawi / addi / cmpwi)
      const int32_t n1 = int32_t(uint32_t((int32_t)(hi - lo) >> 3) + 1u);
      if (n1 <= 8) {
        if (!(hi > lo)) break;  // -> pop
        // selection sort: repeatedly move the smallest key (first of equals) of [lo, hi] to hi
        for (;;) {
          uint32_t m = lo;
          float mk = Key(base, lo);
          if constexpr (kTrack) Touch(ctx, touched);
          for (uint32_t p = lo + 8; p <= hi; p += 8) {
            const float k = Key(base, p);
            if (k < mk) {
              m = p;
              mk = k;
            }
          }
          Swap(base, m, hi);
          hi -= 8;
          if (!(hi > lo)) break;
        }
        break;  // -> pop
      }
      // partition around the first element after swapping it with the middle one
      {
        const int32_t mid = n1 >> 1;
        Swap(base, lo + (uint32_t(mid) << 3), lo);
      }
      const float pk = Key(base, lo);  // the pivot element is not touched by the partition loops
      uint32_t i = lo;
      uint32_t j = hi + 8;
      for (;;) {
        for (;;) {  // scan up while key >= pivot (a NaN key does not stop the scan)
          i += 8;
          if (i > hi) break;
          if constexpr (kTrack) Touch(ctx, touched);
          if (Key(base, i) < pk) break;
        }
        for (;;) {  // scan down while key < pivot
          j -= 8;
          if (j <= lo) break;
          if constexpr (kTrack) Touch(ctx, touched);
          if (!(Key(base, j) < pk)) break;
        }
        if (i > j) break;
        Swap(base, i, j);
      }
      // put the pivot at j
      Swap(base, lo, j);
      const int32_t lsz = int32_t((j - lo - 8) & 0xFFFFFFF8u);
      const int32_t rsz = int32_t((hi - i) & 0xFFFFFFF8u);
      if (lsz >= rsz) {
        // push the left part, continue with the right one
        if (lo + 8 < j) {
          stk[2 * sp] = lo;
          stk[2 * sp + 1] = j - 8;
          ++sp;
        }
        if (!(hi > i)) break;  // -> pop
        lo = i;
        continue;
      }
      // push the right part, continue with the left one
      if (hi > i) {
        stk[2 * sp] = i;
        stk[2 * sp + 1] = hi;
        ++sp;
      }
      if (!(lo + 8 < j)) break;  // -> pop
      hi = j - 8;
    }
    if (--sp < 0) return;
  }
}

}  // namespace detail

inline void Native(PPCContext& ctx, uint8_t* base) {
  if (ctx.r4.s32 < 2) return;
  const uint32_t lo = ctx.r3.u32;
  const uint32_t n = ctx.r4.u32;
  const uint32_t hi = uint32_t(lo + (n << 3) - 8u);
  if (n < 0x20000000u && uint64_t(lo) + (uint64_t(n) << 3) <= 0x100000000ull) [[likely]] {
    ctx.fpscr.disableFlushMode();
    detail::Sort<false>(ctx, base, lo, hi);
  } else {
    detail::Sort<true>(ctx, base, lo, hi);
  }
}

inline void Writes(const PPCContext& ctx, const uint8_t*, me::hot::Writes& w) {
  if (ctx.r4.s32 < 2) return;
  const uint64_t bytes = uint64_t(ctx.r4.u32) << 3;
  if (bytes > me::hot::Writes::kMaxBytes || uint64_t(ctx.r3.u32) + bytes > 0x100000000ull) {
    w.overflow = true;
    return;
  }
  w.Add(ctx.r3.u32, uint32_t(bytes));
}

}  // namespace me::hot::n_82654030

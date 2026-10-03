// sub_8264ADA0 - UE3 skeletal mesh (GPU skinning): re-bind a skin cache object and copy the bone matrices of every section
// into its per-section "bone matrix" arrays, transposed from 4x4 (row, 64 bytes) to 3x4 (48 bytes).
//
//   r3 = this (obj), r4 = arg (new bound object).
//   1. prev = [obj+48]; if prev != 0: virtual call  [[prev+0]+0](prev, 1)  (scalar deleting destructor style; arbitrary guest
//      code, done through the dispatch table exactly like the generated REX_CALL_INDIRECT_FUNC, ctx.r1 = r1 - 176, ctx.r3 = prev,
//      ctx.r4 = 1, no lr change). Its return value is the function's r3 unless a later array grow replaces it.
//   2. [obj+48] = arg. lod = [arg+28]; r25 = [[[obj+12]+160] + lod*4] (LOD info), r21 = [[obj+20] + lod*4] (per-LOD cache).
//      For every section s < [r25+16] (s32): info = [r25+12] + 52*s ([+28] -> u16 bone index array, [+32] = s32 bone count,
//      [+48] = max influences), hdr = [[r21+236] + 4*s] + 248 = {u32 data, u32 count, u32 capacity, u32 maxInfl}:
//        count = 0; for i < bone count: AddUninitialized(hdr, 1 element of 48 bytes, align 8) (the original calls sub_82210298,
//        inlined here; the allocator sub_822100F8 is only called when count > capacity, with the same arguments) and then
//        copy bone [index[i]] of the 64-byte matrix array [[arg+4]] into data[i] with the transposition
//        dst[0..11] = src[0,16,32,48, 4,20,36,52, 8,24,40,56] (12 lfs/stfs pairs, every one re-loading all pointers).
//        [hdr+12] = min([info+48], 4) (s32).
//   3. [obj+32] = -1.
//
// Exactness notes:
//  * Every guest load is performed in the original's order after the stores that precede it (aliasing safe); the copy loop
//    uses a vector transposition (vld4q_u32) only when source and destination are provably disjoint and below 0xE0000000
//    (no wrap / physical offset), the scalar word-by-word copy in the original's order otherwise.
//  * lfs/stfs through f0 is a bit copy: the generated code's float->double->float round trip is folded by the compilers
//    (sNaN payloads are not quieted on either side); the flush mode is switched with ctx.fpscr.disableFlushMode() once per
//    bone, before the copy, exactly where the original does (not at all if no bone is processed).
//  * AddUninitialized: the original passes r4 = (i - count + 1) (64-bit sub, low word compared signed; i - count is 0 unless the
//    count word is clobbered), so the call (== inlined grow check) happens when s32(i - count + 1) > 0. new = count + add;
//    [hdr+4] = new; when s32(new) > s32(cap): newcap = round-toward-zero((new + 3) / 4) * 4, [hdr+8] = newcap, and if
//    data != 0 or newcap != 0: data = Realloc(data, 48 * s32(newcap), 8) -> [hdr+0]. (The callee's own frame is callee scratch.)
//  * r3 (the only candidate return register, kCmp): 0 if there was no previous object, otherwise the return value of the
//    virtual call; replaced by the old count (zero-extended) after every AddUninitialized call. The function has no direct call
//    site (vtable method), so no other volatile register is read by a caller. r1 and the non-volatile registers are restored.
//  * Writes(): obj+32..+52, every section header (16 bytes) and the element ranges of the data arrays (a dry run of the grow
//    logic over the initial memory; ranges are merged and capped at 8). If the virtual call or the allocator would run, the
//    write set is unbounded -> overflow (the self-check guard then runs the original). A data array overlapping a header
//    (feedback through the count word) is also reported as overflow, since it changes the control flow.
#pragma once

#include "../me_hot_common.h"
#include "../me_hot_call.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

extern "C" void sub_822100F8(PPCContext&, uint8_t*);  // allocator Realloc(ptr = r3, size = r4, align = r5) -> r3

namespace me::hot::n_8264ADA0 {

inline constexpr Cmp kCmp = {R(3), 0, 0};

// Fuzzer hooks: the real virtual call / allocator are arbitrary guest code (unbounded write set); the harness replaces
// them by stubs that write nothing, clears the flags and provides a model of the allocator's returned pointer.
inline bool virt_unbounded = true;
inline bool alloc_unbounded = true;
inline uint32_t (*alloc_model)(uint32_t ptr, uint32_t size) = nullptr;

inline uint32_t Round4(uint32_t n) {  // addi 3; srawi 2; addze; rlwinm 2
  const int32_t t = int32_t(n + 3u);
  return uint32_t((t >> 2) + int32_t((t < 0) & ((t & 3) != 0))) << 2;
}

// Slow path of sub_82210298 (capacity exceeded): grow the array of 48-byte elements.
__attribute__((noinline)) inline void Grow(PPCContext& ctx, uint8_t* base, uint32_t arr, uint32_t nw, uint32_t sp) {
  const uint32_t nc = Round4(nw);
  const uint32_t data = Ld32(base, arr);
  St32(base, arr + 8, nc);
  if (data != 0 || nc != 0) {
    ctx.r3.u64 = data;
    ctx.r4.s64 = int64_t(48) * int32_t(Ld32(base, arr + 8));  // mullw
    ctx.r5.u64 = 8;
    ctx.r6.u64 = 8;
    ctx.r1.u32 = sp - 288u;  // the callee's frame (stwu r1,-112)
    sub_822100F8(ctx, base);
    ctx.r1.u32 = sp - 176u;
    St32(base, arr, ctx.r3.u32);
  }
}

// 12 word copies dst[4k] = src[kOff[k]], load/store interleaved in the original's order.
inline void Copy12(uint8_t* base, uint32_t src, uint32_t dst) {
#if defined(__aarch64__)
  const uint32_t d = dst - src;
  if (src < 0xDFFF0000u && dst < 0xDFFF0000u && d >= 64u && d <= uint32_t(0u - 48u)) {
    // Disjoint (source 64 bytes, destination 48 bytes): columns 0..2 of the 4x4 word matrix, raw words (no byte swap needed).
    const uint32x4x4_t m = vld4q_u32(reinterpret_cast<const uint32_t*>(Raw(base, src)));
    uint32_t* o = reinterpret_cast<uint32_t*>(Raw(base, dst));
    vst1q_u32(o, m.val[0]);
    vst1q_u32(o + 4, m.val[1]);
    vst1q_u32(o + 8, m.val[2]);
    return;
  }
#endif
  static constexpr uint8_t kOff[12] = {0, 16, 32, 48, 4, 20, 36, 52, 8, 24, 40, 56};
  for (uint32_t k = 0; k < 12; ++k) {
    uint32_t v;
    std::memcpy(&v, Raw(base, src + kOff[k]), 4);
    std::memcpy(Raw(base, dst + 4 * k), &v, 4);
  }
}

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t obj = ctx.r3.u32, arg = ctx.r4.u32;
  const uint32_t sp = ctx.r1.u32;
  const uint32_t ea = sp - 176u;
  St32(base, ea, sp);  // stwu r1,-176(r1): back chain
  ctx.r1.u32 = ea;

  uint64_t r3 = Ld32(base, obj + 48);
  if (uint32_t(r3) != 0) {
    const uint32_t fn = Ld32(base, Ld32(base, uint32_t(r3)));
    ctx.r3.u64 = r3;
    ctx.r4.s64 = 1;
    CallIndirect(ctx, base, fn);
    r3 = ctx.r3.u64;
  }

  const uint32_t r11 = Ld32(base, obj + 12);
  St32(base, obj + 48, arg);
  const uint32_t lod4 = Ld32(base, arg + 28) << 2;
  const uint32_t r10 = Ld32(base, obj + 20);
  const uint32_t r8 = Ld32(base, r11 + 160);
  const uint32_t r25 = Ld32(base, r8 + lod4);
  const uint32_t r21 = Ld32(base, r10 + lod4);
  if (int32_t(Ld32(base, r25 + 16)) > 0) {
    uint32_t r23 = 0, r24 = 0;  // 4 * s, 52 * s
    int32_t r22 = 0;
    do {
      const uint32_t r30 = r24 + Ld32(base, r25 + 12);
      const uint32_t r29 = Ld32(base, r23 + Ld32(base, r21 + 236)) + 248u;
      St32(base, r29 + 4, 0);
      if (int32_t(Ld32(base, r30 + 32)) > 0) {
        uint32_t r27 = 0, r28 = 0, r31 = 0;  // 2 * i, 48 * i, i
        do {
          const uint32_t cnt = Ld32(base, r29 + 4);
          const uint32_t add = r31 - cnt + 1u;
          if (int32_t(add) > 0) {
            // sub_82210298(r29, add, 48, 8)
            const uint32_t cap = Ld32(base, r29 + 8);
            const uint32_t nw = cnt + add;
            St32(base, r29 + 4, nw);
            if (int32_t(nw) > int32_t(cap)) Grow(ctx, base, r29, nw, sp);
            r3 = cnt;
          }
          const uint32_t idxp = Ld32(base, r30 + 28);
          const uint32_t r9 = Ld32(base, obj + 48);
          const uint32_t dst = r28 + Ld32(base, r29 + 0);
          const uint32_t idx = Ld16(base, r27 + idxp);
          const uint32_t src = (idx << 6) + Ld32(base, r9 + 4);
          ++r31;
          r28 += 48;
          r27 += 2;
          ctx.fpscr.disableFlushMode();
          Copy12(base, src, dst);
        } while (int32_t(r31) < int32_t(Ld32(base, r30 + 32)));
      }
      int32_t m = int32_t(Ld32(base, r30 + 48));
      if (m > 4) m = 4;
      St32(base, r29 + 12, uint32_t(m));
      ++r22;
      r24 += 52;
      r23 += 4;
    } while (r22 < int32_t(Ld32(base, r25 + 16)));
  }
  St32(base, obj + 32, 0xFFFFFFFFu);
  ctx.r1.s64 = ctx.r1.s64 + 176;
  ctx.r3.u64 = r3;
}

// Merge a small set of ranges into the guard's write set (at most 8 regions): sort, join overlapping/adjacent, then merge the
// closest neighbours (conservative superset) until it fits.
struct RangeSet {
  static constexpr int kCap = 96;
  uint32_t lo[kCap];
  uint64_t hi[kCap];  // exclusive
  int n = 0;
  bool bad = false;
  void Add(uint32_t a, uint64_t len) {
    if (!len) return;
    if (n >= kCap || uint64_t(a) + len > 0x100000000ull) {
      bad = true;
      return;
    }
    lo[n] = a;
    hi[n] = uint64_t(a) + len;
    ++n;
  }
  bool Overlaps(uint32_t a, uint64_t len) const {
    for (int i = 0; i < n; ++i)
      if (a < hi[i] && lo[i] < uint64_t(a) + len) return true;
    return false;
  }
  void Emit(me::hot::Writes& w) {
    if (bad) {
      w.overflow = true;
      return;
    }
    for (int i = 1; i < n; ++i) {  // insertion sort by lo
      const uint32_t l = lo[i];
      const uint64_t h = hi[i];
      int j = i - 1;
      while (j >= 0 && lo[j] > l) {
        lo[j + 1] = lo[j];
        hi[j + 1] = hi[j];
        --j;
      }
      lo[j + 1] = l;
      hi[j + 1] = h;
    }
    int m = 0;
    for (int i = 0; i < n; ++i) {
      if (m > 0 && lo[i] <= hi[m - 1]) {
        if (hi[i] > hi[m - 1]) hi[m - 1] = hi[i];
      } else {
        lo[m] = lo[i];
        hi[m] = hi[i];
        ++m;
      }
    }
    while (m > Writes::kMax) {
      int best = 0;
      uint64_t bg = ~0ull;
      for (int i = 0; i + 1 < m; ++i) {
        const uint64_t g = lo[i + 1] - hi[i];
        if (g < bg) {
          bg = g;
          best = i;
        }
      }
      hi[best] = hi[best + 1];
      for (int i = best + 1; i + 1 < m; ++i) {
        lo[i] = lo[i + 1];
        hi[i] = hi[i + 1];
      }
      --m;
    }
    for (int i = 0; i < m; ++i) {
      if (hi[i] - lo[i] > Writes::kMaxBytes) {
        w.overflow = true;
        return;
      }
      w.Add(lo[i], uint32_t(hi[i] - lo[i]));
    }
  }
};

inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) {
  const uint32_t obj = ctx.r3.u32, arg = ctx.r4.u32;
  if (Ld32(base, obj + 48) != 0 && virt_unbounded) {
    w.overflow = true;
    return;
  }
  RangeSet fixed, data;
  fixed.Add(obj + 32, 4);
  fixed.Add(obj + 48, 4);
  const uint32_t r11 = Ld32(base, obj + 12);
  const uint32_t lod4 = Ld32(base, arg + 28) << 2;
  const uint32_t r10 = Ld32(base, obj + 20);
  const uint32_t r8 = Ld32(base, r11 + 160);
  const uint32_t r25 = Ld32(base, r8 + lod4);
  const uint32_t r21 = Ld32(base, r10 + lod4);
  const int32_t ns = int32_t(Ld32(base, r25 + 16));
  if (ns > 64) {
    w.overflow = true;
    return;
  }
  for (int32_t s = 0; s < ns; ++s) {
    const uint32_t r30 = uint32_t(52 * s) + Ld32(base, r25 + 12);
    const uint32_t r29 = Ld32(base, uint32_t(4 * s) + Ld32(base, r21 + 236)) + 248u;
    fixed.Add(r29, 16);
    const int32_t n = int32_t(Ld32(base, r30 + 32));
    if (n <= 0) continue;
    if (n > 4096) {
      w.overflow = true;
      return;
    }
    uint32_t dptr = Ld32(base, r29 + 0), cap = Ld32(base, r29 + 8);
    int32_t i0 = 0;
    for (int32_t i = 0; i < n; ++i) {
      const uint32_t nw = uint32_t(i) + 1u;
      if (int32_t(nw) > int32_t(cap)) {
        const uint32_t nc = Round4(nw);
        cap = nc;
        if (dptr != 0 || nc != 0) {
          if (alloc_unbounded || !alloc_model) {
            w.overflow = true;
            return;
          }
          data.Add(dptr + uint32_t(48 * i0), uint64_t(48) * uint64_t(i - i0));
          dptr = alloc_model(dptr, uint32_t(int64_t(48) * int32_t(nc)));
          i0 = i;
        }
      }
    }
    data.Add(dptr + uint32_t(48 * i0), uint64_t(48) * uint64_t(n - i0));
  }
  for (int i = 0; i < data.n; ++i)
    if (fixed.Overlaps(data.lo[i], data.hi[i] - data.lo[i])) {  // header / obj fields clobbered by element writes: control flow feedback
      w.overflow = true;
      return;
    }
  RangeSet all = fixed;
  for (int i = 0; i < data.n; ++i) all.Add(data.lo[i], data.hi[i] - data.lo[i]);
  all.bad |= data.bad;
  all.Emit(w);
}

}  // namespace me::hot::n_8264ADA0

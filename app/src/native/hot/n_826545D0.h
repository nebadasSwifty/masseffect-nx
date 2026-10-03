// sub_826545D0 - UE3 skeletal mesh: build the per-bone "bone to component" matrices (local bone matrix * reference/skin
// matrix) into an output array.
//
//   r3 = output array {u32 data, u32 count, u32 capacity} (a TArray of 64-byte matrices), r4 = mesh component,
//   r5 = LOD index. mesh = [r4 + 640] (r30), remap = [r4 + 720] (r31, may be null), list = [[[mesh + 160] + r5 * 4] ...] (r27).
//
// Steps of the original:
//   1. sub_82210368(arr, 64, 8, n = [mesh+176]) = "Empty + reserve exactly n 64-byte elements"   (inlined here)
//      sub_82210298(arr, n, 64, 8)              = "AddUninitialized(n)" (count += n, grow if count > capacity)  (inlined)
//      Both only reach the allocator (sub_822100F8, a virtual Realloc through a global object) when the capacity differs
//      from n (first call) or the count exceeds the capacity (second call); that path is called as the original does.
//   2. For every i < [list+28] (list+24 = u16 bone index array): idx = u16[i], then
//        remap path (remap != 0 and [obj+728] == [mesh+128]):  m = [[obj+724] + idx*4];
//          valid iff idx < [mesh+176] && 0 <= m < [remap+688]:  out[idx] = mesh_mats[idx] * [remap+684][m]
//        direct path:                                           valid iff idx < [mesh+176] && idx < [obj+688]:
//                                                               out[idx] = [mesh+172][idx] * [obj+684][idx]
//        invalid: out[idx] = the 64-byte constant block at 0x82E64F30 (identity matrix, read from guest memory).
//      out = [arr] + idx * 64; every index is checked as the original does (all loads in the original order, reloaded
//      every iteration so aliasing between the output and the tables behaves identically).
//   The product (VMX, row vectors, guest memory big-endian): out[i][j] = dot(B[i][0..3], A[0..3][j]) where B = the
//   mesh matrix and A = the other one, evaluated with the host semantics of vmsum4fp128 = simde_mm_dp_ps(a, b, 0xFF)
//   (aarch64: vmulq_f32 + vaddvq_f32): the products in the lane order of the VMX registers (element 3 first), summed
//   ((p3 + p2) + (p1 + p0)) in terms of the matrix elements k. The same arithmetic is done 4 columns at a time with
//   vmulq_f32 on the reversed B rows and vpaddq_f32 (the same FADDP instruction, same pairing). The flush mode is
//   enabled (enableFlushMode) before the first product, as the original, and only if a product is computed.
//   NaN note: when two NaNs with different payloads/signs meet in one multiply or add (e.g. a quiet NaN input and the
//   default NaN of inf * 0) ARM returns the first operand's NaN, i.e. the result depends on the operand order the compiler
//   picked for that instruction; GCC picks different orders for different rows of the original itself (Switch ELF), so
//   the payload of such a NaN is not defined by the source. Which results are NaN, and every non-NaN bit, are exact.
//
// Not reproduced: the 64-byte stack temporaries and the callee stack frames (stack scratch), all volatile scratch
// registers (liveness.py: the two direct callers read none, the function returns void). The register r1 and the
// non-volatile registers are untouched. Residual difference (impossible in practice): a table or matrix pointer that
// points into the callee-owned stack frame of the original, or a 64-byte block that wraps the 4 GB guest address space.
#pragma once

#include "../me_hot_common.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

extern "C" void sub_822100F8(PPCContext&, uint8_t*);  // allocator Realloc(ptr = r3, size = r4, align = r5) -> r3

namespace me::hot::n_826545D0 {

inline constexpr Cmp kCmp = {0, 0, 0};  // returns nothing, no volatile register is read by a caller

inline constexpr uint32_t kIdent = uint32_t(-2098855936 + 20272);  // 64 bytes: identity matrix constant (guest memory)

// Writes(): the allocator is arbitrary guest code, so a call to it makes the write set unbounded (overflow: the guard
// then runs the original). The fuzzer replaces the allocator by a stub that writes nothing and returns alloc_hint for
// every call, and clears alloc_unbounded.
inline bool alloc_unbounded = true;
inline uint32_t alloc_hint = 0;

inline uint32_t Realloc(PPCContext& ctx, uint8_t* base, uint32_t ptr, uint32_t elems) {
  ctx.r3.u64 = ptr;
  ctx.r4.s64 = int64_t(64) * int32_t(elems);  // mullw (full 64-bit product of two sign-extended words)
  ctx.r5.s64 = 8;
  sub_822100F8(ctx, base);
  return ctx.r3.u32;
}

// Forward 8-byte copy of 64 bytes, as the original's ld/std loop (overlap of a destination right above the source
// would propagate; no hazard otherwise, then 4 x 16 bytes).
inline void Copy64(uint8_t* base, uint32_t dst, uint32_t src) {
  if ((dst - src) - 1u < 63u) {
    for (uint32_t k = 0; k < 64; k += 8) St64(base, dst + k, Ld64(base, src + k));
    return;
  }
#if defined(__aarch64__)
  const uint8_t* s = Raw(base, src);
  uint8_t* d = Raw(base, dst);
  const uint8x16_t q0 = vld1q_u8(s), q1 = vld1q_u8(s + 16), q2 = vld1q_u8(s + 32), q3 = vld1q_u8(s + 48);
  vst1q_u8(d, q0);
  vst1q_u8(d + 16, q1);
  vst1q_u8(d + 32, q2);
  vst1q_u8(d + 48, q3);
#else
  uint8_t t[64];
  std::memcpy(t, Raw(base, src), 64);
  std::memcpy(Raw(base, dst), t, 64);
#endif
}

// out = B * A as described above. b, a, out: host pointers to 64 bytes of big-endian floats; all inputs are read
// before the first output byte is written.
inline void MulMatrices(const uint8_t* b, const uint8_t* a, uint8_t* out) {
#if defined(__aarch64__)
  static constexpr uint8_t kRev[16] = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
  const uint8x16_t rev = vld1q_u8(kRev);
  // B rows in the VMX register lane order [e3 e2 e1 e0] (full 16-byte reversal of the big-endian bytes).
  const float32x4_t b0 = vreinterpretq_f32_u8(vqtbl1q_u8(vld1q_u8(b + 0), rev));
  const float32x4_t b1 = vreinterpretq_f32_u8(vqtbl1q_u8(vld1q_u8(b + 16), rev));
  const float32x4_t b2 = vreinterpretq_f32_u8(vqtbl1q_u8(vld1q_u8(b + 32), rev));
  const float32x4_t b3 = vreinterpretq_f32_u8(vqtbl1q_u8(vld1q_u8(b + 48), rev));
  // A rows with the words swapped to host order, transposed with the rows in the order 3,2,1,0 so that every column
  // comes out as [a3j a2j a1j a0j].
  const uint32x4_t r0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(a + 0)));
  const uint32x4_t r1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(a + 16)));
  const uint32x4_t r2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(a + 32)));
  const uint32x4_t r3 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(a + 48)));
  const uint32x4_t t0 = vzip1q_u32(r3, r2), t1 = vzip1q_u32(r1, r0), t2 = vzip2q_u32(r3, r2), t3 = vzip2q_u32(r1, r0);
  const float32x4_t c0 = vreinterpretq_f32_u64(vzip1q_u64(vreinterpretq_u64_u32(t0), vreinterpretq_u64_u32(t1)));
  const float32x4_t c1 = vreinterpretq_f32_u64(vzip2q_u64(vreinterpretq_u64_u32(t0), vreinterpretq_u64_u32(t1)));
  const float32x4_t c2 = vreinterpretq_f32_u64(vzip1q_u64(vreinterpretq_u64_u32(t2), vreinterpretq_u64_u32(t3)));
  const float32x4_t c3 = vreinterpretq_f32_u64(vzip2q_u64(vreinterpretq_u64_u32(t2), vreinterpretq_u64_u32(t3)));
#define ME_ROW(bi, off)                                                                                           \
  {                                                                                                              \
    const float32x4_t s01 = vpaddq_f32(vmulq_f32(bi, c0), vmulq_f32(bi, c1));                                    \
    const float32x4_t s23 = vpaddq_f32(vmulq_f32(bi, c2), vmulq_f32(bi, c3));                                    \
    vst1q_u8(out + off, vrev32q_u8(vreinterpretq_u8_f32(vpaddq_f32(s01, s23))));                                 \
  }
  ME_ROW(b0, 0)
  ME_ROW(b1, 16)
  ME_ROW(b2, 32)
  ME_ROW(b3, 48)
#undef ME_ROW
#else
  auto ld = [](const uint8_t* p, int i) {
    uint32_t v;
    std::memcpy(&v, p + 4 * i, 4);
    v = __builtin_bswap32(v);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  };
  float d[16];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float p[4];  // lane order of the VMX register: element 3 first
      for (int k = 0; k < 4; ++k) p[k] = ld(b, i * 4 + 3 - k) * ld(a, (3 - k) * 4 + j);
      d[i * 4 + j] = (p[0] + p[1]) + (p[2] + p[3]);
    }
  for (int i = 0; i < 16; ++i) {
    uint32_t v;
    std::memcpy(&v, &d[i], 4);
    v = __builtin_bswap32(v);
    std::memcpy(out + 4 * i, &v, 4);
  }
#endif
}

template <bool kRemap>
inline void Bones(PPCContext& ctx, uint8_t* base, uint32_t arr, uint32_t obj, uint32_t mesh, uint32_t remap,
                  uint32_t list_obj) {
  uint32_t off = 0;
  int32_t i = 0;
  bool flush = false;
  do {
    const uint32_t list = Ld32(base, list_obj + 24);
    const uint32_t mapbase = kRemap ? Ld32(base, obj + 724) : 0;
    const uint32_t idx = Ld16(base, off + list);
    const uint32_t m = kRemap ? Ld32(base, (idx << 2) + mapbase) : 0;
    uint32_t a_ptr = 0;
    bool ok = int32_t(idx) < int32_t(Ld32(base, mesh + 176));
    if (kRemap) {
      ok = ok && int32_t(m) >= 0 && int32_t(m) < int32_t(Ld32(base, remap + 688));
      if (ok) a_ptr = Ld32(base, remap + 684) + (m << 6);
    } else {
      ok = ok && int32_t(idx) < int32_t(Ld32(base, obj + 688));
      if (ok) a_ptr = Ld32(base, obj + 684) + (idx << 6);
    }
    if (ok) {
      const uint32_t b_ptr = Ld32(base, mesh + 172) + (idx << 6);
      if (!flush) {
        ctx.fpscr.enableFlushMode();
        flush = true;
      }
      uint8_t* dst = Raw(base, Ld32(base, arr) + (idx << 6));
      // Inputs are fully read inside MulMatrices before the first store (the original copies them to the stack first).
      MulMatrices(Raw(base, b_ptr), Raw(base, a_ptr), dst);
    } else {
      Copy64(base, Ld32(base, arr) + (idx << 6), kIdent);
    }
    ++i;
    off += 2;
  } while (i < int32_t(Ld32(base, list_obj + 28)));
}

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t arr = ctx.r3.u32, obj = ctx.r4.u32;
  const uint32_t mesh = Ld32(base, obj + 640);
  const uint32_t remap = Ld32(base, obj + 720);
  const uint32_t tab = Ld32(base, mesh + 160);
  const uint32_t cnt = Ld32(base, mesh + 176);
  const uint32_t list_obj = Ld32(base, tab + (ctx.r5.u32 << 2));

  // sub_82210368(arr, 64, 8, cnt)
  St32(base, arr + 4, 0);
  if (Ld32(base, arr + 8) != cnt) {
    const uint32_t data = Ld32(base, arr);
    St32(base, arr + 8, cnt);
    if (data != 0 || cnt != 0) St32(base, arr, Realloc(ctx, base, data, cnt));
  }
  // sub_82210298(arr, cnt', 64, 8)
  {
    const uint32_t cnt2 = Ld32(base, mesh + 176);
    const uint32_t old = Ld32(base, arr + 4);
    const uint32_t cap = Ld32(base, arr + 8);
    const uint32_t n = old + cnt2;
    St32(base, arr + 4, n);
    if (int32_t(n) > int32_t(cap)) {
      const int32_t t = int32_t(n + 3u);  // addi 3; srawi 2; addze (round toward zero); rlwinm 2
      const uint32_t nc = uint32_t((t >> 2) + int32_t((t < 0) & ((t & 3) != 0))) << 2;
      const uint32_t data = Ld32(base, arr);
      St32(base, arr + 8, nc);
      if (data != 0 || nc != 0) St32(base, arr, Realloc(ctx, base, data, Ld32(base, arr + 8)));
    }
  }

  const bool use_remap = remap != 0 && Ld32(base, obj + 728) == Ld32(base, mesh + 128);
  if (int32_t(Ld32(base, list_obj + 28)) <= 0) return;
  if (use_remap)
    Bones<true>(ctx, base, arr, obj, mesh, remap, list_obj);
  else
    Bones<false>(ctx, base, arr, obj, mesh, remap, list_obj);
}

inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) {
  const uint32_t arr = ctx.r3.u32, obj = ctx.r4.u32;
  const uint32_t mesh = Ld32(base, obj + 640);
  const uint32_t tab = Ld32(base, mesh + 160);
  const uint32_t cnt = Ld32(base, mesh + 176);
  const uint32_t list_obj = Ld32(base, tab + (ctx.r5.u32 << 2));
  w.Add(arr, 12);
  // dry run of the two array helpers: does the allocator get called (and where does the data pointer end up)?
  uint32_t data = Ld32(base, arr), cap = Ld32(base, arr + 8);
  bool called = false;
  if (cap != cnt) {
    cap = cnt;
    if (data != 0 || cnt != 0) {
      called = true;
      data = alloc_hint;
    }
  }
  if (int32_t(cnt) > int32_t(cap)) {  // count is 0 after the first helper, so the new count is cnt
    const int32_t t = int32_t(cnt + 3u);
    cap = uint32_t((t >> 2) + int32_t((t < 0) & ((t & 3) != 0))) << 2;
    if (data != 0 || cap != 0) {
      called = true;
      data = alloc_hint;
    }
  }
  if (called && alloc_unbounded) {
    w.overflow = true;
    return;
  }
  const int32_t n = int32_t(Ld32(base, list_obj + 28));
  if (n <= 0) return;
  if (n > (1 << 16)) {
    w.overflow = true;
    return;
  }
  const uint32_t list = Ld32(base, list_obj + 24);
  uint32_t mx = 0;
  for (int32_t k = 0; k < n; ++k) {
    const uint32_t idx = Ld16(base, list + 2u * uint32_t(k));
    if (idx > mx) mx = idx;
  }
  const uint64_t bytes = (uint64_t(mx) + 1) * 64;
  if (uint64_t(data) + bytes > 0xFFFFFFFFull) {
    w.overflow = true;
    return;
  }
  w.Add(data, uint32_t(bytes));
}

}  // namespace me::hot::n_826545D0

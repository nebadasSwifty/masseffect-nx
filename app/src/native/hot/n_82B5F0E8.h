// sub_82B5F0E8 - bounding-volume overlap test: does the box q (r4) fit the volume of object r3 and miss every entry?
//
//   r3 = this: [+0] -> object with two virtual getters (vtable slots +20 and +28 take r3 = that object and return an s32),
//        [+20] entries (16 bytes: s32 e0, e1, e2, e3), [+24] entry count (u32);
//   r4 = q: s32 q0 [+0], q1 [+4], q2 [+8], q3 [+12].
//   1. if q1 > getter20(obj) or q3 > getter28(obj) -> return 0       (both getters are guest calls: unchanged, in order)
//   2. for each entry in order: it overlaps when  q0 < e1 && q1 > e0 && q2 < e3 && q3 > e2   (all signed 32-bit)
//      -> return 0 at the first overlap; no overlap (or no entries) -> return 1.
// The two virtual calls are made exactly as the original does (REX_CALL_INDIRECT_FUNC: me_hot_call.h), with the same frame
// (r1 - 112, back chain stored) and r3 the callee sees. The current generated code (codegen options nal + diet) no longer
// stores ctx.lr / r11 / r12 before the calls (locals / elided lr stores), so neither does this version; r4 and r5..r10 are
// the caller's, untouched, as in the original. The loop only reads guest memory (no stores), so it may be evaluated in any
// order / width: four entries per step with NEON (deinterleaving load, byte swap, four compares) when the entry array stays
// below 0xE0000000 (no address wraparound / physical-range offset inside one 16-byte load), scalar otherwise and for the tail.
// Return: r3 (compared). r1 restored; non-volatile registers are locals; the other volatile registers are dead scratch
// (liveness.py: the 2 direct call sites read only r3). No FPU use. The stack word the original stores at r1 - 112 (back
// chain) is written too (callee-owned scratch). Writes(): none beyond that (the getters are assumed read-only: the guard
// calls them once for the native run and once for the original).
#pragma once

#include "../me_hot_common.h"
#include "../me_hot_call.h"

#if defined(__aarch64__) && !defined(ME_HOT_NO_NEON)
#include <arm_neon.h>
#define ME_HOT_82B5F0E8_NEON 1
#endif

namespace me::hot::n_82B5F0E8 {

inline constexpr Cmp kCmp = kCmpRet;

// First entry overlapping the box (p0..p3 = q0, q1, q2, q3), scalar.
inline bool Overlaps(const uint8_t* base, uint32_t e, int32_t p0, int32_t p1, int32_t p2, int32_t p3) {
  return p0 < int32_t(Ld32(base, e + 4)) && p1 > int32_t(Ld32(base, e + 0)) && p2 < int32_t(Ld32(base, e + 12)) &&
         p3 > int32_t(Ld32(base, e + 8));
}

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t q = ctx.r4.u32;
  const uint32_t sp = ctx.r1.u32;
  const uint32_t frame = sp - 112u;
  St32(base, frame, sp);  // stwu r1,-112(r1): back chain
  ctx.r1.u32 = frame;

  uint32_t obj = Ld32(base, self + 0);
  ctx.r3.u64 = obj;
  uint32_t fn = Ld32(base, Ld32(base, obj + 0) + 20);
  CallIndirect(ctx, base, fn);
  if (int32_t(Ld32(base, q + 4)) > ctx.r3.s32) {
    ctx.r3.u64 = 0;
    ctx.r1.u32 = sp;
    return;
  }
  obj = Ld32(base, self + 0);
  ctx.r3.u64 = obj;
  const int32_t p3 = int32_t(Ld32(base, q + 12));
  fn = Ld32(base, Ld32(base, obj + 0) + 28);
  CallIndirect(ctx, base, fn);
  ctx.r1.u32 = sp;
  if (p3 > ctx.r3.s32) {
    ctx.r3.u64 = 0;
    return;
  }

  uint32_t n = Ld32(base, self + 24);
  uint32_t e = Ld32(base, self + 20);
  const int32_t p0 = int32_t(Ld32(base, q + 0));
  const int32_t p1 = int32_t(Ld32(base, q + 4));
  const int32_t p2 = int32_t(Ld32(base, q + 8));
#if defined(ME_HOT_82B5F0E8_NEON)
  if (n >= 4 && uint64_t(e) + 16ull * n <= 0xE0000000ull) {
    const int32x4_t v0 = vdupq_n_s32(p0), v1 = vdupq_n_s32(p1), v2 = vdupq_n_s32(p2), v3 = vdupq_n_s32(p3);
    const uint8_t* ptr = Raw(base, e);
    do {
      const uint32x4x4_t d = vld4q_u32(reinterpret_cast<const uint32_t*>(ptr));
      const int32x4_t e0 = vreinterpretq_s32_u8(vrev32q_u8(vreinterpretq_u8_u32(d.val[0])));
      const int32x4_t e1 = vreinterpretq_s32_u8(vrev32q_u8(vreinterpretq_u8_u32(d.val[1])));
      const int32x4_t e2 = vreinterpretq_s32_u8(vrev32q_u8(vreinterpretq_u8_u32(d.val[2])));
      const int32x4_t e3 = vreinterpretq_s32_u8(vrev32q_u8(vreinterpretq_u8_u32(d.val[3])));
      // overlap: p0 < e1 && p1 > e0 && p2 < e3 && p3 > e2
      const uint32x4_t hit = vandq_u32(vandq_u32(vcgtq_s32(e1, v0), vcltq_s32(e0, v1)), vandq_u32(vcgtq_s32(e3, v2), vcltq_s32(e2, v3)));
      if (vmaxvq_u32(hit) != 0) {
        ctx.r3.u64 = 0;
        return;
      }
      ptr += 64;
      e += 64;
      n -= 4;
    } while (n >= 4);
  }
#endif
  for (; n != 0; --n, e += 16) {
    if (Overlaps(base, e, p0, p1, p2, p3)) {
      ctx.r3.u64 = 0;
      return;
    }
  }
  ctx.r3.u64 = 1;
}

inline void Writes(const PPCContext&, const uint8_t*, me::hot::Writes&) {}

}  // namespace me::hot::n_82B5F0E8

// sub_8262CFC0 - "all planes reject the box" test (leaf, VMX128): returns r3 = 1 when no group of four planes rejects, else 0.
//
//   r3 = object: [+12] -> plane array in SoA groups of four planes (4 x 16-byte vectors per group: normal x, y, z, d), [+16] = s32
//        number of planes (the loop runs ceil(n / 4) times; n <= 0: returns 1 without touching the FPU),
//   r4 -> 3 floats (box center c), r5 -> 3 floats (box half extents e).
//   Per group: dist = cx * X + cy * Y + cz * Z - D, ext = |ex| * |X| + |ey| * |Y| + |ez| * |Z|; any lane with dist > ext -> 0.
//
// Exactness notes:
//  * The original builds c and e with lvlx/lvrx pairs at +0 / +12 and a 4-byte rotate; elements 0..2 of the result are exactly the
//    three big-endian words at the address (only these are used), the fourth word never is. The plane vectors are plain unaligned
//    16-byte loads (lvlx/lvrx at +0 / +16). All arithmetic is lane wise, so a natural lane order (element i in lane i) gives the same
//    decision as the recompiled byte-reversed register image.
//  * The float ops are the recompiled ones in the same operand order: mul(a, b), add(mul(a, b), c), sub, ordered compare
//    (vcgtq_f32 == simde_mm_cmpgt_ps): no fma, no reassociation. |x| is a sign-bit clear.
//  * ctx.fpscr.enableFlushMode() is the first FPU-related instruction of the first iteration: called once before the loop when
//    n > 0 (idempotent), not called when the loop is skipped.
//  * Only r3 is returned; the recompiled code also leaves r5..r11 and v0..v13 behind (liveness.py: no direct call site, the
//    function is only reached through the vtable-style ABI-conforming callers). The spills at 20/28/36(r1) are callee-owned
//    parameter-area scratch. No guest memory is written.
#pragma once

#include "../me_hot_common.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#else
#include <simde/x86/sse4.1.h>
#endif

namespace me::hot::n_8262CFC0 {

inline constexpr Cmp kCmp = {R(3), 0, 0};

#if defined(__aarch64__)
using V4 = float32x4_t;
inline V4 LoadV(const uint8_t* p) { return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(p))); }
inline V4 Abs(V4 v) { return vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(v), vdupq_n_u32(0x7FFFFFFFu))); }
inline V4 Mul(V4 a, V4 b) { return vmulq_f32(a, b); }
inline V4 Add(V4 a, V4 b) { return vaddq_f32(a, b); }
inline V4 Sub(V4 a, V4 b) { return vsubq_f32(a, b); }
inline bool AnyGt(V4 a, V4 b) { return vmaxvq_u32(vcgtq_f32(a, b)) != 0; }
inline V4 Splat(uint32_t bits) { return vreinterpretq_f32_u32(vdupq_n_u32(bits)); }
#else
using V4 = simde__m128;
inline V4 LoadV(const uint8_t* p) {
  return simde_mm_castsi128_ps(
      simde_mm_shuffle_epi8(simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(p)),
                            simde_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)));
}
inline V4 Abs(V4 v) { return simde_mm_and_ps(v, simde_mm_castsi128_ps(simde_mm_set1_epi32(0x7FFFFFFF))); }
inline V4 Mul(V4 a, V4 b) { return simde_mm_mul_ps(a, b); }
inline V4 Add(V4 a, V4 b) { return simde_mm_add_ps(a, b); }
inline V4 Sub(V4 a, V4 b) { return simde_mm_sub_ps(a, b); }
inline bool AnyGt(V4 a, V4 b) { return simde_mm_movemask_ps(simde_mm_cmpgt_ps(a, b)) != 0; }
inline V4 Splat(uint32_t bits) { return simde_mm_castsi128_ps(simde_mm_set1_epi32(int(bits))); }
#endif

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t obj = ctx.r3.u32;
  const uint32_t pc = ctx.r4.u32, pe = ctx.r5.u32;
  const int32_t n = int32_t(Ld32(base, obj + 16));
  const uint32_t planes = Ld32(base, obj + 12);
  if (n <= 0) {
    ctx.r3.s64 = 1;
    return;
  }
  const V4 cx = Splat(Ld32(base, pc + 0)), cy = Splat(Ld32(base, pc + 4)), cz = Splat(Ld32(base, pc + 8));
  const V4 ex = Splat(Ld32(base, pe + 0) & 0x7FFFFFFFu), ey = Splat(Ld32(base, pe + 4) & 0x7FFFFFFFu),
           ez = Splat(Ld32(base, pe + 8) & 0x7FFFFFFFu);
  ctx.fpscr.enableFlushMode();
  uint32_t a = planes;
  int32_t i = 0;
  do {
    const uint8_t* p = Raw(base, a);  // 64 bytes; a group may straddle nothing special on the 32-bit wrap (host memory is contiguous)
    const V4 x = LoadV(p), y = LoadV(p + 16), z = LoadV(p + 32), d = LoadV(p + 48);
    const V4 t1 = Mul(cx, x);
    const V4 t2 = Mul(ex, Abs(x));
    const V4 t3 = Add(Mul(cy, y), t1);
    const V4 t4 = Add(Mul(cz, z), t3);
    const V4 dist = Sub(t4, d);
    const V4 e1 = Add(Mul(ey, Abs(y)), t2);
    const V4 ext = Add(Mul(ez, Abs(z)), e1);
    if (AnyGt(dist, ext)) {
      ctx.r3.s64 = 0;
      return;
    }
    a += 64;
    i += 4;
  } while (i < n);
  ctx.r3.s64 = 1;
}

inline void Writes(const PPCContext&, const uint8_t*, me::hot::Writes&) {}

}  // namespace me::hot::n_8262CFC0

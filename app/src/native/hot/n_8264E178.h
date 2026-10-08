// sub_8264E178 - UE3 FDynamicSpriteEmitterData::Render(Proxy, PDI, View, DPGIndex): sort keys of the sprite particles,
// sort, then DrawRichMesh of the sprite mesh element.
// English copy of editions/ru/overlay/app/src/native/hot/n_8264E178.h (Russian sub_8264EBB0, where it is fuzzed against the
// recompiled original): the guest constants and callee names translated through editions/ru/address_map.json and the
// English data addresses (checked against the English generated code: lis/addi of r30 = 0.0 / 1.0 words, lis/addi of the
// identity matrix). Fuzzed against the English generated sub_8264E178 on 2026-10-08 (tests/hot_fuzz/cases/case_8264E178.inc,
// 200000 iterations, 0 failures; a wrong constant is caught at once). The loc_ labels below are the Russian ones.
//
//   r3 = Proxy (FParticleSystemSceneProxy: +16 PrimitiveSceneInfo, +32/+96/+160 three 4x4 matrices (+32 = LocalToWorld),
//        +224 float, +276 / +284 flag words), r4 = this (dynamic emitter data: +8 s32 active particle count, +16 s32 particle
//        stride, +20 particle data, +24 -> u16 particle indices, +44 material render proxy, +52 u8, +56 s32 bUseLocalSpace,
//        +60 s32 lock-axis flag, +72 s32 EmitterRenderMode, +96 s32, +116 TArray<{u32 index, float key}> ParticleOrder,
//        +128 vertex factory), r5 = PDI, r6 = View (+264/+280/+296/+312: one column of the view-projection matrix),
//        r7 = DPGIndex.
//   EmitterRenderMode 1 / 2 (point / cross): virtual RenderDebug ([[r4]+4], r7 = 0 / 1), other non-zero: nothing.
//   Mode 0: lock-axis values (sub_82653910 -> vertex factory +172 / +184), Material = [[r4+44]]->vfunc0, and when the material
//   asks for sorting (vfunc60 == 2 and (vfunc56 == 2 or vfunc28 != 0)): ParticleOrder.Empty(count) (sub_82210368), then per
//   particle i: P = data + indices[i] * stride, location = P+16..P+27 (transformed by LocalToWorld when bUseLocalSpace),
//   key = dot(View column, (location, 1)), ParticleOrder.AddUninitialized(1) (sub_82210298) = {i, key}; sort (sub_82654030).
//   Then the FMeshElement is built in the stack frame (r1+80 .. r1+392) and DrawRichMesh (sub_8250B8E8) is called.
//
// Why a native version: the per-particle loop is the hottest gameplay leaf of the UE3 render thread in the Feros firefight
// profile (run/me1/manual2: 2.5 % of the busy render-thread samples, 34 of 41 pc samples on the first load of the particle
// record, i.e. a cache miss per particle). The native loop prefetches the particle records 8 iterations ahead (both the
// location line read here and the +96 line the sprite vertex fill sub_8264C7C0 reads next), keeps the loop invariants in
// registers and inlines the no-grow path of AddUninitialized.
//
// Exactness:
//  * Everything outside the particle loop is the generated code transliterated statement by statement: the same guest loads
//    and stores in the same order, the same values in ctx.r3..r10 / f1..f13 at every guest call (the vcalls go through
//    CallIndirect = REX_CALL_INDIRECT_FUNC, the direct calls name the same symbols as the generated code), the same 64-bit
//    register arithmetic (the r1-relative arguments keep r1's upper half), the same disableFlushMode points.
//  * The loop has two implementations. Fast: when, after Empty, count == 0, s32 capacity >= the particle count (no grow
//    can happen, so the allocator is never reached), data != 0 and data + 8 * n <= 0xE0000000, and the element range plus the
//    count word are disjoint from every value the fast loop keeps in registers (this +16..+27, data / capacity words,
//    LocalToWorld rows when local space, the view column). Per iteration it keeps the original order: index load, particle
//    loads, count store, key store, index store; the floats are evaluated with the same double / float expressions as the
//    generated code. Faithful: the generated loop as is (any aliasing, the grow path through sub_82210298 / the allocator).
//  * The prefetch reads indices[i + 8] early: the original reads the same address later (the index pointer cannot change
//    inside the fast loop), and a prefetch instruction has no architectural effect.
//  * Return value: r3 of the last callee (DrawRichMesh or RenderDebug), or the input r3 when nothing is called (kCmp = r3).
//    The function is only called through a vtable (no direct call site, tests/hot_fuzz/liveness.py), so the other volatile
//    registers are scratch; they are nevertheless reproduced where the original leaves them for its callees.
//  * Writes(): every path that calls guest code has an unbounded write set (virtual calls, DrawRichMesh) -> overflow, i.e. the
//    in-game self-check guard runs the original for its checked calls; the native version is verified by the host fuzzer
//    (tests/hot_fuzz/cases/case_8264E178.inc; Russian code: editions/ru/overlay/tests/hot_fuzz/cases/case_8264E178.inc).
#pragma once

#include "../me_hot_common.h"
#include "../me_hot_call.h"

#include <cmath>

extern "C" void sub_82653910(PPCContext&, uint8_t*);  // GetAxisLockValues (leaf)
extern "C" void sub_82210368(PPCContext&, uint8_t*);         // TArray::Empty(r3 = array, r4 = size, r5 = align, r6 = slack)
extern "C" void sub_82210298(PPCContext&, uint8_t*);         // TArray::AddUninitialized(r3 = array, r4 = n, r5 = size, r6 = align)
extern "C" void sub_82654030(PPCContext&, uint8_t*);         // sort of {u32, float key} by key (hot hook n_82654030)
extern "C" void sub_82AC4AF0(PPCContext&, uint8_t*);         // CRT memcpy (hot hook n_82AC4AF0)
extern "C" void sub_8250B8E8(PPCContext&, uint8_t*);  // DrawRichMesh

namespace me::hot::n_8264E178 {

inline constexpr Cmp kCmp = kCmpRet;

namespace detail {

inline constexpr uint32_t kZero = 0x820B90B8u;     // float 0.0 in the real data (f31 at loc_8264EE14)
inline constexpr uint32_t kOne = 0x820BCD64u;      // float 1.0 in the real data (f0 at loc_8264EF54)
inline constexpr uint32_t kIdent = 0x82E64F30u;    // 64-byte identity matrix (world-space emitters)
inline constexpr uint32_t kPrefetch = 8;           // particles ahead

inline double LdD(const uint8_t* base, uint32_t a) {  // lfs: temp.u32 = load; double(temp.f32)
  const uint32_t u = Ld32(base, a);
  float f;
  std::memcpy(&f, &u, 4);
  return double(f);
}
inline uint32_t FBits(double d) {  // stfs: temp.f32 = float(d)
  const float f = float(d);
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
inline uint64_t Rot0(uint64_t v) { return __builtin_rotateleft64(uint32_t(v) | (v << 32), 0); }
inline bool Disjoint(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen) { return a + alen <= b || b + blen <= a; }

// The volatile FPRs the loop body leaves in ctx (f0 and f27-f31 are locals of the generated code).
struct Fpr {
  double f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13;
};

// Loop-invariant inputs of the key: the raw words (converted where the original converts them, every iteration).
struct Inv {
  uint32_t m52, m56, m48, m68, m72, m64, m36, m40, m32, m84, m88, m80;  // LocalToWorld (local space only)
  uint32_t v280, v296, v264, v312;                                      // view column
};
inline double W2D(uint32_t u) {  // temp.u32 = word; double(temp.f32)
  float f;
  std::memcpy(&f, &u, 4);
  return double(f);
}

// One key: the generated statements in their order, with the loads replaced by the hoisted words.
inline double KeyLocal(const Inv& c, double f0, double f12, double f13, Fpr& o) {
  double f11, f10, f9, f8, f7, f6, f5, f4, f3, f2, f1, f31;
  f11 = W2D(c.m52);
  f11 = double(float(f11 * f0));
  f10 = W2D(c.m56);
  f9 = W2D(c.m48);
  f10 = double(float(f10 * f0));
  f8 = W2D(c.m68);
  f0 = double(float(f9 * f0));
  f7 = W2D(c.m72);
  f6 = W2D(c.m64);
  f5 = W2D(c.m36);
  f4 = W2D(c.m40);
  f3 = W2D(c.m32);
  f2 = W2D(c.m84);
  f1 = W2D(c.m88);
  f11 = double(float(std::fma(f8, f13, f11)));
  const double f30 = W2D(c.v280);
  f10 = double(float(std::fma(f7, f13, f10)));
  f31 = W2D(c.m80);
  f0 = double(float(std::fma(f6, f13, f0)));
  const double f29 = W2D(c.v296);
  const double f28 = W2D(c.v264);
  const double f27 = W2D(c.v312);
  f13 = double(float(std::fma(f5, f12, f11)));
  f11 = double(float(std::fma(f4, f12, f10)));
  f12 = double(float(std::fma(f3, f12, f0)));
  f0 = double(float(f13 + f2));
  f13 = double(float(f11 + f1));
  f12 = double(float(f12 + f31));
  f0 = double(float(f30 * f0));
  f0 = double(float(std::fma(f29, f13, f0)));
  f0 = double(float(std::fma(f28, f12, f0)));
  f31 = double(float(f0 + f27));
  o.f1 = f1; o.f2 = f2; o.f3 = f3; o.f4 = f4; o.f5 = f5; o.f6 = f6; o.f7 = f7; o.f8 = f8; o.f9 = f9;
  o.f10 = f10; o.f11 = f11; o.f12 = f12; o.f13 = f13;
  return f31;
}
inline double KeyWorld(const Inv& c, double f0, double f12, double f13, Fpr& o) {
  double f11, f10, f9, f8, f31;
  f11 = W2D(c.v280);
  f0 = double(float(f11 * f0));
  f10 = W2D(c.v296);
  f9 = W2D(c.v264);
  f8 = W2D(c.v312);
  f0 = double(float(std::fma(f10, f13, f0)));
  f0 = double(float(std::fma(f9, f12, f0)));
  f31 = double(float(f0 + f8));
  o.f8 = f8; o.f9 = f9; o.f10 = f10; o.f11 = f11; o.f12 = f12; o.f13 = f13;
  return f31;
}

inline void StoreFpr(PPCContext& ctx, const Fpr& o, bool local) {
  if (local) {
    ctx.f1.f64 = o.f1; ctx.f2.f64 = o.f2; ctx.f3.f64 = o.f3; ctx.f4.f64 = o.f4; ctx.f5.f64 = o.f5;
    ctx.f6.f64 = o.f6; ctx.f7.f64 = o.f7;
  }
  ctx.f8.f64 = o.f8; ctx.f9.f64 = o.f9; ctx.f10.f64 = o.f10; ctx.f11.f64 = o.f11; ctx.f12.f64 = o.f12; ctx.f13.f64 = o.f13;
}

// The generated loop body (loc_8264ECF8 .. loc_8264EDF4) for iteration r27 (r29 = 2 * r27), unchanged.
inline void SlowIteration(PPCContext& ctx, uint8_t* base, uint64_t r31, uint64_t r28, uint64_t r25, uint64_t r30, uint64_t r26,
                          uint64_t r27, uint64_t r29) {
  uint64_t r11 = Ld32(base, uint32_t(r31) + 24);
  const bool world = int32_t(r26) == 0;
  ctx.r9.u64 = Ld32(base, uint32_t(r31) + 16);
  ctx.r10.u64 = Ld32(base, uint32_t(r31) + 20);
  r11 = Ld16(base, uint32_t(r11) + uint32_t(r29));
  r11 = uint64_t(int64_t(int32_t(uint32_t(r11))) * int64_t(ctx.r9.s32));
  r11 = r11 + ctx.r10.u64;
  ctx.fpscr.disableFlushMode();
  double f0 = LdD(base, uint32_t(r11) + 20);
  ctx.f13.f64 = LdD(base, uint32_t(r11) + 24);
  ctx.f12.f64 = LdD(base, uint32_t(r11) + 16);
  double f31;
  if (!world) {
    const uint32_t p = uint32_t(r28), v = uint32_t(r25);
    ctx.f11.f64 = LdD(base, p + 52);
    ctx.f11.f64 = double(float(ctx.f11.f64 * f0));
    ctx.f10.f64 = LdD(base, p + 56);
    ctx.f9.f64 = LdD(base, p + 48);
    ctx.f10.f64 = double(float(ctx.f10.f64 * f0));
    ctx.f8.f64 = LdD(base, p + 68);
    f0 = double(float(ctx.f9.f64 * f0));
    ctx.f7.f64 = LdD(base, p + 72);
    ctx.f6.f64 = LdD(base, p + 64);
    ctx.f5.f64 = LdD(base, p + 36);
    ctx.f4.f64 = LdD(base, p + 40);
    ctx.f3.f64 = LdD(base, p + 32);
    ctx.f2.f64 = LdD(base, p + 84);
    ctx.f1.f64 = LdD(base, p + 88);
    ctx.f11.f64 = double(float(std::fma(ctx.f8.f64, ctx.f13.f64, ctx.f11.f64)));
    const double f30 = LdD(base, v + 280);
    ctx.f10.f64 = double(float(std::fma(ctx.f7.f64, ctx.f13.f64, ctx.f10.f64)));
    f31 = LdD(base, p + 80);
    f0 = double(float(std::fma(ctx.f6.f64, ctx.f13.f64, f0)));
    const double f29 = LdD(base, v + 296);
    const double f28 = LdD(base, v + 264);
    const double f27 = LdD(base, v + 312);
    ctx.f13.f64 = double(float(std::fma(ctx.f5.f64, ctx.f12.f64, ctx.f11.f64)));
    ctx.f11.f64 = double(float(std::fma(ctx.f4.f64, ctx.f12.f64, ctx.f10.f64)));
    ctx.f12.f64 = double(float(std::fma(ctx.f3.f64, ctx.f12.f64, f0)));
    f0 = double(float(ctx.f13.f64 + ctx.f2.f64));
    ctx.f13.f64 = double(float(ctx.f11.f64 + ctx.f1.f64));
    ctx.f12.f64 = double(float(ctx.f12.f64 + f31));
    f0 = double(float(f30 * f0));
    f0 = double(float(std::fma(f29, ctx.f13.f64, f0)));
    f0 = double(float(std::fma(f28, ctx.f12.f64, f0)));
    f31 = double(float(f0 + f27));
  } else {
    const uint32_t v = uint32_t(r25);
    ctx.fpscr.disableFlushMode();
    ctx.f11.f64 = LdD(base, v + 280);
    f0 = double(float(ctx.f11.f64 * f0));
    ctx.f10.f64 = LdD(base, v + 296);
    ctx.f9.f64 = LdD(base, v + 264);
    ctx.f8.f64 = LdD(base, v + 312);
    f0 = double(float(std::fma(ctx.f10.f64, ctx.f13.f64, f0)));
    f0 = double(float(std::fma(ctx.f9.f64, ctx.f12.f64, f0)));
    f31 = double(float(f0 + ctx.f8.f64));
  }
  ctx.r6.s64 = 8;
  ctx.r5.s64 = 8;
  ctx.r4.s64 = 1;
  ctx.r3.u64 = r30;
  sub_82210298(ctx, base);
  ctx.r10.u64 = Ld32(base, uint32_t(r30));
  r11 = (__builtin_rotateleft64(ctx.r3.u32 | (ctx.r3.u64 << 32), 3) & 0xFFFFFFF8ull) + ctx.r10.u64;
  if (int32_t(uint32_t(r11)) != 0) {
    ctx.fpscr.disableFlushMode();
    St32(base, uint32_t(r11) + 4, FBits(f31));
    St32(base, uint32_t(r11), uint32_t(r27));
  }
}

// The fast loop (see the file comment); returns the number of iterations done (all of them when the preconditions hold,
// 0 otherwise: nothing has been read or written then).
inline bool IsNaN(uint32_t u) { return (u & 0x7FFFFFFFu) > 0x7F800000u; }

inline uint32_t FastLoop(PPCContext& ctx, uint8_t* base, uint64_t r31x, uint64_t r28x, uint64_t r25x, uint64_t r30x,
                         uint64_t r26, int32_t n) {
  const uint32_t r31 = uint32_t(r31x), r28 = uint32_t(r28x), r25 = uint32_t(r25x), arr = uint32_t(r30x);
  const bool local = int32_t(uint32_t(r26)) != 0;
  const uint32_t data = Ld32(base, arr), cnt = Ld32(base, arr + 4), cap = Ld32(base, arr + 8);
  if (cnt != 0 || int32_t(cap) < n || data == 0 || uint64_t(data) + 8ull * uint32_t(n) > 0xE0000000ull) return 0;
  // Ranges the fast loop writes: the elements and the count word; ranges it keeps in registers.
  const uint64_t wlo = data, wlen = 8ull * uint32_t(n);
  auto ok = [&](uint64_t a, uint64_t len) {
    return a + len <= 0x100000000ull && Disjoint(wlo, wlen, a, len) && Disjoint(uint64_t(arr) + 4, 4, a, len);
  };
  if (!ok(uint64_t(r31) + 16, 12) || !ok(arr, 4) || !ok(uint64_t(arr) + 8, 4) || !ok(uint64_t(r25) + 264, 52)) return 0;
  if (uint64_t(arr) + 12 > 0x100000000ull || !Disjoint(wlo, wlen, arr, 12)) return 0;
  if (local && !ok(uint64_t(r28) + 32, 60)) return 0;

  const uint32_t stride = Ld32(base, r31 + 16), pdata = Ld32(base, r31 + 20), idxp = Ld32(base, r31 + 24);
  Inv c{};
  // Converted once (the original converts the same words every iteration with the flush mode cleared, as here).
  ctx.fpscr.disableFlushMode();
  if (local) {
    c.m52 = Ld32(base, r28 + 52); c.m56 = Ld32(base, r28 + 56); c.m48 = Ld32(base, r28 + 48); c.m68 = Ld32(base, r28 + 68);
    c.m72 = Ld32(base, r28 + 72); c.m64 = Ld32(base, r28 + 64); c.m36 = Ld32(base, r28 + 36); c.m40 = Ld32(base, r28 + 40);
    c.m32 = Ld32(base, r28 + 32); c.m84 = Ld32(base, r28 + 84); c.m88 = Ld32(base, r28 + 88); c.m80 = Ld32(base, r28 + 80);
  }
  c.v280 = Ld32(base, r25 + 280); c.v296 = Ld32(base, r25 + 296); c.v264 = Ld32(base, r25 + 264); c.v312 = Ld32(base, r25 + 312);
  // NaN inputs: which operand's NaN an instruction returns depends on the operand order the compiler picked for the
  // original, which the source does not define; such keys take the generated statements (SlowIteration) instead.
  bool nan_inv = IsNaN(c.v280) | IsNaN(c.v296) | IsNaN(c.v264) | IsNaN(c.v312);
  if (local)
    nan_inv |= IsNaN(c.m52) | IsNaN(c.m56) | IsNaN(c.m48) | IsNaN(c.m68) | IsNaN(c.m72) | IsNaN(c.m64) | IsNaN(c.m36) |
               IsNaN(c.m40) | IsNaN(c.m32) | IsNaN(c.m84) | IsNaN(c.m88) | IsNaN(c.m80);
  if (nan_inv) return 0;

  Fpr o{};
  bool last_slow = false;
  auto rec = [&](uint32_t i) {  // particle record address of iteration i (as the original computes it)
    const uint32_t idx = Ld16(base, idxp + 2u * i);
    return uint32_t(uint64_t(int64_t(int32_t(idx)) * int64_t(int32_t(stride))) + pdata);
  };
  const uint32_t pf = uint32_t(n) < kPrefetch ? uint32_t(n) : kPrefetch;
  for (uint32_t i = 0; i < pf; ++i) {
    const uint8_t* q = Raw(base, rec(i));
    __builtin_prefetch(q + 16, 0, 3);
    __builtin_prefetch(q + 96, 0, 2);
  }
  for (uint32_t i = 0; i < uint32_t(n); ++i) {
    if (i + kPrefetch < uint32_t(n)) {
      const uint8_t* q = Raw(base, rec(i + kPrefetch));
      __builtin_prefetch(q + 16, 0, 3);
      __builtin_prefetch(q + 96, 0, 2);
    }
    const uint32_t p = rec(i);
    const uint32_t wy = Ld32(base, p + 20), wz = Ld32(base, p + 24), wx = Ld32(base, p + 16);
    if (IsNaN(wy) | IsNaN(wz) | IsNaN(wx)) [[unlikely]] {
      SlowIteration(ctx, base, r31x, r28x, r25x, r30x, r26, i, 2ull * i);
      last_slow = true;
      continue;
    }
    last_slow = false;
    const double f0 = W2D(wy), f13 = W2D(wz), f12 = W2D(wx);
    const double key = local ? KeyLocal(c, f0, f12, f13, o) : KeyWorld(c, f0, f12, f13, o);
    St32(base, arr + 4, i + 1);  // AddUninitialized(1): count = i + 1 (<= capacity)
    const uint32_t e = data + 8u * i;
    St32(base, e + 4, FBits(key));
    St32(base, e, i);
  }
  if (last_slow) return uint32_t(n);  // SlowIteration left the registers
  // Registers as the last AddUninitialized / reload leaves them.
  ctx.r3.u64 = uint32_t(n) - 1u;
  ctx.r4.s64 = 1;
  ctx.r5.s64 = 8;
  ctx.r6.s64 = 8;
  ctx.r9.u64 = cap;
  ctx.r10.u64 = data;
  StoreFpr(ctx, o, local);
  return uint32_t(n);
}

}  // namespace detail

inline void Native(PPCContext& ctx, uint8_t* base) {
  using namespace detail;
  const uint32_t ea = ctx.r1.u32 - 544u;
  St32(base, ea, ctx.r1.u32);  // stwu r1,-544(r1)
  ctx.r1.u32 = ea;
  const uint64_t r31 = ctx.r4.u64, r28 = ctx.r3.u64, r21 = ctx.r5.u64, r25 = ctx.r6.u64;
  uint64_t r22 = ctx.r7.u64;
  const uint32_t mode = Ld32(base, uint32_t(r31) + 72);
  if (int32_t(mode) != 0) {
    if (int32_t(mode) == 1) {
      ctx.r7.s64 = 0;
    } else if (int32_t(mode) == 2) {
      ctx.r7.s64 = 1;
    } else {
      ctx.r1.s64 = ctx.r1.s64 + 544;
      return;
    }
    const uint32_t vt = Ld32(base, uint32_t(r31));
    ctx.r3.u64 = r31;
    ctx.r4.u64 = r21;
    ctx.r5.u64 = r25;
    ctx.r6.u64 = r22;
    CallIndirect(ctx, base, Ld32(base, vt + 4));
    ctx.r1.s64 = ctx.r1.s64 + 544;
    return;
  }

  {  // vertex factory: lock flags and axis-lock values
    uint64_t r11 = Ld8(base, uint32_t(r31) + 52);
    ctx.r10.u64 = Ld32(base, uint32_t(r31) + 128);
    St8(base, ctx.r10.u32 + 164, uint8_t(r11));
    r11 = Ld32(base, uint32_t(r31) + 128);
    ctx.r10.u64 = Ld32(base, uint32_t(r31) + 60);
    St32(base, uint32_t(r11) + 168, ctx.r10.u32);
    r11 = Ld32(base, uint32_t(r31) + 60);
    if (int32_t(uint32_t(r11)) != 0) {
      ctx.r6.s64 = ctx.r1.s64 + 80;
      ctx.r5.s64 = ctx.r1.s64 + 96;
      sub_82653910(ctx, base);
      r11 = Ld32(base, uint32_t(r31) + 128);
      const uint32_t sp = ctx.r1.u32;
      ctx.r10.u64 = Ld32(base, sp + 96);
      ctx.r4.s64 = int64_t(r11) + 172;
      ctx.r9.u64 = Ld32(base, sp + 100);
      ctx.r8.u64 = Ld32(base, sp + 104);
      r11 = r11 + 184;
      ctx.r7.u64 = Ld32(base, sp + 80);
      ctx.r6.u64 = Ld32(base, sp + 84);
      ctx.r5.u64 = Ld32(base, sp + 88);
      St32(base, ctx.r4.u32 + 0, ctx.r10.u32);
      St32(base, ctx.r4.u32 + 4, ctx.r9.u32);
      St32(base, ctx.r4.u32 + 8, ctx.r8.u32);
      St32(base, uint32_t(r11) + 0, ctx.r7.u32);
      St32(base, uint32_t(r11) + 4, ctx.r6.u32);
      St32(base, uint32_t(r11) + 8, ctx.r5.u32);
    }
  }

  // loc_8264EC50: material, sorting decision, keys, sort.
  const uint64_t r24 = 0;
  uint64_t r29 = 0;
  ctx.r3.u64 = Ld32(base, uint32_t(r31) + 44);
  const uint64_t r23 = Ld32(base, uint32_t(r31) + 8);
  bool sort = false;
  CallIndirect(ctx, base, Ld32(base, Ld32(base, ctx.r3.u32)));
  const uint64_t mat = ctx.r3.u64;
  if (int32_t(uint32_t(mat)) != 0) {
    ctx.r3.u64 = mat;
    CallIndirect(ctx, base, Ld32(base, Ld32(base, uint32_t(mat)) + 60));
    if (ctx.r3.s32 == 2) {
      ctx.r3.u64 = mat;
      CallIndirect(ctx, base, Ld32(base, Ld32(base, uint32_t(mat)) + 56));
      if (ctx.r3.s32 == 2) {
        sort = true;
      } else {
        ctx.r3.u64 = mat;
        CallIndirect(ctx, base, Ld32(base, Ld32(base, uint32_t(mat)) + 28));
        sort = ctx.r3.s32 != 0;
      }
    }
  }
  uint64_t r30 = 0;
  if (sort) {  // loc_8264ECCC
    r30 = uint64_t(int64_t(r31) + 116);
    ctx.r6.u64 = r23;
    ctx.r5.s64 = 8;
    ctx.r4.s64 = 8;
    ctx.r3.u64 = r30;
    sub_82210368(ctx, base);
    const uint64_t r26 = Ld32(base, uint32_t(r31) + 56);
    if (int32_t(uint32_t(r23)) > 0) {
      const uint32_t done = FastLoop(ctx, base, r31, r28, r25, r30, r26, int32_t(uint32_t(r23)));
      if (done == 0) {
        uint64_t r27 = 0;
        r29 = 0;
        do {
          SlowIteration(ctx, base, r31, r28, r25, r30, r26, r27, r29);
          r27 = r27 + 1;
          r29 = r29 + 2;
        } while (int32_t(uint32_t(r27)) < int32_t(uint32_t(r23)));
      }
    }
    // loc_8264EE04
    ctx.r4.u64 = Ld32(base, uint32_t(r31) + 120);
    ctx.r3.u64 = Ld32(base, uint32_t(r30));
    sub_82654030(ctx, base);
    r29 = 1;
  }

  // loc_8264EE14: the mesh element in the stack frame.
  const uint32_t sp = ctx.r1.u32;
  uint64_t r11 = Ld32(base, sp + 376);
  const bool sorted = int32_t(uint32_t(r29)) == 1;
  St32(base, sp + 128, uint32_t(r24));
  ctx.r10.u64 = uint32_t(r11) & 0x7FFFFFu;
  r11 = Ld32(base, uint32_t(r31) + 128);
  St32(base, sp + 136, uint32_t(r31));
  ctx.r10.u64 = Rot0(ctx.r10.u64) & 0xFFFFFFFFFFF7FFFFull;
  St32(base, sp + 144, uint32_t(r24));
  St32(base, sp + 148, uint32_t(r24));
  St32(base, sp + 132, uint32_t(r11));
  const uint32_t r30c = kOne;  // lis/addi: 0x820C0000 - 12956
  r11 = ctx.r10.u64 | 2684878848ull;
  ctx.fpscr.disableFlushMode();
  static_assert(kOne - 15532u == kZero);
  double f31 = LdD(base, kZero);
  St32(base, sp + 376, uint32_t(r11));
  St32(base, sp + 368, FBits(f31));
  St32(base, sp + 372, FBits(f31));
  St32(base, sp + 140, 88);
  if (sorted) St32(base, sp + 144, uint32_t(int64_t(r31) + 116));
  r11 = Ld32(base, uint32_t(r31) + 56);
  St32(base, sp + 156, uint32_t(r24));
  if (int32_t(uint32_t(r11)) == 1) {
    for (int k = 0; k < 3; ++k) {
      ctx.r4.s64 = int64_t(r28) + 32 + 64 * k;
      ctx.r3.s64 = ctx.r1.s64 + 160 + 64 * k;
      ctx.r5.s64 = 64;
      sub_82AC4AF0(ctx, base);
    }
  } else {
    const uint32_t src = kIdent;
    const uint32_t d0 = ctx.r1.u32 + 160;
    for (uint32_t k = 0; k < 8; ++k) St64(base, d0 + 8 * k, Ld64(base, src + 8 * k));
    for (uint32_t k = 0; k < 7; ++k) St64(base, d0 + 64 + 8 * k, Ld64(base, src + 8 * k));
    ctx.r8.u64 = Ld64(base, src + 56);
    St64(base, d0 + 120, ctx.r8.u64);
    for (uint32_t k = 0; k < 7; ++k) St64(base, d0 + 128 + 8 * k, Ld64(base, src + 8 * k));
    ctx.r9.u64 = Ld64(base, src + 56);
    St64(base, d0 + 184, ctx.r9.u64);
    ctx.r10.s64 = ctx.r1.s64 + 288 + 64;
  }

  // loc_8264EF20
  ctx.r10.u64 = Ld32(base, uint32_t(r31) + 96);
  ctx.r9.s64 = 1;
  r11 = Ld32(base, sp + 376);
  ctx.fpscr.disableFlushMode();
  double f0 = LdD(base, uint32_t(r28) + 224);
  ctx.r10.s64 = ctx.r10.s64 + -1;
  St32(base, sp + 352, uint32_t(r24));
  const bool lt = f0 < f31;
  r11 = (__builtin_rotateleft64(ctx.r9.u32 | (ctx.r9.u64 << 32), 24) & 0x1800000ull) | (r11 & 0xFFFFFFFFFE7FFFFFull);
  St32(base, sp + 360, uint32_t(r24));
  St32(base, sp + 364, ctx.r10.u32);
  ctx.r10.s64 = lt ? 1 : int64_t(r24);
  // loc_8264EF54
  r22 = (__builtin_rotateleft64(ctx.r10.u32 | (ctx.r10.u64 << 32), 10) & 0x400ull) | (r22 & 0xFFFFFFFFFFFFFBFFull);
  ctx.r9.u64 = Ld32(base, uint32_t(r28) + 284);
  r11 = Rot0(r11) & 0xFFFFFFFFFF8FFFFFull;
  ctx.r8.u64 = Ld32(base, uint32_t(r31) + 44);
  ctx.r10.u64 = __builtin_rotateleft64(r22 & 0xFFFFFFFFull | (r22 << 32), 20) & 0x7FF00000ull;
  ctx.fpscr.disableFlushMode();
  f0 = LdD(base, r30c);
  ctx.r6.u64 = __builtin_rotateleft64(ctx.r9.u32 | (ctx.r9.u64 << 32), 30) & 0x20000000ull;
  ctx.r9.u64 = Ld32(base, uint32_t(r28) + 276);
  ctx.r10.u64 = Rot0(ctx.r10.u64) & 0xFFFFFFFFC07FFFFFull;
  St32(base, sp + 384, FBits(f31));
  r11 = Rot0(r11) & 0xFFFFFFFF9FFFFFFFull;
  St32(base, sp + 80, FBits(f0));
  ctx.r10.u64 = ctx.r10.u64 | ctx.r6.u64;
  St32(base, sp + 152, ctx.r8.u32);
  ctx.r7.s64 = ctx.r1.s64 + 80;
  ctx.r8.u64 = Ld32(base, uint32_t(r28) + 16);
  r11 = ctx.r10.u64 | r11;
  St32(base, sp + 84, FBits(f0));
  ctx.r6.s64 = ctx.r1.s64 + 96;
  St32(base, sp + 88, FBits(f0));
  r11 = Rot0(r11) & 0xFFFFFFFFF1FFFFFFull;
  St32(base, sp + 92, FBits(f0));
  ctx.r5.s64 = ctx.r1.s64 + 112;
  St32(base, sp + 96, FBits(f0));
  ctx.r4.s64 = ctx.r1.s64 + 128;
  St32(base, sp + 100, FBits(f0));
  ctx.r3.u64 = r21;
  St32(base, sp + 104, FBits(f31));
  St32(base, sp + 108, FBits(f0));
  St32(base, sp + 356, uint32_t(r23));
  St32(base, sp + 112, FBits(f0));
  St32(base, sp + 376, uint32_t(r11));
  St32(base, sp + 116, FBits(f31));
  St32(base, sp + 380, uint32_t(r24));
  St32(base, sp + 120, FBits(f31));
  St32(base, sp + 124, FBits(f0));
  sub_8250B8E8(ctx, base);
  ctx.r1.s64 = ctx.r1.s64 + 544;
}

// Every path that reaches guest code (virtual calls, DrawRichMesh) has an unbounded write set; only the "other render
// mode" path writes nothing (its back-chain store is callee-owned stack scratch).
inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) {
  const int32_t mode = int32_t(Ld32(base, ctx.r4.u32 + 72));
  if (mode == 0 || mode == 1 || mode == 2) w.overflow = true;
}

}  // namespace me::hot::n_8264E178

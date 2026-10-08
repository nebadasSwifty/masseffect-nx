// sub_8264D1F8 - UE3 particle sprite emitter: fill the dynamic vertex buffer (4 vertices of 88 bytes per particle) and the
// Russian edition copy: only the guest constants differ from the English file (data addresses translated through the
// generated code of the RU function, which is instruction-for-instruction the same as the English one).
// index buffer (6 u16 indices per particle) from the particle data. Leaf function, returns r3 = 1.
//
//   r3 = emitter-render-data object `obj`:
//          +8 s32 particle count, +68 s32 limit (>= 0 and < count => count := limit), +16 u32 record stride,
//          +20 u32 record base, +24 u32 -> u16 index table (index -> record), +28 float[3] scale (x, y, z),
//          +52 u8 flag (0: the Y component of the position is a copy of X), +76 u32 offset of the UV record from a particle
//          record, +80 / +84 s32 UV divisors, +88 s32 mode (!= 0: float UVs, == 0: UVs quantised through fctiwz).
//   r4 = vertex buffer (352 bytes per particle), r5 = index buffer (12 bytes per particle, 0 = no indices),
//   r6 = 0 or pointer to a pointer to a table of 8-byte entries whose first word is the particle index (0 => particle i).
//   Constants: f9 = float at 0x820B9098, f10 = float at 0x820BCD44 (the texture corner values 0.0 / 1.0 in the real data).
//   f0 = f10 / (float)obj[80], f13 = f10 / (float)obj[84] (fdivs).
//
// The recompiled code ends every statement group with `ctx.fpscr.disableFlushMode()` before the first float load, which is the
// very first FPU instruction on every path: cleared once on entry (idempotent), nothing in the function enables it again. All
// float arithmetic is `float(double op double)` of float operands: for +, -, *, / of floats this equals the float operation
// (double rounding is innocuous since 53 >= 2 * 24 + 2) incl. NaN payload propagation; lfs/stfs pairs are float->double->float
// round trips that GCC folds into raw 32-bit copies (verified in the disassembly of the built ELF), so they are raw word copies.
// fctiwz is the generated expression: NaN -> INT_MIN, >= 2^31 -> INT_MAX, <= -2^31 -> INT_MIN, else truncation toward zero.
//
// Two implementations, selected per call / per particle:
//  * ParticleOrdered: the original statement order, every access an individual guest load/store: exact for ANY aliasing
//    between the output buffers and the data being read.
//  * ParticleFast: loads everything of one particle first, then stores (vertex image in registers). Exact whenever no byte the
//    particle reads lies inside a byte range the function writes (the vertex buffer [r4, r4 + 352 n) or the index buffer
//    [r5, r5 + 12 n)); the writes of the whole call are pairwise disjoint then (also checked), so their order is irrelevant. All
//    of that is checked at run time (once per call for the object / table, per particle for the record / UV record /
//    index-table element); anything else (overlap, 32-bit address wrap) takes ParticleOrdered.
// Callee-owned stack scratch (the spills below r1, the savegprlr area) is not reproduced; the original's LR value (the elided
// __savegprlr call stores ctx.lr) is not reproduced either. Volatile registers other than r3 are scratch (liveness.py: no caller
// reads one; the function has no direct call site, it is only called through a vtable by the ABI-conforming UE3 code).
#pragma once

#include "../me_hot_common.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace me::hot::n_8264C7C0 {

inline constexpr Cmp kCmp = kCmpRet;

namespace detail {

inline constexpr uint32_t kConstA = 0x820B9098u;           // f9
inline constexpr uint32_t kConstB = 0x820B9098u + 15532u;  // f10

// fctiwz + stfiwx + lwz: the integer the recompiled code ends up with (see the file comment)
inline int32_t FtoI(float v) {
  if (v != v) return INT32_MIN;
#if defined(__aarch64__)
  return vcvts_s32_f32(v);  // fcvtzs: saturating
#else
  if (v >= 2147483648.0f) return INT32_MAX;
  if (v <= -2147483648.0f) return INT32_MIN;
  return int32_t(v);
#endif
}
// Single-precision multiply / add / divide with the operand order of the PPC instruction (a first): when both operands are NaNs the
// result is the first one's, so the order is observable and must not be commuted by the compiler (inline asm on aarch64).
inline float Mul(float a, float b) {
#if defined(__aarch64__)
  float r;
  asm("fmul %s0, %s1, %s2" : "=w"(r) : "w"(a), "w"(b));
  return r;
#else
  return a * b;
#endif
}
inline float Add(float a, float b) {
#if defined(__aarch64__)
  float r;
  asm("fadd %s0, %s1, %s2" : "=w"(r) : "w"(a), "w"(b));
  return r;
#else
  return a + b;
#endif
}
inline float Div(float a, float b) {
#if defined(__aarch64__)
  float r;
  asm("fdiv %s0, %s1, %s2" : "=w"(r) : "w"(a), "w"(b));
  return r;
#else
  return a / b;
#endif
}
inline float ItoF(int32_t i) { return float(i); }
inline int32_t Inc(int32_t i) { return int32_t(uint32_t(i) + 1u); }  // addi + extsw

inline float RdF(const uint8_t* base, uint32_t a) { return LdF32(base, a); }
// raw (not byte swapped) word copies: the recompiled lwz / stw pair and the folded lfs / stfs pair
inline uint32_t RawW(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Raw(base, a), 4);
  return v;
}
inline void PutRawW(uint8_t* base, uint32_t a, uint32_t v) { std::memcpy(Raw(base, a), &v, 4); }
inline uint32_t FBits(float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  return v;
}
inline void PutF(uint8_t* base, uint32_t a, float f) { St32(base, a, FBits(f)); }

struct Env {
  uint32_t obj;
  uint32_t tab;      // r6
  bool use_tab;
  float f0, f13, f9, f10;
};

// ---------------------------------------------------------------------------------------------------------------------
// The index buffer part (identical in both implementations): 6 indices {4i, 4i+2, 4i+3, 4i, 4i+1, 4i+2} (low 16 bits).
inline void PutIndices(uint8_t* base, uint32_t at, uint32_t i) {
  const uint32_t b = 4u * i;
  St16(base, at + 0, uint16_t(b));
  St16(base, at + 2, uint16_t(b + 2));
  St16(base, at + 4, uint16_t(b + 3));
  St16(base, at + 6, uint16_t(b));
  St16(base, at + 8, uint16_t(b + 1));
  St16(base, at + 10, uint16_t(b + 2));
}

// ---------------------------------------------------------------------------------------------------------------------
// ParticleOrdered: the original statements in the original order.
inline void CopyOrdered(uint8_t* base, uint32_t o, uint32_t p, uint32_t X, uint32_t Y, uint32_t Z, uint32_t r9) {
  PutRawW(base, o + 0, RawW(base, p + 16));
  PutRawW(base, o + 4, RawW(base, p + 20));
  PutRawW(base, o + 8, RawW(base, p + 24));
  PutRawW(base, o + 12, RawW(base, p + 0));
  PutRawW(base, o + 16, RawW(base, p + 4));
  const uint32_t w8 = RawW(base, p + 8);
  St32(base, o + 24, X);
  St32(base, o + 28, Y);
  St32(base, o + 32, Z);
  PutRawW(base, o + 20, w8);
  PutRawW(base, o + 44, RawW(base, p + 44));
  PutRawW(base, o + 48, RawW(base, p + 96));
  PutRawW(base, o + 52, RawW(base, p + 100));
  PutRawW(base, o + 56, RawW(base, p + 104));
  PutRawW(base, o + 60, RawW(base, p + 108));
  PutRawW(base, o + 72, RawW(base, r9 + 0));
}

inline void ParticleOrdered(uint8_t* base, const Env& e, uint32_t i, uint32_t& out, uint32_t& idxp) {
  const uint32_t obj = e.obj;
  const float f0 = e.f0, f13 = e.f13;
  uint32_t t = e.use_tab ? Ld32(base, Ld32(base, e.tab) + 8u * i) : i;
  const uint32_t tbl = Ld32(base, obj + 24);
  t <<= 1;
  const uint32_t stride = Ld32(base, obj + 16);
  const float s0 = RdF(base, obj + 28);
  const uint32_t rec0 = Ld32(base, obj + 20);
  const float s1 = RdF(base, obj + 32);
  const uint32_t flag52 = Ld8(base, obj + 52);
  const float s2 = RdF(base, obj + 36);
  const uint32_t t16 = Ld16(base, t + tbl);
  const uint32_t p = uint32_t(int64_t(int32_t(t16)) * int64_t(int32_t(stride))) + rec0;
  const float px = RdF(base, p + 80);
  const float py = RdF(base, p + 84);
  const float X = Mul(s0, px);
  const float pz = RdF(base, p + 88);
  const float Y1 = Mul(py, s1);
  const float Z = Mul(s2, pz);
  const uint32_t Xb = FBits(X), Zb = FBits(Z);
  const uint32_t Yb = flag52 ? FBits(Y1) : Xb;
  const uint32_t r9 = Ld32(base, obj + 76) + p;  // original: lwz r29,16(r11) / lwz r9,76(r3) ... (loads only)
  uint32_t o = out;

  // ---- vertex 0
  CopyOrdered(base, o, p, Xb, Yb, Zb, r9);
  if (Ld32(base, obj + 88) != 0) {
    const float a = Mul(RdF(base, r9 + 4), f0);
    const float b = Mul(RdF(base, r9 + 8), f13);
    PutF(base, o + 36, a);
    PutF(base, o + 40, b);
    PutF(base, o + 64, a);
    PutF(base, o + 68, b);
  } else {
    PutF(base, o + 36, Mul(ItoF(FtoI(RdF(base, r9 + 4))), f0));
    PutF(base, o + 40, Mul(ItoF(FtoI(RdF(base, r9 + 8))), f13));
    PutF(base, o + 64, Mul(ItoF(FtoI(RdF(base, r9 + 12))), f0));
    PutF(base, o + 68, Mul(ItoF(FtoI(RdF(base, r9 + 16))), f13));
  }
  PutF(base, o + 80, e.f9);
  PutF(base, o + 84, e.f9);
  o += 88;

  // ---- vertex 1
  CopyOrdered(base, o, p, Xb, Yb, Zb, r9);
  if (Ld32(base, obj + 88) != 0) {
    const float u = RdF(base, r9 + 4);
    const float v = RdF(base, r9 + 8);
    const float a = Mul(u, f0);
    const float z = RdF(base, r9 + 16);
    const float c = Mul(Add(z, v), f13);
    PutF(base, o + 36, a);
    PutF(base, o + 64, a);
    PutF(base, o + 40, c);
    PutF(base, o + 68, c);
  } else {
    PutF(base, o + 36, Mul(ItoF(FtoI(RdF(base, r9 + 4))), f0));
    PutF(base, o + 40, Mul(ItoF(Inc(FtoI(RdF(base, r9 + 8)))), f13));
    PutF(base, o + 64, Mul(ItoF(FtoI(RdF(base, r9 + 12))), f0));
    PutF(base, o + 68, Mul(ItoF(Inc(FtoI(RdF(base, r9 + 16)))), f13));
  }
  PutF(base, o + 80, e.f9);
  PutF(base, o + 84, e.f10);
  o += 88;

  // ---- vertex 2
  CopyOrdered(base, o, p, Xb, Yb, Zb, r9);
  if (Ld32(base, obj + 88) != 0) {
    const float v = RdF(base, r9 + 8);
    const float z = RdF(base, r9 + 16);
    const float zv = Add(z, v);
    const float u = RdF(base, r9 + 4);
    const float w = RdF(base, r9 + 12);
    const float wu = Add(w, u);
    const float c = Mul(zv, f13);
    PutF(base, o + 40, c);
    const float d = Mul(wu, f0);
    PutF(base, o + 36, d);
    PutF(base, o + 64, d);
    PutF(base, o + 68, c);
  } else {
    PutF(base, o + 36, Mul(ItoF(Inc(FtoI(RdF(base, r9 + 4)))), f0));
    PutF(base, o + 40, Mul(ItoF(Inc(FtoI(RdF(base, r9 + 8)))), f13));
    PutF(base, o + 64, Mul(ItoF(Inc(FtoI(RdF(base, r9 + 12)))), f0));
    PutF(base, o + 68, Mul(ItoF(Inc(FtoI(RdF(base, r9 + 16)))), f13));
  }
  PutF(base, o + 80, e.f10);
  PutF(base, o + 84, e.f10);
  o += 88;

  // ---- vertex 3
  CopyOrdered(base, o, p, Xb, Yb, Zb, r9);
  if (Ld32(base, obj + 88) != 0) {
    const float w = RdF(base, r9 + 12);
    const float u = RdF(base, r9 + 4);
    const float wu = Add(w, u);
    const float v = RdF(base, r9 + 8);
    const float b = Mul(v, f13);
    PutF(base, o + 40, b);
    const float d = Mul(wu, f0);
    PutF(base, o + 36, d);
    PutF(base, o + 64, d);
    PutF(base, o + 68, b);
  } else {
    PutF(base, o + 36, Mul(ItoF(Inc(FtoI(RdF(base, r9 + 4)))), f0));
    PutF(base, o + 40, Mul(ItoF(FtoI(RdF(base, r9 + 8))), f13));
    PutF(base, o + 64, Mul(ItoF(Inc(FtoI(RdF(base, r9 + 12)))), f0));
    PutF(base, o + 68, Mul(ItoF(FtoI(RdF(base, r9 + 16))), f13));
  }
  PutF(base, o + 80, e.f10);
  PutF(base, o + 84, e.f9);
  out = o + 88;

  if (idxp != 0) {
    PutIndices(base, idxp, i);
    idxp += 12;
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// ParticleFast: call-level constants hoisted, the particle's reads before its writes.
struct Fast {
  uint32_t obj_tbl, obj_stride, obj_rec0, obj_off76;
  float s0, s1, s2;
  bool flag52, mode;          // obj[52] != 0, obj[88] != 0
  uint32_t tab_arr;           // *(r6)
  uint64_t o_lo, o_hi, i_lo, i_hi;  // write ranges (empty: lo == hi == 0)
};

inline bool Hit(const Fast& f, uint64_t a, uint64_t len) {
  return ((a < f.o_hi) & (a + len > f.o_lo)) | ((a < f.i_hi) & (a + len > f.i_lo));
}

// Returns false (nothing done) when this particle must take the ordered path.
inline bool ParticleFast(uint8_t* base, const Env& e, const Fast& f, uint32_t i, uint32_t& out, uint32_t& idxp) {
  const float f0 = e.f0, f13 = e.f13;
  const uint32_t t = e.use_tab ? Ld32(base, f.tab_arr + 8u * i) : i;
  const uint32_t a16 = (t << 1) + f.obj_tbl;
  if (Hit(f, a16, 2)) return false;
  const uint32_t t16 = Ld16(base, a16);
  const uint32_t p = t16 * f.obj_stride + f.obj_rec0;
  const uint32_t r9 = f.obj_off76 + p;
  if ((uint64_t(p) + 112 > 0x100000000ull) | (uint64_t(r9) + 20 > 0x100000000ull)) return false;
  if (Hit(f, p, 112) | Hit(f, r9, 20)) return false;

  const uint8_t* pp = Raw(base, p);
  const uint8_t* up = Raw(base, r9);
  uint32_t w16[3], w0[3], w96[4], w44, wuv0;
  std::memcpy(w16, pp + 16, 12);
  std::memcpy(w0, pp + 0, 12);
  std::memcpy(&w44, pp + 44, 4);
  std::memcpy(w96, pp + 96, 16);
  std::memcpy(&wuv0, up, 4);
  float px, py, pz;
  {
    uint32_t b[3];
    std::memcpy(b, pp + 80, 12);
    for (int k = 0; k < 3; ++k) b[k] = __builtin_bswap32(b[k]);
    std::memcpy(&px, &b[0], 4);
    std::memcpy(&py, &b[1], 4);
    std::memcpy(&pz, &b[2], 4);
  }
  const float X = Mul(f.s0, px);
  const float Y1 = Mul(py, f.s1);
  const float Z = Mul(f.s2, pz);
  uint32_t xyz[3];
  xyz[0] = __builtin_bswap32(FBits(X));
  xyz[1] = f.flag52 ? __builtin_bswap32(FBits(Y1)) : xyz[0];
  xyz[2] = __builtin_bswap32(FBits(Z));

  // UVs of the four corners: {+36, +40, +64, +68} per vertex (as host float bit patterns)
  float uv[4][4];
  if (f.mode) {
    const float u = RdF(base, r9 + 4), v = RdF(base, r9 + 8), w = RdF(base, r9 + 12), z = RdF(base, r9 + 16);
    const float a = Mul(u, f0);
    const float b = Mul(v, f13);
    const float c = Mul(Add(z, v), f13);
    const float d = Mul(Add(w, u), f0);
    uv[0][0] = a; uv[0][1] = b; uv[0][2] = a; uv[0][3] = b;
    uv[1][0] = a; uv[1][1] = c; uv[1][2] = a; uv[1][3] = c;
    uv[2][0] = d; uv[2][1] = c; uv[2][2] = d; uv[2][3] = c;
    uv[3][0] = d; uv[3][1] = b; uv[3][2] = d; uv[3][3] = b;
  } else {
    const int32_t i4 = FtoI(RdF(base, r9 + 4)), i8 = FtoI(RdF(base, r9 + 8));
    const int32_t i12 = FtoI(RdF(base, r9 + 12)), i16 = FtoI(RdF(base, r9 + 16));
    const float a0 = Mul(ItoF(i4), f0), a1 = ItoF(Inc(i4)) * f0;
    const float b0 = Mul(ItoF(i8), f13), b1 = ItoF(Inc(i8)) * f13;
    const float c0 = Mul(ItoF(i12), f0), c1 = ItoF(Inc(i12)) * f0;
    const float d0 = Mul(ItoF(i16), f13), d1 = ItoF(Inc(i16)) * f13;
    uv[0][0] = a0; uv[0][1] = b0; uv[0][2] = c0; uv[0][3] = d0;
    uv[1][0] = a0; uv[1][1] = b1; uv[1][2] = c0; uv[1][3] = d1;
    uv[2][0] = a1; uv[2][1] = b1; uv[2][2] = c1; uv[2][3] = d1;
    uv[3][0] = a1; uv[3][1] = b0; uv[3][2] = c1; uv[3][3] = d0;
  }
  // corner constants {+80, +84}: v0 (f9, f9), v1 (f9, f10), v2 (f10, f10), v3 (f10, f9)
  const uint32_t k9 = __builtin_bswap32(FBits(e.f9)), k10 = __builtin_bswap32(FBits(e.f10));
  const uint32_t kc[4][2] = {{k9, k9}, {k9, k10}, {k10, k10}, {k10, k9}};

  uint8_t* o = Raw(base, out);
  for (int v = 0; v < 4; ++v, o += 88) {
    uint32_t t[2];
    std::memcpy(o + 0, w16, 12);
    std::memcpy(o + 12, w0, 12);
    // +20 is w0[2] (the original stores p+8 at o+20 after X / Y / Z at +24..+36, disjoint addresses)
    std::memcpy(o + 24, xyz, 12);
    std::memcpy(o + 44, &w44, 4);
    std::memcpy(o + 48, w96, 16);
    std::memcpy(o + 72, &wuv0, 4);
    for (int k = 0; k < 4; ++k) {
      const uint32_t b = __builtin_bswap32(FBits(uv[v][k]));
      std::memcpy(o + (k < 2 ? 36 + 4 * k : 64 + 4 * (k - 2)), &b, 4);
    }
    t[0] = kc[v][0];
    t[1] = kc[v][1];
    std::memcpy(o + 80, t, 8);
  }
  out += 352;
  if (idxp != 0) {
    PutIndices(base, idxp, i);
    idxp += 12;
  }
  return true;
}

}  // namespace detail

// n = clamped particle count (<= 0: nothing to do)
inline int32_t Count(const uint8_t* base, uint32_t obj) {
  const int32_t lim = int32_t(Ld32(base, obj + 68));
  int32_t n = int32_t(Ld32(base, obj + 8));
  if (lim >= 0 && n > lim) n = lim;
  return n;
}

inline void Native(PPCContext& ctx, uint8_t* base) {
  using namespace detail;
  const uint32_t obj = ctx.r3.u32;
  ctx.fpscr.disableFlushMode();
  const int32_t n = Count(base, obj);
  const float c10 = RdF(base, kConstB);
  const float f0 = Div(c10, float(int32_t(Ld32(base, obj + 80))));
  const float f13 = Div(c10, float(int32_t(Ld32(base, obj + 84))));
  ctx.r3.s64 = 1;
  if (n <= 0) return;

  Env e;
  e.obj = obj;
  e.tab = ctx.r6.u32;
  e.use_tab = e.tab != 0;
  e.f0 = f0;
  e.f13 = f13;
  e.f9 = RdF(base, kConstA);
  e.f10 = c10;
  uint32_t out = ctx.r4.u32;
  uint32_t idxp = ctx.r5.u32;

  // ---- call-level conditions for the fast implementation
  Fast f;
  const uint64_t un = uint64_t(uint32_t(n));
  f.o_lo = out;
  f.o_hi = uint64_t(out) + 352u * un;
  f.i_lo = f.i_hi = 0;
  bool fast = f.o_hi <= 0x100000000ull;
  if (idxp != 0) {
    f.i_lo = idxp;
    f.i_hi = uint64_t(idxp) + 12u * un;
    fast &= f.i_hi <= 0x100000000ull;
    fast &= (f.o_hi <= f.i_lo) | (f.i_hi <= f.o_lo);  // the two buffers are disjoint
  }
  fast &= uint64_t(obj) + 96 <= 0x100000000ull;
  if (fast) {
    // the object fields (obj[0..96)) must not be written by this call
    fast &= !Hit(f, obj, 96);
    f.tab_arr = 0;
    if (e.use_tab) {
      fast &= uint64_t(e.tab) + 4 <= 0x100000000ull;
      fast &= !Hit(f, e.tab, 4);
      if (fast) {
        f.tab_arr = Ld32(base, e.tab);
        fast &= uint64_t(f.tab_arr) + 8u * un <= 0x100000000ull;
        fast &= !Hit(f, f.tab_arr, 8u * un);
      }
    }
  }
  if (fast) {
    f.obj_tbl = Ld32(base, obj + 24);
    f.obj_stride = Ld32(base, obj + 16);
    f.obj_rec0 = Ld32(base, obj + 20);
    f.obj_off76 = Ld32(base, obj + 76);
    f.s0 = RdF(base, obj + 28);
    f.s1 = RdF(base, obj + 32);
    f.s2 = RdF(base, obj + 36);
    f.flag52 = Ld8(base, obj + 52) != 0;
    f.mode = Ld32(base, obj + 88) != 0;
    for (int32_t i = 0; i < n; ++i) {
      if (!ParticleFast(base, e, f, uint32_t(i), out, idxp)) ParticleOrdered(base, e, uint32_t(i), out, idxp);
    }
  } else {
    for (int32_t i = 0; i < n; ++i) ParticleOrdered(base, e, uint32_t(i), out, idxp);
  }
}

inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) {
  const uint32_t obj = ctx.r3.u32;
  const int32_t n = Count(base, obj);
  if (n <= 0) return;
  const uint64_t un = uint32_t(n);
  if (352u * un > me::hot::Writes::kMaxBytes || uint64_t(ctx.r4.u32) + 352u * un > 0x100000000ull) {
    w.overflow = true;
    return;
  }
  w.Add(ctx.r4.u32, uint32_t(352u * un));
  if (ctx.r5.u32 != 0) {
    if (uint64_t(ctx.r5.u32) + 12u * un > 0x100000000ull) {
      w.overflow = true;
      return;
    }
    w.Add(ctx.r5.u32, uint32_t(12u * un));
  }
}

}  // namespace me::hot::n_8264C7C0

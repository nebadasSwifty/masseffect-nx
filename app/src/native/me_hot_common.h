// Mass Effect - hot guest function replacements: shared definitions (light header, no SDK runtime).
//
// A "hot hook" replaces one recompiled guest function (sub_X) by a hand-written C++/NEON version with the same
// observable behaviour. Each function lives in src/native/hot/n_<addr>.h as
//
//   namespace me::hot::n_<addr> {
//     inline constexpr Cmp kCmp = ...;                                   // volatile regs that must match
//     inline void Native(PPCContext& ctx, uint8_t* base);                // the replacement
//     inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w);  // guest ranges it writes
//   }
//
// and is registered in me_hot_guest.cpp (ME_HOT_HOOK: cvar, self-check guard, permanent fallback). The same headers
// are compiled by the macOS differential fuzzer (tests/hot_fuzz) against the recompiled original.
//
// What "exact" means here: memory effects (all guest ranges the original writes, except the
// callee-owned stack scratch below r1 and the caller's parameter save area), the return registers (r3, f1, v1, ...),
// the non-volatile registers (untouched) and the FPSCR flush-mode state. Volatile scratch registers the original leaves
// behind (r0, r4-r12, f0-f13, v0-v13) are NOT reproduced unless a caller reads them: tests/hot_fuzz/liveness.py checks
// every direct call site of a hooked function for reads of volatile registers after the call.
#pragma once

#include <cstdint>
#include <cstring>

#include <rex/platform.h>
#include <rex/ppc/context.h>

namespace me::hot {

// Guest memory accessors with the generated code's semantics (big-endian; the +0x1000 physical-range offset of the
// Windows / Apple-silicon hosts, none on the Switch; no volatile, as tools/pch_no_volatile.py).
inline uint32_t PhysOff(uint32_t a) {
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
  return a >= 0xE0000000u ? 0x1000u : 0u;
#else
  (void)a;
  return 0u;
#endif
}
inline uint8_t* Raw(uint8_t* base, uint32_t a) { return base + a + PhysOff(a); }
inline const uint8_t* Raw(const uint8_t* base, uint32_t a) { return base + a + PhysOff(a); }
inline uint8_t Ld8(const uint8_t* base, uint32_t a) { return *Raw(base, a); }
inline uint16_t Ld16(const uint8_t* base, uint32_t a) {
  uint16_t v;
  std::memcpy(&v, Raw(base, a), 2);
  return __builtin_bswap16(v);
}
inline uint32_t Ld32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Raw(base, a), 4);
  return __builtin_bswap32(v);
}
inline uint64_t Ld64(const uint8_t* base, uint32_t a) {
  uint64_t v;
  std::memcpy(&v, Raw(base, a), 8);
  return __builtin_bswap64(v);
}
inline float LdF32(const uint8_t* base, uint32_t a) {
  const uint32_t v = Ld32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline void St8(uint8_t* base, uint32_t a, uint8_t v) { *Raw(base, a) = v; }
inline void St16(uint8_t* base, uint32_t a, uint16_t v) {
  v = __builtin_bswap16(v);
  std::memcpy(Raw(base, a), &v, 2);
}
inline void St32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(Raw(base, a), &v, 4);
}
inline void St64(uint8_t* base, uint32_t a, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(Raw(base, a), &v, 8);
}
inline void StF32(uint8_t* base, uint32_t a, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  St32(base, a, v);
}

struct Region {
  uint32_t addr;
  uint32_t len;
};

// Guest ranges a replacement may write, computed from the inputs BEFORE the call. The guard snapshots them, runs the
// replacement, rolls the memory back, runs the original and compares. `overflow` = too many/too large to verify.
struct Writes {
  static constexpr int kMax = 8;
  static constexpr uint32_t kMaxBytes = 1u << 20;
  Region r[kMax];
  int n = 0;
  bool overflow = false;
  void Add(uint32_t addr, uint32_t len) {
    if (!len) return;
    if (n >= kMax || len > kMaxBytes) {
      overflow = true;
      return;
    }
    r[n++] = {addr, len};
  }
};

// Volatile registers that must be identical after the replacement (bit n = register n). r1, r13-r31, f14-f31,
// v14-v31 and the FPSCR are always compared.
struct Cmp {
  uint32_t gpr;
  uint32_t fpr;
  uint32_t vr;
};
inline constexpr uint32_t R(int n) { return 1u << n; }
// r3 / f1 / v1: the return registers.
inline constexpr Cmp kCmpRet = {R(3), R(1), R(1)};

}  // namespace me::hot

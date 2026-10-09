// Differential fuzzer for the hot guest hooks (app/src/native/hot/n_<addr>.h) against the recompiled originals.
// Build and run: python3 tests/hot_fuzz/build.py [--iters N] [--seed S] [case ...]   (macOS arm64, clang++)
//
// A case supplies: the extracted original (__imp__sub_X, compiled from app/generated), the native Native/Writes
// pair and kCmp, a memory window and a generator that fills the window and the registers. The harness runs the
// original on arena A and the native version on arena B (two separate 4 GB reservations holding identical window
// contents; the guest only ever sees 32-bit guest addresses, so the bases may differ), then compares
//   * the whole window (every byte),
//   * the registers selected by kCmp plus the always-compared set (r1, r13-r31, f14-f31, v14-v31, FPSCR),
//   * that every byte the original or the native version changed lies inside the ranges Writes() declares
//     (the guard only snapshots those),
//   * the hardware FPCR after the call.
// The volatile registers kCmp does not select are counted per register as information ("scratch differs").
#pragma once

#include <sys/mman.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include "../../app/src/native/me_hot_common.h"

namespace fuzz {

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
  uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s * 0x2545F4914F6CDD1Dull;
  }
  uint32_t u32() { return uint32_t(next() >> 16); }
  uint64_t u64() { return next(); }
  uint32_t below(uint32_t n) { return n ? uint32_t((next() >> 20) % n) : 0; }
  uint32_t range(uint32_t a, uint32_t b) { return a + below(b - a + 1); }
  bool chance(double p) { return double(next() >> 11) * (1.0 / 9007199254740992.0) < p; }
  // random float bit pattern: mostly ordinary, sometimes zero/denormal/inf/NaN
  uint32_t float_bits() {
    switch (below(12)) {
      case 0: return 0;
      case 1: return 0x80000000u;
      case 2: return below(1u << 23);                 // denormal
      case 3: return 0x7F800000u | (u32() & 0x807FFFFFu);  // inf / NaN (incl. signalling)
      case 4: return u32();
      default: {
        float f = float(int32_t(u32() % 2000001) - 1000000) * (1.0f / 64.0f);
        uint32_t b;
        std::memcpy(&b, &f, 4);
        return b;
      }
    }
  }
};

struct Arena {
  uint8_t* base = nullptr;
  Arena() {
    void* p = mmap(nullptr, 0x100000000ull + 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
      std::perror("mmap");
      std::exit(2);
    }
    base = static_cast<uint8_t*>(p);
  }
};

// Natives that return bool (false = declined, run the original): the harness runs them through this adapter.
template <auto Native, auto Orig>
inline void NativeAdapter(PPCContext& c, uint8_t* base) {
  if constexpr (std::is_void_v<decltype(Native(c, base))>) {
    Native(c, base);
  } else if (!Native(c, base)) {
    Orig(c, base);
  }
}

using Gen = void (*)(Rng&, uint8_t* base, PPCContext&);

struct Case {
  const char* name;
  PPCFunc* orig;
  PPCFunc* nat;
  void (*writes)(const PPCContext&, const uint8_t*, me::hot::Writes&);
  me::hot::Cmp cmp;
  uint32_t win_addr, win_len;  // guest window the generator may touch (compared as a whole)
  Gen gen;
  // Optional: the write set the console guard computes (production settings of the native's switches), when the case
  // widens `writes` for the fuzzer (e.g. stubs for guest calls that the console guard never lets the native make). The
  // guard replay in main.cpp uses it; nullptr = `writes`.
  void (*guard_writes)(const PPCContext&, const uint8_t*, me::hot::Writes&) = nullptr;
};

inline std::vector<Case>& Registry() {
  static std::vector<Case> r;
  return r;
}
struct Registrar {
  explicit Registrar(const Case& c) { Registry().push_back(c); }
};

// Guest stack pointer inside the window for every case: the top 0x1000 bytes of the window.
inline uint32_t StackTop(const Case& c) { return c.win_addr + c.win_len - 0x100; }

}  // namespace fuzz

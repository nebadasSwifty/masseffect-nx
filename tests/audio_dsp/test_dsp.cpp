// Host differential test (macOS arm64, same ISA/FPCR semantics as the Switch): runs the recompiled originals
// (extracted verbatim from app/generated/default by run.sh) and the native replacements
// (src/native/me_audio_dsp.h) on identical random guest memory and compares every byte plus FPCR state.
//
//   tests/audio_dsp/run.sh [iterations]

#include "masseffect_pch.h"

#include <sys/mman.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "../../app/src/native/me_audio_dsp.h"

using me::audio_dsp::GP;

// ---- stubs for what the originals call ------------------------------------------------------------------
static uint32_t g_stub_buffer = 0;
static uint32_t g_stub_channels = 0;

extern "C" void __imp__sub_82AA65E8(PPCContext& ctx, uint8_t* base) {
  const uint32_t out = ctx.r4.u32;
  GP(base, out)[1] = uint8_t(g_stub_channels);
  const uint32_t be = __builtin_bswap32(g_stub_buffer);
  std::memcpy(GP(base, out + 8), &be, 4);
}
REX_EXTERN(__imp__sub_82AC4A50);
extern "C" void __imp__sub_82AC4A50(PPCContext& ctx, uint8_t* base) {  // guest memset(r3, r4, r5)
  std::memset(GP(base, ctx.r3.u32), int(ctx.r4.u32 & 0xFF), ctx.r5.u32);
}

// ---- the recompiled originals (extracted by run.sh) ------------------------------------------------------
#include "ref_82AA53C0.inc"
#include "ref_82B4D580.inc"

namespace {

std::mt19937_64 rng(12345);

uint32_t R(uint32_t n) { return uint32_t(rng() % n); }

float RandomFloat() {
  uint32_t bits;
  switch (R(12)) {
    case 0: bits = R(0x00800000);                           // denormal
            bits |= (R(2) << 31); break;
    case 1: bits = 0x7F800000 | (R(2) << 31); break;        // inf
    case 2: bits = 0x7FC00000 | R(0x400000) | (R(2) << 31); break;  // NaN with payload
    case 3: bits = R(2) << 31; break;                       // +-0
    case 4: bits = 0x7F7FFFFF - R(1000); break;             // huge
    default: {
      const float mag = std::ldexp(float(R(1 << 24)) / float(1 << 24), int(R(40)) - 30);
      const float v = (R(2) ? -mag : mag);
      std::memcpy(&bits, &v, 4);
    }
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
float SmallFloat() {  // well-behaved audio-range value
  return (float(R(2000001)) / 1000000.0f - 1.0f) * (R(8) == 0 ? 1.7f : 0.9f);
}

void StoreF(uint8_t* base, uint32_t a, float f) { me::audio_dsp::StoreBEf(base, a, f); }
float LoadF(uint8_t* base, uint32_t a) { return me::audio_dsp::LoadBEf(base, a); }

struct Sim {
  uint8_t* base;
  explicit Sim() {
    void* p = mmap(nullptr, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
      std::perror("mmap");
      std::exit(2);
    }
    base = static_cast<uint8_t*>(p);
  }
};

void SetFz(PPCContext& ctx, bool on) {
  if (on) ctx.fpscr.enableFlushModeUnconditional();
  else ctx.fpscr.disableFlushModeUnconditional();
}

struct Result {
  uint32_t csr_cached;
  uint32_t csr_hw;
  uint64_t r1, lr;
};

bool Same(const Result& a, const Result& b) { return a.csr_cached == b.csr_cached && a.csr_hw == b.csr_hw && a.r1 == b.r1 && a.lr == b.lr; }

// Bytes equal, except that two NaNs with different payload/sign count as equal: the compiler is free to
// commute the operands of a commutative FP op (LLVM and GCC both do), which picks a different operand's NaN
// payload. Real audio never carries NaNs (the output stage maps non-finite samples to 0).
// Returns the offset of the first real difference, or ~0u.
uint32_t FirstDiff(const uint8_t* a, const uint8_t* b, uint32_t bytes) {
  for (uint32_t o = 0; o + 4 <= bytes; o += 4) {
    if (std::memcmp(a + o, b + o, 4) == 0) continue;
    uint32_t x, y;
    std::memcpy(&x, a + o, 4);
    std::memcpy(&y, b + o, 4);
    x = __builtin_bswap32(x);
    y = __builtin_bswap32(y);
    const bool nan_x = (x & 0x7F800000) == 0x7F800000 && (x & 0x007FFFFF);
    const bool nan_y = (y & 0x7F800000) == 0x7F800000 && (y & 0x007FFFFF);
    if (nan_x && nan_y) continue;
    return o;
  }
  return ~0u;
}

int g_fail = 0;
void Fail(const char* what, int iter, const char* detail) {
  if (g_fail < 12) std::printf("MISMATCH %s iter %d: %s\n", what, iter, detail);
  ++g_fail;
}

// Fills [addr, addr+bytes) of both memories with the same random float words.
void FillBoth(Sim& a, Sim& b, uint32_t addr, uint32_t bytes, bool wild) {
  for (uint32_t o = 0; o + 4 <= bytes; o += 4) {
    const float f = wild ? RandomFloat() : SmallFloat();
    StoreF(a.base, addr + o, f);
    StoreF(b.base, addr + o, f);
  }
}

// ---------------------------------------------------------------------------------------------------------
void TestMix(Sim& A, Sim& B, int iterations) {
  PPCContext ctx{};
  for (int it = 0; it < iterations; ++it) {
    const bool wild = R(3) == 0;
    const uint32_t n = R(5) == 0 ? R(40) : (R(2) ? 256 : R(700));
    const uint32_t bytes = ((((n >> 2) ? ((n >> 2) - 1) / 4 + 1 : 0)) * 64) + 128;
    const uint32_t dst = 0x100000 + R(0x1000) * 4 + (R(2) ? 0 : R(16));
    uint32_t src;
    switch (R(4)) {
      case 0: src = dst; break;                                // in place
      case 1: src = dst + 16 * R(8); break;                    // overlapping
      default: src = 0x200000 + R(0x1000) * 4 + (R(2) ? 0 : R(16));
    }
    const uint32_t gain_a = 0x300000 + 16 * R(64), step_a = 0x310000 + 16 * R(64);
    FillBoth(A, B, 0x100000, 0x20000, wild);
    FillBoth(A, B, 0x200000, 0x20000, wild);
    FillBoth(A, B, gain_a, 16, wild);
    FillBoth(A, B, step_a, 16, R(2) == 0);
    const bool fz_before = R(2);
    Result ra, rb;
    {
      PPCContext c = ctx;
      SetFz(c, fz_before);
      c.r3.u64 = dst; c.r4.u64 = src; c.r5.u64 = gain_a + (R(2) ? 0 : 0); c.r6.u64 = step_a; c.r7.u64 = n;
      c.r1.u64 = 0x800000; c.lr = 0x82AA56D0;
      __imp__sub_82AA53C0(c, A.base);
      ra = {c.fpscr.csr, c.fpscr.getcsr(), c.r1.u64, c.lr};
    }
    {
      PPCContext c = ctx;
      SetFz(c, fz_before);
      c.r3.u64 = dst; c.r4.u64 = src; c.r5.u64 = gain_a; c.r6.u64 = step_a; c.r7.u64 = n;
      c.r1.u64 = 0x800000; c.lr = 0x82AA56D0;
      me::audio_dsp::Native82AA53C0(c, B.base);
      rb = {c.fpscr.csr, c.fpscr.getcsr(), c.r1.u64, c.lr};
    }
    for (uint32_t base_addr : {0x100000u, 0x200000u}) {
      const uint32_t off = FirstDiff(A.base + base_addr, B.base + base_addr, 0x20000);
      if (off != ~0u) {
        uint32_t wa, wb;
        std::memcpy(&wa, A.base + base_addr + off, 4);
        std::memcpy(&wb, B.base + base_addr + off, 4);
        char d[260];
        std::snprintf(d, sizeof d, "memory differs (n=%u dst=%08X src=%08X fz=%d wild=%d) first at %08X ref %08X native %08X", n, dst, src, int(fz_before), int(wild), base_addr + off, __builtin_bswap32(wa), __builtin_bswap32(wb));
        Fail("82AA53C0", it, d);
        break;
      }
    }
    if (!Same(ra, rb)) {
      char d[200];
      std::snprintf(d, sizeof d, "state differs csr %08X/%08X hw %08X/%08X (fz_before=%d)", ra.csr_cached, rb.csr_cached, ra.csr_hw, rb.csr_hw, int(fz_before));
      Fail("82AA53C0 state", it, d);
    }
  }
}

// ---------------------------------------------------------------------------------------------------------
void TestSmoother(Sim& A, Sim& B, int iterations) {
  PPCContext ctx{};
  for (int it = 0; it < iterations; ++it) {
    const bool wild = R(4) == 0;
    const uint32_t channels = R(6) == 0 ? R(3) : 1 + R(8);
    const uint32_t self = 0x400000 + 16 * R(16);
    const uint32_t buffer = 0x500000 + (R(4) ? 0 : 4 * R(8));
    g_stub_channels = channels;
    g_stub_buffer = buffer;
    const uint32_t arg = 0x600000;
    // object: A @12, B @16, state @20..
    for (auto* S : {&A, &B}) std::memset(S->base + self, 0, 128);
    const int special = int(R(10));
    float a_val = special == 0 ? 0.0f : (special == 1 ? RandomFloat() : 0.9f + float(R(1000)) / 10000.0f);
    float b_val = special == 2 ? 0.0f : (special == 3 ? RandomFloat() : SmallFloat() * 0.5f + 0.5f);
    const float c_lo = R(5) ? -1.0f : RandomFloat();
    const float c_hi = R(5) ? 1.0f : RandomFloat();
    const float zero = R(8) ? 0.0f : RandomFloat();
    if (special == 4) { a_val = zero; }
    if (special == 5) { b_val = zero; }
    for (auto* S : {&A, &B}) {
      StoreF(S->base, self + 12, a_val);
      StoreF(S->base, self + 16, b_val);
      StoreF(S->base, 0x821BE0F0u, c_lo);
      StoreF(S->base, 0x82002978u, c_hi);
      StoreF(S->base, 0x82002974u, zero);
    }
    for (uint32_t c = 0; c < 8; ++c) {
      const float st = wild ? RandomFloat() : SmallFloat();
      StoreF(A.base, self + 20 + 4 * c, st);
      StoreF(B.base, self + 20 + 4 * c, st);
    }
    FillBoth(A, B, 0x500000, 0x4000, wild);
    const bool fz_before = R(2);
    Result ra, rb;
    uint64_t r3a = 0, r3b = 0;
    {
      PPCContext c = ctx;
      SetFz(c, fz_before);
      c.r3.u64 = self; c.r4.u64 = arg; c.r1.u64 = 0x800000; c.lr = 0x82AA1234;
      __imp__sub_82B4D580(c, A.base);
      ra = {c.fpscr.csr, c.fpscr.getcsr(), c.r1.u64, c.lr};
      r3a = c.r3.u64;
    }
    {
      PPCContext c = ctx;
      SetFz(c, fz_before);
      c.r3.u64 = self; c.r4.u64 = arg; c.r1.u64 = 0x800000; c.lr = 0x82AA1234;
      me::audio_dsp::Native82B4D580(c, B.base);
      rb = {c.fpscr.csr, c.fpscr.getcsr(), c.r1.u64, c.lr};
      r3b = c.r3.u64;
    }
    // Compare the object (state), the buffer and the whole scratch above the stack the original uses.
    const uint32_t regions[][2] = {{self, 128}, {0x500000, 0x4000}};
    for (auto& rg : regions) {
      if (FirstDiff(A.base + rg[0], B.base + rg[0], rg[1]) != ~0u) {
        char d[220];
        std::snprintf(d, sizeof d, "memory differs at %08X (channels=%u special=%d fz=%d wild=%d a=%g b=%g)", rg[0], channels, special, int(fz_before), int(wild), a_val, b_val);
        Fail("82B4D580", it, d);
        break;
      }
    }
    if (!Same(ra, rb) || r3a != r3b) {
      char d[220];
      std::snprintf(d, sizeof d, "state differs csr %08X/%08X hw %08X/%08X r1 %llX/%llX lr %llX/%llX r3 %llX/%llX", ra.csr_cached, rb.csr_cached, ra.csr_hw, rb.csr_hw, (unsigned long long)ra.r1, (unsigned long long)rb.r1, (unsigned long long)ra.lr, (unsigned long long)rb.lr, (unsigned long long)r3a, (unsigned long long)r3b);
      Fail("82B4D580 state", it, d);
    }
  }
}

}  // namespace

// Timing of typical calls (the mixer's: 256 floats per channel, 6 channels), recompiled vs native.
void Bench(Sim& A) {
  using clk = std::chrono::steady_clock;
  PPCContext ctx{};
  ctx.fpscr.InitHost();
  FillBoth(A, A, 0x100000, 0x20000, false);
  FillBoth(A, A, 0x200000, 0x20000, false);
  FillBoth(A, A, 0x300000, 32, false);
  StoreF(A.base, 0x400000 + 12, 0.97f);
  StoreF(A.base, 0x400000 + 16, 0.5f);
  StoreF(A.base, 0x821BE0F0u, -1.0f);
  StoreF(A.base, 0x82002978u, 1.0f);
  StoreF(A.base, 0x82002974u, 0.0f);
  g_stub_channels = 6;
  g_stub_buffer = 0x500000;
  const int N = 20000;
  auto run = [&](auto&& f) {
    const auto t0 = clk::now();
    for (int i = 0; i < N; ++i) f();
    return std::chrono::duration<double, std::micro>(clk::now() - t0).count() / N;
  };
  auto mix_args = [&](PPCContext& c) { c.r3.u64 = 0x100000; c.r4.u64 = 0x200000; c.r5.u64 = 0x300000; c.r6.u64 = 0x300010; c.r7.u64 = 256; c.r1.u64 = 0x800000; };
  auto sm_args = [&](PPCContext& c) { c.r3.u64 = 0x400000; c.r4.u64 = 0x600000; c.r1.u64 = 0x800000; };
  PPCContext c = ctx;
  const double mix_ref = run([&] { mix_args(c); __imp__sub_82AA53C0(c, A.base); });
  const double mix_nat = run([&] { mix_args(c); me::audio_dsp::Native82AA53C0(c, A.base); });
  const double sm_ref = run([&] { sm_args(c); __imp__sub_82B4D580(c, A.base); });
  const double sm_nat = run([&] { sm_args(c); me::audio_dsp::Native82B4D580(c, A.base); });
  std::printf("bench (host, us/call): 82AA53C0 256 floats: recompiled %.2f native %.2f (x%.1f) | 82B4D580 6 channels: recompiled %.2f native %.2f (x%.1f)\n",
              mix_ref, mix_nat, mix_ref / mix_nat, sm_ref, sm_nat, sm_ref / sm_nat);
}

int main(int argc, char** argv) {
  const int iterations = argc > 1 ? std::atoi(argv[1]) : 20000;
  Sim A, B;
  {
    PPCContext ctx{};
    ctx.fpscr.InitHost();
  }
  if (argc > 2 && std::strcmp(argv[2], "bench") == 0) {
    Bench(A);
    return 0;
  }
  TestMix(A, B, iterations);
  std::printf("82AA53C0: %d iterations, failures so far %d\n", iterations, g_fail);
  TestSmoother(A, B, iterations);
  std::printf("82B4D580: %d iterations, failures total %d\n", iterations, g_fail);
  return g_fail ? 1 : 0;
}

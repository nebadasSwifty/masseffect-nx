// ORIG: sub_82AAFB90 sub_82ACBAF0
// NATIVE: n_82AAFB90.h
// XAudio voice resampler (s16 mono -> float, linear interpolation, 32.32 position, volume ramp). See n_82AAFB90.h.
// The .rdata constants (outside the window) are written into both arenas before each run: mostly the real values
// (K1 = 2^32, C_B = 0,1,2,3, C_A = 4,5,6,7, KF = 8, K2 = 2^-32), sometimes random bit patterns.
// Window: [win, win + 0x40000): state at +0x100, source at +0x1000.., destination at +0x20000.., stack at the top.
// Covered: typical calls (256 outputs, rate 0.25..4), short/odd/zero/negative counts, odd source addresses (the lvlx
// halfword split at a 16-byte boundary), unaligned destinations, wild floats (NaN/inf/denormal/huge) in the gains, the
// rate and the position, consumed/written clamps on both sides.
extern "C" void __imp__sub_82AAFB90(PPCContext&, uint8_t*);
namespace {
namespace c82AAFB90 {
constexpr uint32_t kWin = 0x40000000, kLen = 0x40000;
uint64_t g_k1, g_k2;
uint32_t g_kf, g_ca[4], g_cb[4];
uint64_t n_calls = 0, n_odd = 0, n_wild = 0, n_short = 0, n_declined = 0, n_restack = 0, n_console = 0;

void Env(uint8_t* base) {
  namespace n = me::hot::n_82AAFB90;
  me::hot::St64(base, n::kK1, g_k1);
  me::hot::St64(base, n::kK2, g_k2);
  me::hot::St32(base, n::kKF, g_kf);
  for (int i = 0; i < 4; ++i) {
    me::hot::St32(base, n::kCA + 4 * i, g_ca[i]);
    me::hot::St32(base, n::kCB + 4 * i, g_cb[i]);
  }
}
uint32_t FB(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}
// Random float bits without NaN: with two different NaN operands an IEEE add/mul returns the first one's payload and
// the compiler may commute the operands of the recompiled code's commutative ops, so only one NaN source per call is
// generated (the result is a NaN either way; real audio carries none).
uint32_t NoNaN(fuzz::Rng& r) {
  uint32_t b = r.float_bits();
  if ((b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu)) b &= 0xFF800000u;  // -> inf
  return b;
}
uint64_t DB(double d) {
  uint64_t b;
  std::memcpy(&b, &d, 8);
  return b;
}
void orig_w(PPCContext& ctx, uint8_t* base) {
  Env(base);
  __imp__sub_82AAFB90(ctx, base);
}
void nat_w(PPCContext& ctx, uint8_t* base) {
  Env(base);
  if (!me::hot::n_82AAFB90::Native(ctx, base)) {
    ++n_declined;
    __imp__sub_82AAFB90(ctx, base);
  }
}
void gen(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  ++n_calls;
  const bool wild_consts = r.chance(0.05);
  g_k1 = wild_consts && r.chance(0.5) ? r.u64() : DB(4294967296.0);
  g_k2 = wild_consts && r.chance(0.5) ? r.u64() : DB(1.0 / 4294967296.0);
  g_kf = wild_consts && r.chance(0.5) ? NoNaN(r) : FB(8.0f);
  for (int i = 0; i < 4; ++i) {
    g_cb[i] = wild_consts && r.chance(0.3) ? NoNaN(r) : FB(float(i));
    g_ca[i] = wild_consts && r.chance(0.3) ? NoNaN(r) : FB(float(4 + i));
  }
  const uint32_t st = kWin + 0x100 + 4 * r.below(16);
  uint32_t src = kWin + 0x1000 + 2 * r.below(0x400);
  if (r.chance(0.15)) {
    src |= 1;  // odd: halfwords straddle 16-byte blocks every 8 samples
    ++n_odd;
  }
  uint32_t dst = kWin + 0x20000 + 16 * r.below(0x100);
  if (r.chance(0.2)) dst += r.below(16);
  int32_t count;
  switch (r.below(10)) {
    case 0: count = int32_t(r.below(17)); ++n_short; break;
    case 1: count = -int32_t(r.below(100)); ++n_short; break;
    case 2: count = int32_t(r.below(1200)); break;
    default: count = 256;
  }
  const bool wild = r.chance(0.15);
  if (wild) ++n_wild;
  // rate (samples per output) and position fraction: keep the read span inside the window for ordinary inputs
  const float rate = wild && r.chance(0.3) ? [&] { float f; uint32_t b = r.float_bits(); std::memcpy(&f, &b, 4); return f; }()
                                           : float(0.05 + double(r.below(1000)) / 1000.0 * (r.chance(0.2) ? 8.0 : 3.9));
  const float frac = wild && r.chance(0.3) ? [&] { float f; uint32_t b = r.float_bits(); std::memcpy(&f, &b, 4); return f; }()
                                           : float(double(r.below(1u << 20)) / double(1u << 20)) * (r.chance(0.1) ? 3.0f : 1.0f);
  const int nan_gain = r.below(2);  // the only gain that may be a NaN
  auto gain = [&](int which) -> uint32_t {
    if (wild && r.chance(0.4)) return which == nan_gain ? r.float_bits() : NoNaN(r);
    return FB(float(r.below(20001)) / 10000.0f);
  };
  me::hot::St32(base, st + 0, src - 2 * r.below(64));                 // source start
  me::hot::St32(base, st + 4, r.below(4000));                         // length
  me::hot::St8(base, st + 13, uint8_t(r.chance(0.8) ? 1 : 1 + r.below(4)));
  me::hot::St32(base, st + 20, dst - 4 * r.below(300));               // destination start
  me::hot::St32(base, st + 24, r.below(1500));                        // capacity
  me::hot::St32(base, st + 36, gain(0));
  me::hot::St32(base, st + 40, r.chance(0.2) ? me::hot::Ld32(base, st + 36) : gain(1));
  me::hot::St32(base, st + 44, FB(rate));
  me::hot::St32(base, st + 48, FB(frac));
  // source samples: audio-like or random 16-bit
  for (uint32_t a = kWin + 0x1000; a < kWin + 0x1F000; a += 2) {
    const uint16_t v = r.chance(0.5) ? uint16_t(r.u32()) : uint16_t(int16_t(int32_t(r.below(2001)) - 1000));
    me::hot::St16(base, a, v);
  }
  // The callee-owned stack frame (below r1) holds the original's spills: a source read there would see them in the
  // original and the window garbage in the native run. Real sources never alias the callee's frame: keep the simulated
  // read addresses out of [sp - 0x400, sp + 0x100) (else fall back to an ordinary rate and position).
  {
    namespace d = me::hot::n_82AAFB90::detail;
    double k1;
    std::memcpy(&k1, &g_k1, 8);
    auto reaches_stack = [&]() {
      const uint32_t sp = kWin + kLen - 0x100;
      float f44, f48;
      uint32_t b44 = me::hot::Ld32(base, st + 44), b48 = me::hot::Ld32(base, st + 48);
      std::memcpy(&f44, &b44, 4);
      std::memcpy(&f48, &b48, 4);
      uint64_t pos = uint64_t(d::Fctidz(double(f48) * k1));
      const uint64_t rt = uint64_t(d::Fctidz(double(f44) * k1));
      uint32_t s = src;
      int64_t left = count;
      do {
        for (int k = 0; k < 8; ++k) {
          const uint32_t a = s + (uint32_t(pos >> 32) << 1);
          pos += rt;
          if (uint64_t(a) < uint64_t(sp) + 0x100 && uint64_t(a) + 4 > uint64_t(sp) - 0x400) return true;
        }
        s += uint32_t(pos >> 32) << 1;
        pos &= 0xFFFFFFFFull;
        left -= 8;
      } while (int32_t(left) > 0);
      return false;
    };
    if (reaches_stack()) {
      ++n_restack;
      me::hot::St32(base, st + 44, FB(1.0f));
      me::hot::St32(base, st + 48, FB(0.5f));
      if (g_k1 != DB(4294967296.0)) g_k1 = DB(4294967296.0);
    }
  }
  ctx.r3.u64 = (r.chance(0.2) ? (uint64_t(r.u32()) << 32) : 0) | src;  // upper half: garbage the original ignores
  ctx.r5.u64 = dst;
  ctx.r6.u64 = (r.chance(0.2) ? (uint64_t(r.u32()) << 32) : 0) | uint32_t(count);
  ctx.r7.u64 = st;
  // Console regression (2026-10-09, RU spacewalk, sub_82B2D780): 256 outputs into a 16-byte aligned buffer, r9 = r5,
  // r4 = 0x6500, r10 = 0x64FF. The native result was right; the guard compared overlapping declared ranges wrongly
  // (the guard replay in main.cpp now runs the console guard's algorithm on every iteration).
  if (r.chance(0.1)) {
    ++n_console;
    const uint32_t d = kWin + 0x20000 + 16 * r.below(0x80);
    ctx.r5.u64 = d;
    ctx.r6.u64 = 0x100;
    ctx.r9.u64 = d;
    ctx.r4.u64 = 0x6500;
    ctx.r10.u64 = 0x64FF;
    me::hot::St32(base, st + 20, d);
    me::hot::St32(base, st + 24, 256);
  }
  Env(base);
}
struct Stat {
  ~Stat() {
    std::printf("   [82AAFB90 coverage: calls=%llu odd sources=%llu wild=%llu short/negative counts=%llu declined=%llu reads kept off the stack frame=%llu console regression inputs=%llu]\n",
                (unsigned long long)n_calls, (unsigned long long)n_odd, (unsigned long long)n_wild,
                (unsigned long long)n_short, (unsigned long long)n_declined, (unsigned long long)n_restack,
                (unsigned long long)n_console);
  }
} stat;
}  // namespace c82AAFB90
fuzz::Registrar reg_82AAFB90({"sub_82AAFB90", c82AAFB90::orig_w, c82AAFB90::nat_w, me::hot::n_82AAFB90::Writes,
                              me::hot::n_82AAFB90::kCmp, c82AAFB90::kWin, c82AAFB90::kLen, c82AAFB90::gen});
}  // namespace

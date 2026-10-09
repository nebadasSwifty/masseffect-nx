// ORIG: sub_8245FF18 sub_82267B38
// NATIVE: n_8245FF18.h
// TFieldIterator<UProperty>::IterateToNext (English sub_8245FF18), see app/src/native/hot/n_8245FF18.h. Russian copy:
// editions/ru/tests/hot_fuzz/cases/case_8245FF18.inc.
// Environment: random acyclic class chains (+60), field lists (+52 class, +64 next), struct super chains (virtual
// GetSuperStruct at vtable +304 = host stub through the dispatch table; returns [struct + 100] with garbage in the upper
// half of r3, scribbles r4-r12 and its own frame, logs the callee-visible state). A struct flag [+104] makes the stub
// clear the UProperty singleton, so the guest getter's construction path runs mid-loop (its two callees are host stubs
// that return [0x82EA0010] / count calls). The singleton itself is 0 at entry in ~3 % of the inputs (native declines).
extern "C" void __imp__sub_8245FF18(PPCContext&, uint8_t*);
extern "C" void __imp__sub_82267B38(PPCContext&, uint8_t*);
namespace rex::runtime {
__attribute__((weak)) PPCFunc* ResolveIndirectFunction(uint32_t) { return nullptr; }
}  // namespace rex::runtime
namespace {
namespace c8245FF18 {
constexpr uint32_t kWin = 0x82EA0000u, kLen = 0x20000u, kSing = 0x82EAF0C8u, kNewCls = kWin + 0x10, kCnt = kWin + 0x20,
                   kMarker = kWin + 0x30;
constexpr uint32_t kT = 0x82210000u + 4 * 100;
struct Call {
  uint64_t r[12];
};
Call g_log[8], g_orig_log[8];
int g_n = 0, g_orig_n = 0;
uint64_t n_calls = 0, n_decl = 0, n_vcalls = 0, n_clear = 0, n_found = 0, n_end = 0, n_bad = 0;
void Stub(PPCContext& c, uint8_t* base) {
  if (g_n < 8) {
    Call& l = g_log[g_n];
    l.r[0] = c.r1.u64; l.r[1] = c.lr; l.r[2] = c.r3.u64; l.r[3] = c.r4.u64; l.r[4] = c.r5.u64; l.r[5] = c.r6.u64;
    l.r[6] = c.r7.u64; l.r[7] = c.r8.u64; l.r[8] = c.r9.u64; l.r[9] = c.r10.u64; l.r[10] = c.r11.u64; l.r[11] = c.r12.u64;
  }
  ++g_n;
  const uint32_t s = c.r3.u32;
  const uint32_t sup = me::hot::Ld32(base, s + 100);
  if (me::hot::Ld32(base, s + 104) != 0) me::hot::St32(base, kSing, 0);
  me::hot::St32(base, c.r1.u32 - 64, 0xA5A5A5A5u);
  c.r3.u64 = (uint64_t(sup * 0x9E3779B1u + 7) << 32) | sup;
  c.r4.u64 = 0xDEAD0004DEAD0004ull; c.r5.u64 = 0xDEAD0005DEAD0005ull; c.r6.u64 = 0xDEAD0006DEAD0006ull;
  c.r7.u64 = 0xDEAD0007DEAD0007ull; c.r8.u64 = 0xDEAD0008DEAD0008ull; c.r9.u64 = 0xDEAD0009DEAD0009ull;
  c.r10.u64 = 0xDEAD0010DEAD0010ull; c.r11.u64 = 0xDEAD0011DEAD0011ull; c.r12.u64 = 0xDEAD0012DEAD0012ull;
}
void Env(uint8_t* base) {
  *reinterpret_cast<PPCFunc**>(base + 0x82000000ull + 0xFC0000ull + (uint64_t(kT) - 0x82210000ull) * 2) = Stub;
}
}  // namespace c8245FF18
}  // namespace
// The getter's construction path: sub_824B0688 returns the new class object, sub_824B0738 registers it.
extern "C" void sub_824B0688(PPCContext& c, uint8_t* base) {
  c.r3.u64 = me::hot::Ld32(base, c8245FF18::kNewCls);
  c.r4.u64 = 0xBEEF0004ull; c.r10.u64 = 0xBEEF0010ull; c.r11.u64 = 0xBEEF0011ull;
}
extern "C" void sub_824B0738(PPCContext& c, uint8_t* base) {
  me::hot::St32(base, c8245FF18::kCnt, me::hot::Ld32(base, c8245FF18::kCnt) + 1);
  c.r3.u64 = 0x12345678ull; c.r10.u64 = 0xBEEF1010ull;
}
namespace {
namespace c8245FF18 {
void orig_w(PPCContext& ctx, uint8_t* base) {
  Env(base);
  g_n = 0;
  __imp__sub_8245FF18(ctx, base);
  g_orig_n = g_n;
  std::memcpy(g_orig_log, g_log, sizeof g_log);
  n_vcalls += g_n;
  (ctx.r3.u32 != 0 ? ++n_found : ++n_end);
}
void nat_w(PPCContext& ctx, uint8_t* base) {
  Env(base);
  g_n = 0;
  if (me::hot::Ld32(base, kSing) == 0) ++n_decl;
  fuzz::NativeAdapter<me::hot::n_8245FF18::Native, __imp__sub_8245FF18>(ctx, base);
  bool bad = g_n != g_orig_n;
  for (int i = 0; !bad && i < g_n && i < 8; ++i)
    for (int k = 0; k < 12 && !bad; ++k) bad = g_log[i].r[k] != g_orig_log[i].r[k];
  if (bad) {
    if (n_bad < 3) {
      std::printf("   log mismatch: native %d calls, original %d calls\n", g_n, g_orig_n);
      for (int i = 0; i < 8 && (i < g_n || i < g_orig_n); ++i)
        for (int k = 0; k < 12; ++k)
          if (g_log[i].r[k] != g_orig_log[i].r[k])
            std::printf("     call %d reg#%d native=%llx orig=%llx\n", i, k, (unsigned long long)g_log[i].r[k],
                        (unsigned long long)g_orig_log[i].r[k]);
    }
    ++n_bad;
    me::hot::St32(base, kMarker, 0xBADBAD01u);
  }
}
// Write set for the fuzzer: the iterator plus the environment cells the callees may write (singleton, call counter).
void writes(const PPCContext& ctx, const uint8_t*, me::hot::Writes& w) {
  w.Add(ctx.r3.u32, 8);
  w.Add(kSing, 4);
  w.Add(kCnt, 4);
}
void gen(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  ++n_calls;
  using me::hot::St32;
  const uint32_t cls0 = kWin + 0x1000, fld0 = kWin + 0x3000, st0 = kWin + 0x8000, vt = kWin + 0x9000;
  const int nc = 16, nf = 48, ns = 8;
  auto C = [&](int i) { return cls0 + 96u * uint32_t(i); };
  auto F = [&](int i) { return fld0 + 96u * uint32_t(i); };
  auto S = [&](int i) { return st0 + 128u * uint32_t(i); };
  for (int i = 0; i < nc; ++i) {
    uint32_t sup = 0;
    if (i + 1 < nc && !r.chance(0.25)) sup = C(i + 1 + int(r.below(uint32_t(std::min(3, nc - i - 1)))));
    St32(base, C(i) + 60, sup);
  }
  // the UProperty class: somewhere in the chains (low indices reach it less often)
  const uint32_t prop = C(int(r.range(4, nc - 1)));
  St32(base, kSing, r.chance(0.03) ? 0 : (r.chance(0.97) ? prop : C(int(r.below(nc)))));
  St32(base, kNewCls, r.chance(0.8) ? prop : (r.chance(0.5) ? 0 : C(int(r.below(nc)))));
  St32(base, kCnt, 0);
  for (int i = 0; i < nf; ++i) {
    St32(base, F(i) + 52, r.chance(0.08) ? 0 : C(int(r.below(nc))));
    uint32_t nx = 0;
    if (i + 1 < nf && !r.chance(0.15)) nx = F(i + 1 + int(r.below(uint32_t(std::min(4, nf - i - 1)))));
    St32(base, F(i) + 64, nx);
  }
  St32(base, vt + 304, kT);
  for (int i = 0; i < ns; ++i) {
    St32(base, S(i) + 0, vt);
    St32(base, S(i) + 76, r.chance(0.2) ? 0 : F(int(r.below(nf))));
    uint32_t sup = 0;
    if (i + 1 < ns && !r.chance(0.2)) sup = S(i + 1 + int(r.below(uint32_t(std::min(2, ns - i - 1)))));
    St32(base, S(i) + 100, sup);
    St32(base, S(i) + 104, r.chance(0.04) ? 1 : 0);
    if (me::hot::Ld32(base, S(i) + 104)) ++n_clear;
  }
  const uint32_t it = kWin + 0xA000 + r.below(64) * 16;
  St32(base, it + 0, r.chance(0.05) ? 0 : S(int(r.below(ns))));
  St32(base, it + 4, r.chance(0.15) ? 0 : F(int(r.below(nf))));
  ctx.r3.u64 = it | (r.chance(0.3) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0);
  Env(base);
}
struct Stat {
  ~Stat() {
    std::printf("   [8245FF18 coverage: calls=%llu declined(singleton 0)=%llu virtual calls (orig)=%llu clear-flag structs=%llu "
                "orig r3!=0=%llu r3==0=%llu | callee-visible-state log mismatches=%llu]\n",
                (unsigned long long)n_calls, (unsigned long long)n_decl, (unsigned long long)n_vcalls,
                (unsigned long long)n_clear, (unsigned long long)n_found, (unsigned long long)n_end,
                (unsigned long long)n_bad);
  }
} stat;
}  // namespace c8245FF18
fuzz::Registrar reg_8245FF18({"sub_8245FF18", c8245FF18::orig_w, c8245FF18::nat_w, c8245FF18::writes,
                              me::hot::n_8245FF18::kCmp, c8245FF18::kWin, c8245FF18::kLen, c8245FF18::gen,
                              me::hot::n_8245FF18::Writes});  // guard replay: the console guard's write set
}  // namespace

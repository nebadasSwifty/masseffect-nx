// ORIG: sub_8256A1A0
// NATIVE: n_8256A1A0.h
// The function reads constants from, and writes two globals in, guest memory far outside the fuzz window (0x820B.. 0x82EC..).
// The harness only copies/compares the window, so this case synchronises those few small regions itself: the generator fills
// them in arena A and keeps a snapshot; the native wrapper restores that snapshot into arena B, runs the native version
// and then compares the regions with arena A (after the original ran) and with the declared Writes(); a mismatch flips a
// window byte of arena B so the harness reports a failure.
extern "C" void __imp__sub_8256A1A0(PPCContext&, uint8_t*);
namespace {
namespace c8256A1A0 {
struct Reg {
  uint32_t addr, len;
};
const Reg kRegs[] = {{0x821BE114, 4}, {0x821BE37C, 4}, {0x820BCE44, 4}, {0x820BCD64, 4},
                     {0x820B90B8, 4}, {0x821BE4E8, 8}, {0x82EC3114, 12}, {0x82EAF510, 4}};
constexpr int kN = sizeof(kRegs) / sizeof(kRegs[0]);
uint8_t g_init[kN][16];
uint8_t* g_a = nullptr;
uint64_t g_gen = 0, g_synced = ~0ull;
uint64_t g_cov[16];

void put32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}
uint32_t fbits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}
float nice(fuzz::Rng& r) {
  switch (r.below(8)) {
    case 0: return float(int(r.below(9)) - 4);
    case 1: return float(int(r.below(2001)) - 1000) / 8.0f;
    default: return float(int(r.below(200001)) - 100000) / 16384.0f;
  }
}
// mostly ordinary, sometimes NaN / inf / denormal / huge
uint32_t odd(fuzz::Rng& r) {
  if (r.chance(0.05)) return r.float_bits();
  return fbits(nice(r));
}

void gen(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  g_a = base;
  ++g_gen;
  const uint32_t win = 0x40000000, sp = win + 0x20000 - 0x100;
  // ---- constants
  float kmin, kmax, k1, k3, k4;
  double k2;
  switch (r.below(6)) { case 0: kmin = 3.4e38f; kmax = -3.4e38f; break; case 1: kmin = 1e30f; kmax = -1e30f; break;
    case 2: kmin = __builtin_inff(); kmax = -__builtin_inff(); break; case 3: kmin = 100.0f; kmax = -100.0f; break;
    case 4: kmin = 3.4e38f; kmax = -__builtin_inff(); break;
    default: { float a = nice(r); kmin = a; kmax = nice(r); } }
  if (r.chance(0.02)) kmin = __builtin_nanf("");
  switch (r.below(5)) { case 0: k1 = 1e-3f; break; case 1: k1 = 1e-4f; break; case 2: k1 = 0.5f; break; case 3: k1 = 0.0f; break;
    default: k1 = std::fabs(nice(r)) * 0.01f; }
  switch (r.below(6)) { case 0: k2 = double(k1); break; case 1: k2 = 1e-8; break; case 2: k2 = 0.1; break; case 3: k2 = 1e-3; break;
    case 4: k2 = double(k1) * 4.0; break;
    default: k2 = double(std::fabs(nice(r))) * 0.01; }
  if (r.chance(0.03)) k2 = -1.0;
  switch (r.below(3)) { case 0: k3 = 1.0f; break; case 1: k3 = 0.001f; break; default: k3 = nice(r); }
  switch (r.below(5)) { case 0: case 1: k4 = 0.0f; break; case 2: k4 = -0.0f; break; case 3: k4 = 0.5f; break; default: k4 = nice(r); }
  put32(base, 0x821BE114, fbits(kmin));
  put32(base, 0x821BE37C, fbits(kmax));
  put32(base, 0x820BCE44, fbits(k1));
  put32(base, 0x820BCD64, fbits(k3));
  put32(base, 0x820B90B8, fbits(k4));
  {
    uint64_t b;
    std::memcpy(&b, &k2, 8);
    b = __builtin_bswap64(b);
    std::memcpy(base + 0x821BE4E8, &b, 8);
  }
  // globals written by the parallel branch: random content, flag random
  for (int i = 0; i < 3; ++i) put32(base, 0x82EC3114 + 4 * i, r.u32());
  put32(base, 0x82EAF510, r.chance(0.5) ? 0 : r.u32());
  for (int i = 0; i < kN; ++i) std::memcpy(g_init[i], base + kRegs[i].addr, kRegs[i].len);
  // ---- vertex list
  const uint32_t lst = win + 0x100 + r.below(0x40) * 8;
  const uint32_t verts = win + 0x2000 + r.below(0x40) * 4;
  int32_t cnt;
  switch (r.below(10)) {
    case 0: cnt = 0; break;
    case 1: cnt = -int32_t(r.below(5)); break;
    case 2: cnt = 1; break;
    case 3: cnt = int32_t(r.range(1, 400)); break;
    default: cnt = int32_t(r.range(1, 12));
  }
  put32(base, lst, verts);
  put32(base, lst + 4, uint32_t(cnt));
  const float scale = r.chance(0.7) ? 1.0f : (r.chance(0.5) ? 10.0f : 0.01f);
  for (int i = 0; i < (cnt > 0 ? cnt : 0) * 3 && i < 1500; ++i) put32(base, verts + 4 * i, r.chance(0.97) ? fbits(nice(r) * scale) : r.float_bits());
  // ---- direction
  const uint32_t dir = win + 0x4000 + r.below(0x40) * 4;
  float d0 = nice(r), d1 = nice(r), d2 = nice(r);
  if (r.chance(0.2)) d0 = 0;
  if (r.chance(0.2)) d1 = 0;
  if (r.chance(0.2)) d2 = 0;
  put32(base, dir, r.chance(0.04) ? r.float_bits() : fbits(d0));
  put32(base, dir + 4, r.chance(0.04) ? r.float_bits() : fbits(d1));
  put32(base, dir + 8, r.chance(0.04) ? r.float_bits() : fbits(d2));
  // ---- ray parameters (doubles in f1..f3)
  double t = double(nice(r)), tend;
  switch (r.below(6)) {
    case 0: tend = t; break;
    case 1: tend = t + double(k1) * 0.5; break;
    case 2: tend = t - double(k1) * 0.5; break;
    case 3: tend = t + 1e-5; break;
    default: tend = double(nice(r)) * 0.5;
  }
  double pad = r.chance(0.3) ? 0.0 : double(std::fabs(nice(r)) * 0.25f);
  if (r.chance(0.04)) t = double(r.float_bits() ? nice(r) : 0.0f) + 1e-12;       // not a float value
  if (r.chance(0.03)) { uint64_t b = r.u64(); std::memcpy(&t, &b, 8); }
  if (r.chance(0.03)) { uint64_t b = r.u64(); std::memcpy(&tend, &b, 8); }
  if (r.chance(0.02)) { uint64_t b = r.u64(); std::memcpy(&pad, &b, 8); }
  if (r.chance(0.03)) tend = double(r.float_bits() ? __builtin_nanf("") : 0.0f);
  ctx.f1.f64 = t;
  ctx.f2.f64 = tend;
  ctx.f3.f64 = pad;
  // ---- outputs and arguments
  ctx.r3.u64 = lst;
  ctx.r5.u64 = dir;
  const uint32_t o9 = win + 0x6000 + r.below(0x100) * 4;
  uint32_t o10 = r.chance(0.1) ? o9 : win + 0x6400 + r.below(0x100) * 4;
  if (r.chance(0.03)) o10 = o9 + 2;
  ctx.r9.u64 = o9;
  ctx.r10.u64 = o10;
  put32(base, o9, odd(r));
  if (o10 != o9) put32(base, o10, odd(r));
  uint32_t p84 = win + 0x6800 + r.below(0x100) * 4, p92 = win + 0x6C00 + r.below(0x100) * 4;
  if (r.chance(0.05)) p92 = p84 + 4 * r.below(4);
  if (r.chance(0.05)) p84 = o9 - 4 * r.below(3);
  if (r.chance(0.05)) p92 = o10 - 4 * r.below(3);
  if (r.chance(0.05)) p84 = dir - 4 * r.below(4);
  if (r.chance(0.05)) p92 = dir - 4 * r.below(4);
  put32(base, sp + 84, p84);
  put32(base, sp + 92, p92);
  // make the t13 > cur9 / t0 < cur10 comparisons go both ways: bias the current values towards +-range
  if (r.chance(0.3)) put32(base, o9, fbits(-100.0f));
  if (r.chance(0.3) && o10 != o9) put32(base, o10, fbits(100.0f));
}

// native wrapper: out-of-window synchronisation and checks (see the top of the file)
void nat(PPCContext& ctx, uint8_t* baseB) {
  if (ctx.lr == 0 || !g_a) {  // benchmark mode: only synchronise once per generated input
    if (g_synced != g_gen) {
      for (int i = 0; i < kN; ++i) std::memcpy(baseB + kRegs[i].addr, g_init[i], kRegs[i].len);
      g_synced = g_gen;
    }
    me::hot::n_8256A1A0::Native(ctx, baseB);
    return;
  }
  for (int i = 0; i < kN; ++i) std::memcpy(baseB + kRegs[i].addr, g_init[i], kRegs[i].len);
  me::hot::Writes w;
  me::hot::n_8256A1A0::Writes(ctx, baseB, w);
  me::hot::n_8256A1A0::Native(ctx, baseB);
  const char* bad = nullptr;
  for (int i = 0; i < kN && !bad; ++i)
    for (uint32_t k = 0; k < kRegs[i].len; ++k) {
      const uint32_t a = kRegs[i].addr + k;
      if (baseB[a] != g_a[a]) bad = "region differs from the original";
      if (baseB[a] != g_init[i][k]) {
        bool cov = false;
        for (int j = 0; j < w.n; ++j) cov |= (a >= w.r[j].addr && a - w.r[j].addr < w.r[j].len);
        if (!cov) bad = "changed outside Writes()";
      }
    }
  if (bad) {
    static int shown = 0;
    if (shown++ < 5) std::printf("  [8256A1A0 out-of-window: %s]\n", bad);
    baseB[0x40000000] ^= 0xFF;
  }
}
fuzz::Registrar reg_8256A1A0({"sub_8256A1A0", __imp__sub_8256A1A0, nat, me::hot::n_8256A1A0::Writes,
                              me::hot::n_8256A1A0::kCmp, 0x40000000, 0x20000, gen});
}  // namespace c8256A1A0
}  // namespace

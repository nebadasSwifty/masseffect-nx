// ORIG: sub_8256AFD8
// NATIVE: n_8256AFD8.h
// Constants live outside the fuzz window (0x821BE...): same out-of-window synchronisation scheme as case_8256A1A0.inc
// (the function writes nothing outside the window, but the wrapper still verifies that).
extern "C" void __imp__sub_8256AFD8(PPCContext&, uint8_t*);
namespace {
namespace c8256AFD8 {
struct Reg {
  uint32_t addr, len;
};
const Reg kRegs[] = {{0x821BE114, 4}, {0x821BE37C, 4}, {0x821BE1C8, 8}};
constexpr int kN = sizeof(kRegs) / sizeof(kRegs[0]);
uint8_t g_init[kN][16];
uint8_t* g_a = nullptr;
uint64_t g_gen = 0, g_synced = ~0ull;

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
uint32_t odd(fuzz::Rng& r) {
  if (r.chance(0.05)) return r.float_bits();
  return fbits(nice(r));
}

void gen(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  g_a = base;
  ++g_gen;
  const uint32_t win = 0x40000000;
  float kmin, kmax;
  switch (r.below(6)) {
    case 0: kmin = 3.4e38f; kmax = -3.4e38f; break;
    case 1: kmin = 1e30f; kmax = -1e30f; break;
    case 2: kmin = __builtin_inff(); kmax = -__builtin_inff(); break;
    case 3: kmin = 100.0f; kmax = -100.0f; break;
    case 4: kmin = 3.4e38f; kmax = -__builtin_inff(); break;
    default: kmin = nice(r); kmax = nice(r);
  }
  if (r.chance(0.02)) kmin = __builtin_nanf("");
  double k;
  switch (r.below(5)) {
    case 0: k = 1.0; break;
    case 1: k = 0.5; break;
    case 2: k = 2.0 * (1 + r.below(4)); break;
    case 3: k = double(nice(r)); break;
    default: k = 1.0 / 3.0;
  }
  if (r.chance(0.02)) k = 0.0;
  put32(base, 0x821BE114, fbits(kmin));
  put32(base, 0x821BE37C, fbits(kmax));
  {
    uint64_t b;
    std::memcpy(&b, &k, 8);
    b = __builtin_bswap64(b);
    std::memcpy(base + 0x821BE1C8, &b, 8);
  }
  for (int i = 0; i < kN; ++i) std::memcpy(g_init[i], base + kRegs[i].addr, kRegs[i].len);
  // vertex list
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
  for (int i = 0; i < (cnt > 0 ? cnt : 0) * 3 && i < 1500; ++i)
    put32(base, verts + 4 * i, r.chance(0.97) ? fbits(nice(r) * scale) : r.float_bits());
  // direction
  const uint32_t dir = win + 0x4000 + r.below(0x40) * 4;
  float d0 = nice(r), d1 = nice(r), d2 = nice(r);
  if (r.chance(0.1)) d0 = 0;
  if (r.chance(0.1)) d1 = 0;
  if (r.chance(0.1)) d2 = 0;
  if (r.chance(0.1)) { d0 = 0; d1 = 0; d2 = 0; }
  put32(base, dir, r.chance(0.04) ? r.float_bits() : fbits(d0));
  put32(base, dir + 4, r.chance(0.04) ? r.float_bits() : fbits(d1));
  put32(base, dir + 8, r.chance(0.04) ? r.float_bits() : fbits(d2));
  // t, pad: t near the interval [lo, hi] of the projected vertices so that every branch is reached
  double lo = 1e30, hi = -1e30;
  for (int i = 0; i < (cnt > 0 ? cnt : 0) && i < 500; ++i) {
    float v[3];
    for (int j = 0; j < 3; ++j) {
      uint32_t b;
      std::memcpy(&b, base + verts + 12 * i + 4 * j, 4);
      b = __builtin_bswap32(b);
      std::memcpy(&v[j], &b, 4);
    }
    double dd = double(v[0]) * d0 + double(v[1]) * d1 + double(v[2]) * d2;
    lo = dd < lo ? dd : lo;
    hi = dd > hi ? dd : hi;
  }
  if (lo > hi) { lo = -4; hi = 4; }
  double pad = r.chance(0.3) ? 0.0 : double(std::fabs(nice(r)) * 0.25f);
  double t;
  switch (r.below(6)) {
    case 0: t = lo - pad - 0.5; break;
    case 1: t = hi + pad + 0.5; break;
    case 2: t = lo - pad; break;
    case 3: t = hi + pad; break;
    default: t = lo + (hi - lo) * (double(r.below(1001)) / 1000.0);
  }
  t = double(float(t));
  if (r.chance(0.03)) { uint64_t b = r.u64(); std::memcpy(&t, &b, 8); }
  if (r.chance(0.03)) { uint64_t b = r.u64(); std::memcpy(&pad, &b, 8); }
  if (r.chance(0.03)) t += 1e-9;
  ctx.f1.f64 = t;
  ctx.f2.f64 = pad;
  ctx.r3.u64 = lst;
  ctx.r4.u64 = dir;
  // io float + out vector
  const uint32_t io = win + 0x6000 + r.below(0x100) * 4;
  uint32_t out = win + 0x6800 + r.below(0x100) * 4;
  if (r.chance(0.05)) out = dir - 4 * r.below(4);   // output overlaps the direction
  if (r.chance(0.03)) out = io - 4 * r.below(3);    // output overlaps *r7
  put32(base, io, r.chance(0.1) ? 0 : (r.chance(0.5) ? fbits(std::fabs(nice(r))) : odd(r)));
  ctx.r7.u64 = io;
  ctx.r8.u64 = out;
}

void nat(PPCContext& ctx, uint8_t* baseB) {
  if (ctx.lr == 0 || !g_a) {  // benchmark mode
    if (g_synced != g_gen) {
      for (int i = 0; i < kN; ++i) std::memcpy(baseB + kRegs[i].addr, g_init[i], kRegs[i].len);
      g_synced = g_gen;
    }
    me::hot::n_8256AFD8::Native(ctx, baseB);
    return;
  }
  for (int i = 0; i < kN; ++i) std::memcpy(baseB + kRegs[i].addr, g_init[i], kRegs[i].len);
  me::hot::Writes w;
  me::hot::n_8256AFD8::Writes(ctx, baseB, w);
  me::hot::n_8256AFD8::Native(ctx, baseB);
  bool bad = false;
  for (int i = 0; i < kN && !bad; ++i)
    for (uint32_t k = 0; k < kRegs[i].len; ++k)
      if (baseB[kRegs[i].addr + k] != g_a[kRegs[i].addr + k]) bad = true;
  if (bad) {
    static int shown = 0;
    if (shown++ < 5) std::printf("  [8256AFD8 out-of-window region differs]\n");
    baseB[0x40000000] ^= 0xFF;
  }
}
fuzz::Registrar reg_8256AFD8({"sub_8256AFD8", __imp__sub_8256AFD8, nat, me::hot::n_8256AFD8::Writes,
                              me::hot::n_8256AFD8::kCmp, 0x40000000, 0x20000, gen});
}  // namespace c8256AFD8
}  // namespace

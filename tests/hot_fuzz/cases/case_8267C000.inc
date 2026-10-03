// ORIG: sub_8267C000
// NATIVE: n_8267C000.h
extern "C" void __imp__sub_8267C000(PPCContext&, uint8_t*);
namespace {
// Read-only constants of the function (outside the window): eps double @0x821BE4E8, float thr @0x820B90B8, float scale
// @0x820BCD64, poked into the arena used for each call.
uint64_t g_eps_8267C000;
uint32_t g_thr_8267C000, g_scale_8267C000;
void poke_8267C000(uint8_t* base) {
  uint64_t e = __builtin_bswap64(g_eps_8267C000);
  uint32_t t = __builtin_bswap32(g_thr_8267C000), s = __builtin_bswap32(g_scale_8267C000);
  std::memcpy(base + 0x821BE4E8u, &e, 8);
  std::memcpy(base + 0x820B90B8u, &t, 4);
  std::memcpy(base + 0x820BCD64u, &s, 4);
}
void orig_8267C000(PPCContext& c, uint8_t* base) {
  poke_8267C000(base);
  __imp__sub_8267C000(c, base);
}
void nat_8267C000(PPCContext& c, uint8_t* base) {
  poke_8267C000(base);
  me::hot::n_8267C000::Native(c, base);
}
void put32_8267C000(uint8_t* base, uint32_t addr, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + addr, &v, 4);
}
uint32_t fb_8267C000(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}
// float word: mostly normal, `special` => the usual zoo; signalling NaNs only when `snan`
uint32_t word_8267C000(fuzz::Rng& r, bool special, bool snan, float scale) {
  if (special && r.chance(0.25)) {
    uint32_t b = r.float_bits();
    if (!snan && (b & 0x7F800000u) == 0x7F800000u && (b & 0x7FFFFFu)) b |= 0x400000u;
    return b;
  }
  return fb_8267C000((float(r.below(2000001)) - 1000000.0f) * (scale / 1000000.0f));
}
int g_dbg_it_8267C000 = 0;
void gen_8267C000(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  ++g_dbg_it_8267C000;
  const uint32_t win = 0x40000000;
  const bool special = r.chance(0.35);
  const float scale = r.chance(0.8) ? 3.0f : (r.chance(0.5) ? 100.0f : 0.01f);
  uint32_t v[4];
  for (int i = 0; i < 4; ++i) v[i] = win + 0x100 + i * 0x40 + (r.chance(0.2) ? 4 : 0);
  for (int i = 0; i < 4; ++i)
    for (int k = 0; k < 3; ++k) put32_8267C000(base, v[i] + 4 * k, word_8267C000(r, special, true, scale));
  // some structure: make two projections equal / the axis axis-aligned
  if (r.chance(0.1)) for (int k = 0; k < 3; ++k) put32_8267C000(base, v[1] + 4 * k, __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + v[2] + 4 * k)));
  if (r.chance(0.2)) { put32_8267C000(base, v[3] + 4, fb_8267C000(0.0f)); put32_8267C000(base, v[3] + 8, fb_8267C000(0.0f)); }
  uint32_t r10 = win + 0x1000, o = win + 0x2000, o2 = win + 0x2100;
  // best-time float and far-time float
  if (r.chance(0.03)) o2 = r10;                                        // aliases the best time
  else if (r.chance(0.03)) r10 = o + 4 * r.below(3);                   // best time inside the output vector
  if (r.chance(0.03)) o = v[1] + 4 * r.below(3) - 4 * r.below(2);      // output vector over an input vector
  // (no signalling NaN in the two words the function compares / reads back: see the header notes)
  put32_8267C000(base, r10, word_8267C000(r, special, false, 3.0f));
  put32_8267C000(base, o2, word_8267C000(r, special, false, 3.0f));
  put32_8267C000(base, win + 0x3F54, o2);
  put32_8267C000(base, win + 0x3F5C, o);
  ctx.r3.u64 = v[0];
  ctx.r4.u64 = v[1];
  ctx.r5.u64 = v[2];
  ctx.r6.u64 = v[3];
  ctx.r10.u64 = r10 | (r.chance(0.2) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0);
  // times
  double f1 = (double(r.below(2000001)) - 1000000.0) * 3e-6;
  if (r.chance(0.2)) f1 = double(float(f1));
  double f2;
  const uint32_t m = r.below(10);
  if (m < 2) f2 = f1;                                            // parallel case (|f2 - f1| = 0 < eps)
  else if (m < 3) f2 = f1 + 1e-7;                                // tiny, around eps
  else if (m < 4) f2 = f1 - 3e-6;
  else f2 = f1 + (double(r.below(2000001)) - 1000000.0) * 4e-6;
  if (r.chance(0.04)) { uint64_t b = r.u64(); std::memcpy(&f2, &b, 8); }
  if (r.chance(0.04)) { uint64_t b = r.u64(); std::memcpy(&f1, &b, 8); }
  ctx.f1.f64 = f1;
  ctx.f2.f64 = f2;
  double f3 = r.chance(0.2) ? 0.0 : double(r.below(1000000)) * 2e-6;
  if (r.chance(0.3)) f3 = double(float(f3));
  if (r.chance(0.03)) { uint64_t b = r.u64(); std::memcpy(&f3, &b, 8); }
  ctx.f3.f64 = f3;
  // constants
  const double epsv[] = {1e-6, 1e-5, 1e-3, 0.0, 1e-9};
  double eps = r.chance(0.85) ? epsv[r.below(3)] : epsv[r.below(5)];
  if (r.chance(0.02)) { uint64_t b = r.u64(); std::memcpy(&eps, &b, 8); }
  std::memcpy(&g_eps_8267C000, &eps, 8);
  g_thr_8267C000 = r.chance(0.85) ? fb_8267C000(0.0f) : fb_8267C000(float(r.below(100)) * 0.01f - 0.3f);
  if (r.chance(0.02)) {  // odd threshold; no signalling NaN (clang -O2 merges the two final compares through fmaxnm, which
                         // treats an sNaN operand as unspecified: an optimiser artefact of the Mac build, not the Switch GCC)
    g_thr_8267C000 = r.float_bits();
    if ((g_thr_8267C000 & 0x7F800000u) == 0x7F800000u && (g_thr_8267C000 & 0x7FFFFFu)) g_thr_8267C000 |= 0x400000u;
  }
  g_scale_8267C000 = r.chance(0.85) ? fb_8267C000(1.0f) : fb_8267C000(float(r.below(400)) * 0.01f - 1.0f);
  if (r.chance(0.02)) g_scale_8267C000 = r.float_bits();
  if (const char* e = std::getenv("HOTDBG")) if (g_dbg_it_8267C000 == std::atoi(e) + 1) {
    for (int i = 0; i < 4; ++i) { for (int k = 0; k < 3; ++k) { uint32_t w = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + v[i] + 4 * k)); float f; std::memcpy(&f, &w, 4); std::printf("v%d[%d]=%08x(%g) ", i, k, w, f); } std::printf("\n"); }
    uint64_t a, b2, c2; std::memcpy(&a, &ctx.f1.f64, 8); std::memcpy(&b2, &ctx.f2.f64, 8); std::memcpy(&c2, &ctx.f3.f64, 8);
    std::printf("f1=%016llx(%g) f2=%016llx(%g) f3=%016llx(%g) eps=%g thr=%08x scale=%08x r10=%x o=%x o2=%x\n", (unsigned long long)a, ctx.f1.f64, (unsigned long long)b2, ctx.f2.f64, (unsigned long long)c2, ctx.f3.f64, eps, g_thr_8267C000, g_scale_8267C000, (unsigned)ctx.r10.u32, o, o2);
    std::printf("r10val=%08x o2val=%08x\n", __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + r10)), __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + o2)));
  }
}
fuzz::Registrar reg_8267C000({"sub_8267C000", orig_8267C000, nat_8267C000, me::hot::n_8267C000::Writes,
                              me::hot::n_8267C000::kCmp, 0x40000000, 0x4000, gen_8267C000});
}  // namespace

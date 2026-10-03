// ORIG: sub_824DD848
// NATIVE: n_824DD848.h
extern "C" void __imp__sub_824DD848(PPCContext&, uint8_t*);
namespace {
// The window holds the RNG word (0x82EAC8C0) and the object / table / output; the two read-only float constants the
// function loads (0x820B90B8, 0x820BCCD4) live outside the window: the wrappers below poke them into the arena that
// is used for the call (both arenas get the same values).
uint32_t g_cst_f12_824DD848, g_cst_thr_824DD848;
void poke_824DD848(uint8_t* base) {
  uint32_t a = __builtin_bswap32(g_cst_f12_824DD848), b = __builtin_bswap32(g_cst_thr_824DD848);
  std::memcpy(base + 0x820B90B8u, &a, 4);
  std::memcpy(base + 0x820BCCD4u, &b, 4);
}
void orig_824DD848(PPCContext& c, uint8_t* base) {
  poke_824DD848(base);
  __imp__sub_824DD848(c, base);
}
void nat_824DD848(PPCContext& c, uint8_t* base) {
  poke_824DD848(base);
  me::hot::n_824DD848::Native(c, base);
}
void put32_824DD848(uint8_t* base, uint32_t addr, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + addr, &v, 4);
}
float BitsToF_824DD848(uint32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}
uint32_t flt_824DD848(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}
void gen_824DD848(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  const uint32_t win = 0x82EAC000;
  // table / data area: float patterns
  for (uint32_t a = win + 0xA00; a < win + 0x2600; a += 4) {
    uint32_t b = r.float_bits();
    if ((b & 0x7F800000u) == 0x7F800000u && (b & 0x7FFFFFu)) b |= 0x400000u;  // no signalling NaNs (see header)
    put32_824DD848(base, a, b);
  }
  const uint32_t obj = win + 0x100 + r.below(0x20) * 0x20;
  // type byte
  uint32_t type = r.below(10) < 3 ? 3 : (r.below(10) < 4 ? 2 : r.below(256));
  if (r.chance(0.1)) type = r.below(5);
  uint32_t b2 = r.chance(0.04) ? 0 : r.range(1, 6), b3 = r.chance(0.04) ? 0 : r.range(1, 6);
  base[obj + 1] = uint8_t(type);
  base[obj + 2] = uint8_t(b2);
  base[obj + 3] = uint8_t(b3);
  const uint32_t n = b2 * b3;
  uint32_t k = r.range(1, 8);
  uint32_t size = 2 + n * k + (n ? r.below(n) : 0);
  if (r.chance(0.04)) size = r.u32();
  else if (r.chance(0.04)) size = r.below(4);
  const uint32_t P = win + 0xA00 + r.below(0x300) * 4;
  put32_824DD848(base, obj + 4, r.chance(0.02) ? win + r.below(0x3000) : P);
  put32_824DD848(base, obj + 8, size);
  static const float steps[] = {0.5f, 1.0f, 0.1f, 0.25f, 2.0f, 1e-30f, 3.3f, -1.0f, 0.0f};
  float step = r.chance(0.8) ? steps[r.below(6)] : steps[r.below(9)];
  if (r.chance(0.05)) step = BitsToF_824DD848(r.float_bits());
  float mn = r.chance(0.6) ? 0.0f : float(int32_t(r.u32() % 2001) - 1000) * 0.125f;
  if (r.chance(0.04)) mn = BitsToF_824DD848(r.float_bits());
  if (step != step) {
    // A NaN step makes f12/f13 NaNs; a NaN table entry in the same fmadds would make the NaN choice depend on the operand
    // order the C++ compiler picked for the commutative multiplicands (clang here, GCC on the Switch): keep one NaN source.
    for (uint32_t a = win + 0xA00; a < win + 0x2600; a += 4) {
      uint32_t v;
      std::memcpy(&v, base + a, 4);
      v = __builtin_bswap32(v);
      if ((v & 0x7F800000u) == 0x7F800000u && (v & 0x7FFFFFu)) put32_824DD848(base, a, 0);
    }
  }
  put32_824DD848(base, obj + 16, flt_824DD848(step));
  put32_824DD848(base, obj + 20, flt_824DD848(mn));
  g_cst_f12_824DD848 = r.chance(0.8) ? 0 : (r.chance(0.5) ? flt_824DD848(step) : r.float_bits());
  g_cst_thr_824DD848 = r.chance(0.8) ? flt_824DD848(float(r.below(1000)) * 0.001f) : r.float_bits();
  // time
  double t;
  uint32_t m = r.below(20);
  if (m < 15) {
    double u = (double(r.below(1000000)) / 1000000.0) * (double(k) + 2.0) - 0.6;
    t = double(mn) + double(step) * u;
    if (r.chance(0.5)) t = double(float(t));
    if (r.chance(0.1)) t = double(mn) + double(step) * double(r.range(0, k + 1));  // exactly on a row boundary
  } else if (m < 17) {
    t = double(int32_t(r.u32())) / 1024.0;
  } else {
    uint64_t bits = r.u64();
    if (r.chance(0.5)) bits = (bits & 0x800FFFFFFFFFFFFFull) | (uint64_t(1023 + int(r.below(70)) - 5) << 52);
    std::memcpy(&t, &bits, 8);
  }
  ctx.f1.f64 = t;
  // output
  uint32_t cnt;
  if (r.chance(0.08)) cnt = r.chance(0.5) ? 0 : uint32_t(-int32_t(r.below(5)));
  else if (r.chance(0.9)) cnt = r.range(1, 12);
  else cnt = r.range(1, 100);
  uint32_t out = win + 0x2800 + r.below(0x300) * 4;
  if (r.chance(0.02)) out = obj + r.below(6) - 2;             // aliases the object bytes (kept word-ish)
  else if (r.chance(0.02)) out = win + 0x8C0 - 4 * r.below(4);  // aliases the RNG word
  else if (r.chance(0.02)) out = P + 4 * r.below(40);           // aliases the table
  ctx.r3.u64 = obj | (r.chance(0.2) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0);
  ctx.r5.u64 = out | (r.chance(0.2) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0);
  ctx.r6.u64 = cnt | (r.chance(0.2) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0);
  put32_824DD848(base, win + 0x8C0, r.u32());  // RNG state
}
fuzz::Registrar reg_824DD848({"sub_824DD848", orig_824DD848, nat_824DD848, me::hot::n_824DD848::Writes,
                              me::hot::n_824DD848::kCmp, 0x82EAC000, 0x4000, gen_824DD848});
}  // namespace

// ORIG: sub_822631E8 sub_822634D8
// NATIVE: n_822631E8.h
// sub_822631E8: 4x4 float matrix inverse. The two constants it reads (1.0f, the compare constant 0.0f) and the 64-byte
// identity block live outside the fuzz window, so the generator writes them into arena A and the native wrapper writes
// the same bytes into arena B (the harness only copies the window).
extern "C" void __imp__sub_822631E8(PPCContext&, uint8_t*);
extern "C" void __imp__sub_822634D8(PPCContext&, uint8_t*);
namespace {
namespace c822631E8 {

uint32_t g_one = 0x3F800000, g_cmp = 0, g_ident[16];  // host-order bit patterns of the guest constants
uint64_t n_gen = 0, n_det0 = 0, n_cmp_eq = 0, n_nan = 0, n_alias = 0;
struct Stat {
  ~Stat() {
    std::printf("   [822631E8 coverage: gen=%llu det==0 branch=%llu det==cmp(nonzero)=%llu nan-det-rerolled=%llu dst-overlaps-src=%llu]\n",
                (unsigned long long)n_gen, (unsigned long long)n_det0, (unsigned long long)n_cmp_eq,
                (unsigned long long)n_nan, (unsigned long long)n_alias);
  }
} stat;
void put_consts(uint8_t* base) {
  me::hot::St32(base, me::hot::n_822631E8::kR, g_one);
  me::hot::St32(base, me::hot::n_822631E8::kR - 15532, g_cmp);
  for (int i = 0; i < 16; ++i) me::hot::St32(base, me::hot::n_822631E8::kIdent + 4 * i, g_ident[i]);
}
void nat(PPCContext& ctx, uint8_t* base) {
  put_consts(base);
  me::hot::n_822631E8::Native(ctx, base);
}
uint32_t nonan(uint32_t b) { return ((b & 0x7F800000u) == 0x7F800000u && (b & 0x7FFFFFu)) ? (b & 0xFF800000u) : b; }
float fb(uint32_t b) {
  if ((b & 0x7F800000u) == 0x7F800000u && (b & 0x7FFFFFu)) b &= 0xFF800000u;  // NaN -> inf (see gen)
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}
void gen(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  const uint32_t win = 0x40000000;
  // constants: usually the real ones (1.0f, 0.0f, identity); sometimes perturbed
  g_one = r.chance(0.9) ? 0x3F800000u : (r.float_bits() & 0xBFFFFFFFu);  // finite (inf would create NaN mixes, see gen)
  g_cmp = r.chance(0.9) ? 0u : nonan(r.float_bits());
  for (int i = 0; i < 16; ++i) g_ident[i] = (i % 5 == 0) ? 0x3F800000u : (r.chance(0.97) ? 0u : r.float_bits());
  put_consts(base);

  const uint32_t src = win + 0x1000 + (r.chance(0.8) ? r.below(0x800) * 16 : r.below(0x2000));
  uint32_t dst = win + 0x9000 + (r.chance(0.8) ? r.below(0x800) * 16 : r.below(0x2000));
  if (r.chance(0.12)) dst = src;                               // in-place
  else if (r.chance(0.08)) dst = src + 4 * r.range(0, 24) - 48;  // partial overlap
  if (dst != src && dst + 64 > src && src + 64 > dst) n_alias++;
  float m[16];
  uint32_t kind = r.below(20);
  auto small = [&]() { return float(int32_t(r.below(41)) - 20); };
  auto frac = [&]() { return float(int32_t(r.below(2000001)) - 1000000) * (1.0f / 1024.0f); };
  auto build = [&]() {
  for (int i = 0; i < 16; ++i) m[i] = r.chance(0.5) ? small() : frac();
  switch (kind) {
    case 0: for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f; break;           // identity
    case 1: for (int i = 0; i < 16; ++i) m[i] = 0.0f; break;                                   // zero matrix
    case 2: for (int i = 0; i < 4; ++i) m[4 + i] = m[i]; break;                                // two equal rows -> det 0
    case 3: for (int i = 0; i < 4; ++i) m[8 + i] = m[i] * 2.0f; break;                         // proportional rows
    case 4: for (int i = 0; i < 4; ++i) m[12 + i] = m[i] + m[4 + i]; break;                    // dependent rows
    case 5: for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? small() : 0.0f; break;          // diagonal (maybe singular)
    case 6: { // rigid-ish transform with translation row
      for (int i = 0; i < 12; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
      m[12] = frac(); m[13] = frac(); m[14] = frac(); m[15] = 1.0f;
      break;
    }
    case 7: m[r.below(16)] = r.chance(0.5) ? INFINITY : -INFINITY; break;                      // inf entry
    case 8: for (int i = 0; i < 16; ++i) m[i] = r.chance(0.2) ? (r.chance(0.5) ? 0.0f : -0.0f) : fb(r.float_bits()); break;  // special values
    case 9: for (int i = 0; i < 16; ++i) m[i] = fb(0x00000001u + r.below(0x800000u)) * (r.chance(0.5) ? 1 : -1); break;  // denormals
    case 10: for (int i = 0; i < 16; ++i) m[i] = fb(0x7E800000u + r.below(0x01000000u)) * (r.chance(0.5) ? 1 : -1); break;  // huge: overflow/inf
    case 11: for (int i = 0; i < 16; ++i) m[i] = fb(0x1A000000u + r.below(0x03000000u)); break;  // tiny: underflow to denormal
    case 12: for (int i = 0; i < 16; ++i) m[i] = fb(r.u32() & 0xBFFFFFFFu); break;              // random normal-ish bits
    default: break;
  }
  uint32_t bits[16];
  for (int i = 0; i < 16; ++i) std::memcpy(&bits[i], &m[i], 4);
  for (int i = 0; i < 16; ++i) me::hot::St32(base, src + 4 * i, bits[i]);
  };
  // The sign/payload of a NaN result is only defined when at most one distinct NaN meets in an operation (which operand
  // an FPU instruction returns when two NaNs meet depends on the operand order the compiler happened to pick, also in the
  // original). A NaN determinant (always -NaN from the fnmsubs negation) meets the default NaNs of the other products, so such
  // inputs are replaced by an ordinary matrix; NaN *inputs* are therefore not generated either.
  ctx.r3.u64 = dst | (r.chance(0.15) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0);
  ctx.r4.u64 = src | (r.chance(0.15) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0);
  // sometimes make the compare constant equal to the actual determinant (non-zero equal branch)
  {
    double det;
    for (int attempt = 0;; ++attempt) {
      build();
      PPCContext tmp;
      tmp.r1.u64 = ctx.r1.u64 - 0x2000;  // scratch area, away from the data
      tmp.r3.u64 = src;
      tmp.fpscr.csr = ctx.fpscr.getcsr();
      __imp__sub_822634D8(tmp, base);
      det = tmp.f1.f64;
      if (det == det) break;
      n_nan++;
      kind = 99;
    }
    if (r.chance(0.05)) {
      float fd = float(det);
      std::memcpy(&g_cmp, &fd, 4);
      put_consts(base);
    }
    if (det == double(fb(g_cmp))) n_cmp_eq++;
    if (det == 0.0) n_det0++;
  }
  n_gen++;
}
}  // namespace c822631E8
fuzz::Registrar reg_822631E8({"sub_822631E8", __imp__sub_822631E8, c822631E8::nat, me::hot::n_822631E8::Writes,
                              me::hot::n_822631E8::kCmp, 0x40000000, 0x20000, c822631E8::gen});
}  // namespace

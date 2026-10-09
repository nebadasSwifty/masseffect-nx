// ORIG: sub_82262EC0 sub_822631B0
// NATIVE: n_822631E8.h
// Russian edition copy of tests/hot_fuzz/cases/case_822631E8.inc (RU sub_82262EC0 / sub_822631B0, English sub_822631E8 /
// sub_822634D8); the guest constants come from the RU n_822631E8.h.
// sub_82262EC0: 4x4 float matrix inverse. The two constants it reads (1.0f, the compare constant 0.0f) and the 64-byte
// identity block live outside the fuzz window, so the generator writes them into arena A and the native wrapper writes
// the same bytes into arena B (the harness only copies the window).
// Special float values (+-inf, quiet/signalling NaNs with random payloads and signs, +-0, denormals, huge values that
// overflow) are generated on purpose: a NaN determinant or NaN result must make the native version decline (the
// original runs), because the NaN sign/payload depends on the compiled operand order (console guard failure 2026-10-09:
// "memory +0 native=0x7f original=0xff" from a -inf input).
extern "C" void __imp__sub_82262EC0(PPCContext&, uint8_t*);
extern "C" void __imp__sub_822631B0(PPCContext&, uint8_t*);
namespace {
namespace c822631E8 {

uint32_t g_one = 0x3F800000, g_cmp = 0, g_ident[16];  // host-order bit patterns of the guest constants
uint64_t n_gen = 0, n_det0 = 0, n_cmp_eq = 0, n_nan = 0, n_inf = 0, n_alias = 0;
struct Stat {
  ~Stat() {
    std::printf("   [82262EC0 coverage: gen=%llu det==0 branch=%llu det==cmp(nonzero)=%llu nan-det=%llu inf-det=%llu dst-overlaps-src=%llu]\n",
                (unsigned long long)n_gen, (unsigned long long)n_det0, (unsigned long long)n_cmp_eq,
                (unsigned long long)n_nan, (unsigned long long)n_inf, (unsigned long long)n_alias);
  }
} stat;
void put_consts(uint8_t* base) {
  me::hot::St32(base, me::hot::n_822631E8::kR, g_one);
  me::hot::St32(base, me::hot::n_822631E8::kR - 15532, g_cmp);
  for (int i = 0; i < 16; ++i) me::hot::St32(base, me::hot::n_822631E8::kIdent + 4 * i, g_ident[i]);
}
void nat(PPCContext& ctx, uint8_t* base) {
  put_consts(base);
  if (!me::hot::n_822631E8::Native(ctx, base)) __imp__sub_82262EC0(ctx, base);  // declined (NaN): the original
}
float fb(uint32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}
// one special value: +-0, +-inf, quiet or signalling NaN with a random payload and sign, denormal, huge, max/min normal
uint32_t special_bits(fuzz::Rng& r) {
  const uint32_t sign = r.chance(0.5) ? 0x80000000u : 0u;
  switch (r.below(9)) {
    case 0: return sign;                                                    // +-0
    case 1: return sign | 0x7F800000u;                                      // +-inf
    case 2: return sign | 0x7FC00000u | (r.u32() & 0x3FFFFFu);              // quiet NaN, payload
    case 3: return sign | 0x7F800000u | (1u + r.below(0x3FFFFFu));          // signalling NaN, payload
    case 4: return sign | 0x7FC00000u;                                      // default-like NaN
    case 5: return sign | (1u + r.below(0x7FFFFFu));                        // denormal
    case 6: return sign | (0x7E800000u + r.below(0x00FFFFFFu));             // huge (products overflow)
    case 7: return sign | (r.chance(0.5) ? 0x7F7FFFFFu : 0x00800000u);      // max / min normal
    default: return sign | 0x3F800000u;                                     // +-1
  }
}
void gen(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  const uint32_t win = 0x40000000;
  // constants: usually the real ones (1.0f, 0.0f, identity); sometimes perturbed
  g_one = r.chance(0.9) ? 0x3F800000u : (r.chance(0.5) ? special_bits(r) : r.float_bits());
  g_cmp = r.chance(0.9) ? 0u : (r.chance(0.5) ? special_bits(r) : r.float_bits());
  for (int i = 0; i < 16; ++i) g_ident[i] = (i % 5 == 0) ? 0x3F800000u : (r.chance(0.97) ? 0u : r.float_bits());
  put_consts(base);

  const uint32_t src = win + 0x1000 + (r.chance(0.8) ? r.below(0x800) * 16 : r.below(0x2000));
  uint32_t dst = win + 0x9000 + (r.chance(0.8) ? r.below(0x800) * 16 : r.below(0x2000));
  if (r.chance(0.12)) dst = src;                               // in-place
  else if (r.chance(0.08)) dst = src + 4 * r.range(0, 24) - 48;  // partial overlap
  if (dst != src && dst + 64 > src && src + 64 > dst) n_alias++;
  float m[16];
  uint32_t kind = r.below(26);
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
    case 13: m[r.below(16)] = fb(special_bits(r)); break;                                       // one special entry
    case 14: for (int i = 0, n = 1 + r.below(4); i < n; ++i) m[r.below(16)] = fb(special_bits(r)); break;  // a few
    case 15: for (int i = 0; i < 16; ++i) m[i] = r.chance(0.3) ? fb(special_bits(r)) : m[i]; break;  // many
    case 16: { // -inf / +inf next to 1.0 and 0 (the console failure: r9 held 0xff800000'3f800000)
      for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
      m[r.below(16)] = r.chance(0.5) ? -INFINITY : INFINITY;
      break;
    }
    case 17: m[r.below(16)] = fb((r.chance(0.5) ? 0x7FC00000u : 0xFFC00000u) | (r.u32() & 0x3FFFFFu)); break;  // one NaN
    case 18: for (int i = 0; i < 16; ++i) m[i] = fb(r.float_bits()); break;                    // harness mix (incl. NaN)
    default: break;
  }
  uint32_t bits[16];
  for (int i = 0; i < 16; ++i) std::memcpy(&bits[i], &m[i], 4);
  for (int i = 0; i < 16; ++i) me::hot::St32(base, src + 4 * i, bits[i]);
  };
  // NaN determinants and NaN results are kept: the native version delegates them to the original (see n_822631E8.h).
  ctx.r3.u64 = dst | (r.chance(0.15) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0);
  ctx.r4.u64 = src | (r.chance(0.15) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0);
  // sometimes make the compare constant equal to the actual determinant (non-zero equal branch)
  {
    build();
    PPCContext tmp;
    tmp.r1.u64 = ctx.r1.u64 - 0x2000;  // scratch area, away from the data
    tmp.r3.u64 = src;
    tmp.fpscr.csr = ctx.fpscr.getcsr();
    __imp__sub_822631B0(tmp, base);
    const double det = tmp.f1.f64;
    if (det != det) n_nan++;
    if (std::isinf(det)) n_inf++;
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
fuzz::Registrar reg_822631E8({"sub_82262EC0", __imp__sub_82262EC0, c822631E8::nat, me::hot::n_822631E8::Writes,
                              me::hot::n_822631E8::kCmp, 0x40000000, 0x20000, c822631E8::gen});
}  // namespace

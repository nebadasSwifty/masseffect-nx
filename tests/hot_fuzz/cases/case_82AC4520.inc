// ORIG: sub_82AC4520
// NATIVE: n_82AC4520.h
// wcscmp: two UTF-16 (big-endian) strings, equal / one unit different / prefix / different case, any alignment, page edges.
extern "C" void __imp__sub_82AC4520(PPCContext&, uint8_t*);
namespace {
inline uint16_t wc_unit(fuzz::Rng& r) {
  static const uint16_t kPat[] = {0x0001, 0x0100, 0x00FF, 0xFF00, 0x8000, 0x0080, 0x0101, 0xFFFF, 0x7FFF, 0x0002, 0x0200, 0x8001,
                                  0x0180, 0x0040, 0x0041, 0x005A, 0x005B, 0x0060, 0x0061, 0x007A, 0x007B, 0x0141, 0xFF41, 0x00C0};
  uint16_t v;
  switch (r.below(5)) {
    case 0: v = kPat[r.below(sizeof kPat / sizeof kPat[0])]; break;
    case 1: case 2: v = uint16_t(0x41 + r.below(26) + (r.chance(0.5) ? 32 : 0)); break;
    case 3: v = uint16_t(0x20 + r.below(0x5F)); break;
    default: v = uint16_t(r.u32()); break;
  }
  return v ? v : uint16_t(0x0100);
}
inline void wc_put(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
inline uint16_t wc_get(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
// pick a start address with a chosen alignment relation / page-edge proximity
inline uint32_t wc_addr(fuzz::Rng& r, uint32_t lo, uint32_t span) {
  uint32_t p = lo + r.below(span);
  switch (r.below(6)) {
    case 0: p |= 1; break;                      // odd
    case 1: p = (p & ~7u) + r.below(4) * 2; break;  // 8-byte block + even offset
    case 2: p = (p & ~0xFFFu) + 0x1000 - 2 * r.range(0, 24); break;  // just before a 4 KB boundary
    case 3: p &= ~7u; break;
    default: p &= ~1u; break;
  }
  return p;
}
void gen_82AC4520(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  const uint32_t win = 0x40000000;
  uint32_t len;
  switch (r.below(6)) {
    case 0: len = r.below(4); break;
    case 1: len = r.below(12); break;
    case 2: len = r.chance(0.2) ? r.below(3000) : r.below(200); break;
    default: len = r.below(40); break;
  }
  const uint32_t pa = wc_addr(r, win + 0x1000, 0x4000);
  const uint32_t pb = r.chance(0.08) ? pa : wc_addr(r, win + 0x6000, 0x4000);  // pa == pb: identical strings
  // string a
  for (uint32_t i = 0; i < len; ++i) wc_put(base + pa + 2 * i, wc_unit(r));
  wc_put(base + pa + 2 * len, 0);
  // string b = copy of a with a mutation (or an unrelated string)
  if (pb != pa) {
    uint32_t lb = len;
    if (r.chance(0.1)) {  // unrelated
      lb = r.below(20);
      for (uint32_t i = 0; i < lb; ++i) wc_put(base + pb + 2 * i, wc_unit(r));
    } else {
      for (uint32_t i = 0; i < len; ++i) wc_put(base + pb + 2 * i, wc_get(base + pa + 2 * i));
      switch (r.below(8)) {
        case 0: case 1: break;  // equal
        case 2: case 3: case 4:  // one unit changed (bit flip / other unit), possibly into the terminator
          if (len) {
            const uint32_t k = r.below(len);
            uint16_t v = wc_get(base + pb + 2 * k);
            v = r.chance(0.3) ? uint16_t(v ^ (1u << r.below(16))) : r.chance(0.5) ? uint16_t(v + (r.chance(0.5) ? 1 : -1)) : wc_unit(r);
            wc_put(base + pb + 2 * k, v);
            if (r.chance(0.1)) wc_put(base + pb + 2 * k, 0);
          }
          break;
        case 5:  // b shorter
          if (len) lb = r.below(len);
          break;
        case 6:  // b longer
          for (uint32_t i = 0; i < r.range(1, 6); ++i) wc_put(base + pb + 2 * (lb++), wc_unit(r));
          break;
        default:  // the last unit differs
          if (len) wc_put(base + pb + 2 * (len - 1), uint16_t(wc_get(base + pb + 2 * (len - 1)) ^ 0x2020));
          break;
      }
      wc_put(base + pb + 2 * lb, 0);
    }
    wc_put(base + pb + 2 * lb, 0);
  }
  // keep non-zero garbage after the terminators sometimes (random fill already does)
  ctx.r3.u64 = pa | (r.chance(0.3) ? (r.u64() << 32) : 0);
  ctx.r4.u64 = pb | (r.chance(0.3) ? (r.u64() << 32) : 0);
  if (r.chance(0.5)) std::swap(ctx.r3.u64, ctx.r4.u64);
}
fuzz::Registrar reg_82AC4520({"sub_82AC4520", __imp__sub_82AC4520, me::hot::n_82AC4520::Native, me::hot::n_82AC4520::Writes,
                              me::hot::n_82AC4520::kCmp, 0x40000000, 0x20000, gen_82AC4520});
}  // namespace

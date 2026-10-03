// ORIG: sub_82AC4058
// NATIVE: n_82AC4058.h
extern "C" void __imp__sub_82AC4058(PPCContext&, uint8_t*);
namespace {
// a non-zero UTF-16 unit (big-endian bytes at p), biased towards units with one zero byte and carry-prone patterns
inline void put_unit_nz(fuzz::Rng& r, uint8_t* p) {
  static const uint16_t kPat[] = {0x0001, 0x0100, 0x00FF, 0xFF00, 0x8000, 0x0080, 0x0101, 0xFFFF, 0x7FFF, 0x0002, 0x0200, 0x8001, 0x0180};
  uint16_t v;
  switch (r.below(4)) {
    case 0: v = kPat[r.below(sizeof kPat / sizeof kPat[0])]; break;
    case 1: v = uint16_t(0x20 + r.below(0x5F)); break;  // ASCII
    default: v = uint16_t(r.u32()); break;
  }
  if (v == 0) v = 0x0100;
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
void gen_82AC4058(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  const uint32_t win = 0x40000000;
  uint32_t len;
  switch (r.below(8)) {
    case 0: len = r.below(5); break;
    case 1: len = r.below(12); break;
    case 2: len = r.chance(0.2) ? r.below(6000) : r.below(300); break;
    default: len = r.below(60); break;
  }
  uint32_t p = win + 0x1000 + r.below(0x8000);
  p = r.chance(0.1) ? (p | 1) : (p & ~1u);
  if (r.chance(0.5)) p = (p & ~7u) + r.range(0, 6) / 2 * 2 + (r.chance(0.05) ? 1 : 0);  // all head lengths
  if (r.chance(0.1)) p = (p & ~0xFFFu) + 0x1000 - 2 * r.range(0, 5);  // string end / start next to a 4 KB boundary
  for (uint32_t i = 0; i < len; ++i) put_unit_nz(r, base + p + 2 * i);
  base[p + 2 * len] = 0;
  base[p + 2 * len + 1] = 0;
  // terminated, but sometimes followed by non-zero data and by more strings
  ctx.r3.u64 = p | (r.chance(0.3) ? (r.u64() << 32) : 0);
}
fuzz::Registrar reg_82AC4058({"sub_82AC4058", __imp__sub_82AC4058, me::hot::n_82AC4058::Native, me::hot::n_82AC4058::Writes,
                              me::hot::n_82AC4058::kCmp, 0x40000000, 0x20000, gen_82AC4058});
}  // namespace

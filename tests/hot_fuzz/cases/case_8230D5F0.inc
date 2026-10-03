// ORIG: sub_8230D5F0
// NATIVE: n_8230D5F0.h
extern "C" void __imp__sub_8230D5F0(PPCContext&, uint8_t*);
namespace {
void put32_8230D5F0(uint8_t* base, uint32_t addr, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + addr, &v, 4);
}
// One table at `t`: buckets at `t + 0x200`, entries at `t + 0x400` (up to 8 entries of 16 bytes); returns nothing.
void make_table_8230D5F0(fuzz::Rng& r, uint8_t* base, uint32_t t, uint32_t next, uint32_t hi, uint32_t lo) {
  const uint32_t nbk = 1u << r.below(4);
  const uint32_t ne = r.range(1, 8);
  const uint32_t buckets = t + 0x200, entries = t + 0x400;
  put32_8230D5F0(base, t + 60, next);
  put32_8230D5F0(base, t + 164, entries);
  put32_8230D5F0(base, t + 168, r.chance(0.05) ? (r.chance(0.5) ? 0 : uint32_t(-int32_t(r.below(3)))) : ne);
  put32_8230D5F0(base, t + 176, r.chance(0.05) ? 0 : buckets);
  put32_8230D5F0(base, t + 180, r.chance(0.04) ? (r.chance(0.5) ? 0 : r.u32()) : nbk);
  for (uint32_t b = 0; b < nbk; ++b) put32_8230D5F0(base, buckets + b * 4, r.chance(0.2) ? 0xFFFFFFFFu : r.below(ne));
  for (uint32_t e = 0; e < ne; ++e) {
    const uint32_t a = entries + e * 16;
    const uint32_t nx = (e + 1 < ne && !r.chance(0.35)) ? r.range(e + 1, ne - 1) : 0xFFFFFFFFu;  // acyclic
    put32_8230D5F0(base, a + 0, nx);
    uint32_t h = hi, l = lo;
    uint32_t m = r.below(10);
    if (m < 3) {  // other key
      h = r.chance(0.5) ? hi : r.below(16);
      l = r.chance(0.5) ? lo : r.u32();
      if (h == hi && l == lo) l ^= 1;
    } else if (m == 3) {  // hi matches, lo off by one
      l = lo + 1;
    } else if (m == 4) {  // lo matches only
      h = hi ^ 0x80;
    }
    put32_8230D5F0(base, a + 4, h);
    put32_8230D5F0(base, a + 8, l);
    put32_8230D5F0(base, a + 12, r.chance(0.3) ? 0 : (r.chance(0.5) ? r.u32() : 0x40000000u + r.below(0x1000)));
  }
}
void gen_8230D5F0(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  const uint32_t win = 0x40000000;
  uint32_t hi = r.chance(0.9) ? r.below(16) : r.u32();
  uint32_t lo = r.chance(0.7) ? r.below(8) : r.u32();
  const uint32_t obj = win + 0x100;
  const uint32_t owner = win + 0x200;
  const uint32_t kTab = win + 0x1000;  // 8 tables of 0x800 bytes
  uint32_t order[8];
  for (int i = 0; i < 8; ++i) order[i] = i;
  for (int i = 7; i > 0; --i) std::swap(order[i], order[r.below(i + 1)]);
  uint32_t n1 = r.range(1, 4), n2 = r.chance(0.2) ? 0 : r.range(1, 4);
  uint32_t tabs[8];
  for (int i = 0; i < 8; ++i) tabs[i] = kTab + order[i] * 0x800;
  // chain 1: tabs[0..n1), chain 2: tabs[4..4+n2) (or sharing the first chain's tables: acyclic by construction)
  for (uint32_t i = 0; i < n1; ++i) make_table_8230D5F0(r, base, tabs[i], i + 1 < n1 ? tabs[i + 1] : 0, hi, lo);
  uint32_t first2 = 0;
  if (n2) {
    if (r.chance(0.1)) {
      first2 = tabs[r.below(n1)];  // chain 2 enters chain 1 somewhere
    } else {
      for (uint32_t i = 0; i < n2; ++i) make_table_8230D5F0(r, base, tabs[4 + i], i + 1 < n2 ? tabs[4 + i + 1] : 0, hi, lo);
      first2 = tabs[4];
    }
  }
  put32_8230D5F0(base, obj + 24, r.chance(0.1) ? 0 : owner);
  put32_8230D5F0(base, owner + 32, r.chance(0.1) ? 0 : tabs[0]);
  put32_8230D5F0(base, obj + 52, first2);
  ctx.r3.u64 = obj | (r.chance(0.2) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0);
  ctx.r4.u64 = (uint64_t(hi) << 32) | lo;
  ctx.r5.u64 = r.chance(0.8) ? (r.chance(0.3) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0) : r.u64() | 1;
}
fuzz::Registrar reg_8230D5F0({"sub_8230D5F0", __imp__sub_8230D5F0, me::hot::n_8230D5F0::Native, me::hot::n_8230D5F0::Writes,
                              me::hot::n_8230D5F0::kCmp, 0x40000000, 0x20000, gen_8230D5F0});
}  // namespace

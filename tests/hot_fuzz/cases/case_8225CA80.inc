// ORIG: sub_8225CA80
// NATIVE: n_8225CA80.h
extern "C" void __imp__sub_8225CA80(PPCContext&, uint8_t*);
namespace {
void put32_8225CA80(uint8_t* base, uint32_t addr, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + addr, &v, 4);
}
void gen_8225CA80(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  const uint32_t win = 0x82EAC000;
  const uint32_t cont = win + 0x100;
  const uint32_t grpArr = win + 0x200;
  // 8 groups, each with an item array of 12 entries, items with the skip bit random
  const uint32_t kItems = 16;
  for (uint32_t i = 0; i < kItems; ++i) {
    const uint32_t a = win + 0x1800 + i * 0x90;
    uint32_t v = r.u32();
    if (r.chance(0.5)) v |= 0x80000000u; else v &= 0x7FFFFFFFu;
    put32_8225CA80(base, a + 132, v);
  }
  for (uint32_t g = 0; g < 8; ++g) {
    const uint32_t ga = win + 0xA00 + g * 0x180;
    const uint32_t ia = win + 0x300 + g * 0x40;
    put32_8225CA80(base, grpArr + g * 4, ga);
    put32_8225CA80(base, ga + 60, ia);
    uint32_t cnt = r.chance(0.1) ? r.below(3) : r.range(0, 7);
    if (r.chance(0.03)) cnt = uint32_t(-int32_t(r.below(3)));
    put32_8225CA80(base, ga + 64, cnt);
    put32_8225CA80(base, ga + 216, r.chance(0.7) ? r.below(3) : uint32_t(int32_t(r.below(10)) - 3));
    put32_8225CA80(base, ga + 364, r.chance(0.5) ? 0 : (r.chance(0.5) ? 1 : r.u32()));
    for (uint32_t e = 0; e < 12; ++e) {
      uint32_t p = 0;
      if (!r.chance(0.15)) p = win + 0x1800 + r.below(kItems) * 0x90;
      put32_8225CA80(base, ia + e * 4, p);
    }
  }
  put32_8225CA80(base, cont + 72, grpArr);
  uint32_t gcount = r.chance(0.1) ? r.below(2) : r.range(1, 8);
  if (r.chance(0.02)) gcount = uint32_t(-int32_t(r.below(3)));
  put32_8225CA80(base, cont + 76, gcount);
  put32_8225CA80(base, 0x82EAEA94, cont);
  put32_8225CA80(base, 0x82EAC8D8, r.chance(0.5) ? 0 : r.u32());
  // iterator
  uint32_t it = win + 0x2C00 + r.below(0x80) * 16;
  if (r.chance(0.01)) it = win + 0xA00 + r.below(8) * 0x180 + 8 * r.below(8);  // aliases a group
  else if (r.chance(0.01)) it = cont + 64 + 4 * r.below(6);                    // aliases the container
  else if (r.chance(0.01)) it = grpArr - 8 + 4 * r.below(6);
  put32_8225CA80(base, it + 0, uint32_t(int32_t(r.below(9)) - 2));
  uint32_t o = r.below(10) < 8 ? r.below(gcount + 2) : uint32_t(int32_t(r.below(6)) - 2);
  put32_8225CA80(base, it + 4, o);
  put32_8225CA80(base, it + 8, r.chance(0.8) ? 0 : (r.chance(0.5) ? 1 : r.u32()));
  put32_8225CA80(base, it + 12, r.u32());
  put32_8225CA80(base, it + 16, r.chance(0.5) ? 0 : r.u32());
  ctx.r3.u64 = it | (r.chance(0.3) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0);
}
fuzz::Registrar reg_8225CA80({"sub_8225CA80", __imp__sub_8225CA80, me::hot::n_8225CA80::Native, me::hot::n_8225CA80::Writes,
                              me::hot::n_8225CA80::kCmp, 0x82EAC000, 0x4000, gen_8225CA80});
}  // namespace

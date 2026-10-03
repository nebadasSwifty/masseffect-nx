// ORIG: sub_824F9D10
// NATIVE: n_824F9D10.h
extern "C" void __imp__sub_824F9D10(PPCContext&, uint8_t*);
namespace {
void gen_824F9D10(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  const uint32_t win = 0x40000000;
  const uint32_t dev = win + 0x100 + r.below(0x400) * 16;
  const uint32_t desc = win + 0x8000 + r.below(0x1000) * 4;
  uint32_t src = win + 0x9000 + r.below(0x1000) * 4;
  const uint32_t reg = r.chance(0.9) ? r.below(300) : r.below(0x10000);
  // keep dev + reg*16 + 5900 inside the window
  const uint32_t reg2 = (dev + reg * 16 + 6000 < win + 0x1F000 && dev + (reg + 368) * 16 + 16 < win + 0x1F000) ? reg : r.below(100);
  if (r.chance(0.15)) {  // the source float4 overlaps the destination words (the original interleaves its loads and stores)
    const uint32_t a0 = dev + ((reg2 + 368u) << 4), a1 = dev + (reg2 << 4);
    const uint32_t tg[4] = {a0, a1 + 5892, a1 + 5896, a1 + 5900};
    const uint32_t cand = tg[r.below(4)] - 4 * r.below(5);
    if (cand >= win + 0x100 && cand + 16 < win + 0x1F000 && cand != desc && cand + 16 <= desc || cand >= desc + 4) src = cand;
  }
  base[desc] = 0;  // big-endian u16 at desc
  *reinterpret_cast<uint16_t*>(base + desc) = __builtin_bswap16(uint16_t(reg2));
  *reinterpret_cast<uint16_t*>(base + desc + 2) = __builtin_bswap16(uint16_t(r.chance(0.15) ? 0 : r.range(1, 5)));
  for (int i = 0; i < 4; ++i) {
    uint32_t b = __builtin_bswap32(r.float_bits());
    std::memcpy(base + src + 4 * i, &b, 4);
  }
  ctx.r3.u64 = dev;
  ctx.r5.u64 = desc;
  ctx.r6.u64 = src;
}
fuzz::Registrar reg_824F9D10({"sub_824F9D10", __imp__sub_824F9D10, me::hot::n_824F9D10::Native, me::hot::n_824F9D10::Writes,
                              me::hot::n_824F9D10::kCmp, 0x40000000, 0x20000, gen_824F9D10});
}  // namespace

// ORIG: sub_8262CFC0
// NATIVE: n_8262CFC0.h
extern "C" void __imp__sub_8262CFC0(PPCContext&, uint8_t*);
namespace {
void gen_8262CFC0(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  const uint32_t win = 0x40000000;
  const uint32_t obj = win + 0x100;
  const uint32_t planes = win + 0xA000 + (r.chance(0.7) ? r.below(0x1000) * 16 : r.below(0x10000));  // stays inside the window (limit win + 0x20000)
  const uint32_t count = r.chance(0.1) ? r.below(4) : r.range(1, 40);
  const uint32_t p4 = win + 0x8000 + r.below(0x1000);
  const uint32_t p5 = win + 0x9000 + r.below(0x1000);
  auto st32 = [&](uint32_t a, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(base + a, &v, 4);
  };
  st32(obj + 12, planes);
  st32(obj + 16, r.chance(0.05) ? uint32_t(-int32_t(r.below(5))) : count);  // <= 0: the loop is skipped
  // plane data: small ordinary floats most of the time so the tests sometimes pass, sometimes not
  for (uint32_t i = 0; i < 4 * 48; ++i) {
    uint32_t b;
    if (r.chance(0.85)) {
      float f = float(int32_t(r.below(2001)) - 1000) * (r.chance(0.5) ? 0.01f : 1.0f);
      std::memcpy(&b, &f, 4);
    } else {
      b = r.float_bits();
    }
    st32(planes + i * 4, b);
  }
  for (int i = 0; i < 4; ++i) {
    uint32_t b;
    float f = float(int32_t(r.below(2001)) - 1000) * 0.01f;
    std::memcpy(&b, &f, 4);
    st32(p4 + 4 * i, r.chance(0.9) ? b : r.float_bits());
    st32(p5 + 4 * i, r.chance(0.9) ? (b ^ (r.chance(0.3) ? 0x80000000u : 0)) : r.float_bits());
  }
  ctx.r3.u64 = obj;
  ctx.r4.u64 = p4;
  ctx.r5.u64 = p5;
}
fuzz::Registrar reg_8262CFC0({"sub_8262CFC0", __imp__sub_8262CFC0, me::hot::n_8262CFC0::Native, me::hot::n_8262CFC0::Writes,
                              me::hot::n_8262CFC0::kCmp, 0x40000000, 0x20000, gen_8262CFC0});
}  // namespace

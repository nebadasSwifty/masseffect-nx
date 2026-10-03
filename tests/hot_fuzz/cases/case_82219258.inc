// ORIG: sub_82219258
// NATIVE: n_82219258.h
extern "C" void __imp__sub_82219258(PPCContext&, uint8_t*);
namespace {
void gen_82219258(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  const uint32_t win = 0x40000000;
  const uint32_t dev = win + 0x100 + (r.chance(0.8) ? r.below(0x400) * 16 : r.below(0x4000));
  ctx.r3.u64 = dev;
  ctx.r4.u64 = r.chance(0.8) ? r.below(200) : (r.u64() & 0xFFFF);
  const uint32_t n = r.chance(0.9) ? r.below(40) : r.below(300);
  ctx.r6.u64 = n | (r.chance(0.2) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0);
  // destination = dev + (r4 + 112) * 16 must stay inside the window: shrink r4 if not
  while (dev + (uint32_t(ctx.r4.u32 + 112) << 4) + n * 16 + 64 > win + 0x1F000) ctx.r4.u64 >>= 1;
  uint32_t src = win + 0x8000 + r.below(0x6000);
  if (r.chance(0.2)) {  // overlap with the destination
    uint32_t dst = (dev + (uint32_t(ctx.r4.u32 + 112) << 4)) & ~15u;
    src = dst + r.range(0, 128) - 64;
  }
  ctx.r5.u64 = src;
  ctx.r7.u64 = r.u64();
  // sometimes the dirty mask word is inside the copy range
  if (r.chance(0.05)) ctx.r4.u64 = 0xFFFFFF90u + r.below(8);  // (r4 + 112) * 16 wraps to the device itself / a few vectors above
}
fuzz::Registrar reg_82219258({"sub_82219258", __imp__sub_82219258, me::hot::n_82219258::Native,
                              me::hot::n_82219258::Writes, me::hot::n_82219258::kCmp, 0x40000000, 0x20000, gen_82219258});
}  // namespace

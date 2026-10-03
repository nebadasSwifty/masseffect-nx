// ORIG: sub_82AC9C30
// NATIVE: n_82AC9C30.h
// wcsncpy: src string shorter than / equal to / longer than n, overlap with dst < src (equals memmove) and dst > src (declined).
extern "C" void __imp__sub_82AC9C30(PPCContext&, uint8_t*);
namespace {
void gen_82AC9C30(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  const uint32_t win = 0x40000000;
  uint32_t n;
  switch (r.below(6)) {
    case 0: n = r.below(4); break;
    case 1: n = r.below(20); break;
    case 2: n = r.chance(0.2) ? r.below(3000) : r.below(300); break;
    default: n = r.below(80); break;
  }
  uint32_t k;  // index of the first zero unit of src
  switch (r.below(6)) {
    case 0: k = n; break;
    case 1: k = n ? n - 1 : 0; break;
    case 2: k = n + r.below(5); break;
    case 3: k = n > 0 ? n - 1 - r.below(std::min<uint32_t>(n, 3)) : 0; break;
    default: k = r.below(n + 3); break;
  }
  const uint32_t lo = win + 0x1000, span = 0x10000;
  uint32_t src = lo + r.below(span);
  src = r.chance(0.2) ? (src | 1) : (src & ~1u);
  uint32_t dst;
  switch (r.below(6)) {
    case 0: dst = src - std::min<uint32_t>(r.range(1, 40), src - lo); break;  // dst < src
    case 1: dst = src + r.range(1, 40); break;                                // dst > src, overlap (declined) or adjacent
    case 2: dst = src + 2 * k + 2 + r.below(4); break;                        // just after the source string
    case 3: dst = src; break;
    default: dst = lo + 0x11000 + r.below(0x4000); dst = r.chance(0.2) ? (dst | 1) : dst; break;
  }
  for (uint32_t i = 0; i < k + 1; ++i) {
    uint16_t v = i == k ? 0 : uint16_t(r.chance(0.3) ? (0x41 + r.below(26)) : r.u32());
    if (i != k && v == 0) v = 1;
    base[src + 2 * i] = uint8_t(v >> 8);
    base[src + 2 * i + 1] = uint8_t(v);
  }
  ctx.r3.u64 = dst | (r.chance(0.3) ? (r.u64() << 32) : 0);
  ctx.r4.u64 = src | (r.chance(0.3) ? (r.u64() << 32) : 0);
  ctx.r5.u64 = n | (r.chance(0.3) ? (r.u64() << 32) : 0);
}
fuzz::Registrar reg_82AC9C30({"sub_82AC9C30", __imp__sub_82AC9C30,
                              fuzz::NativeAdapter<me::hot::n_82AC9C30::Native, __imp__sub_82AC9C30>,
                              me::hot::n_82AC9C30::Writes, me::hot::n_82AC9C30::kCmp, 0x40000000, 0x20000, gen_82AC9C30});
}  // namespace

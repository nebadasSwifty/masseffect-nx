// ORIG: sub_829730C0
// NATIVE: n_82AC4A50.h
// Russian edition case: the original is the RU function (sub_829730C0 = English sub_82AC4A50).
// XDK CRT memset (see n_82AC4A50.h): every destination alignment, lengths 0..64 (head/tail paths), up to a few KB, garbage
// in the upper halves of r4 / r5 and in the bits 8..31 of r4.
extern "C" void __imp__sub_829730C0(PPCContext&, uint8_t*);
namespace {
namespace c82AC4A50 {
constexpr uint32_t kWin = 0x40000000, kLen = 0x10000;
uint64_t n_calls = 0, n_zero = 0, n_small = 0;
void nat_w(PPCContext& ctx, uint8_t* base) {
  fuzz::NativeAdapter<me::hot::n_82AC4A50::Native, __imp__sub_829730C0>(ctx, base);
}
void gen(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  (void)base;
  ++n_calls;
  uint32_t n;
  switch (r.below(6)) {
    case 0: n = 0; break;
    case 1: case 2: n = r.below(65); break;
    case 3: n = r.below(300); break;
    default: n = r.below(0x2000);
  }
  if (n == 0) ++n_zero;
  else if (n <= 64) ++n_small;
  const uint32_t dst = kWin + 0x100 + r.below(0x4000);
  ctx.r3.u64 = (r.chance(0.2) ? (uint64_t(r.u32()) << 32) : 0) | dst;
  ctx.r4.u64 = r.chance(0.5) ? r.below(256) : r.u64();
  ctx.r5.u64 = (r.chance(0.3) ? (uint64_t(r.u32()) << 32) : 0) | n;
}
struct Stat {
  ~Stat() {
    std::printf("   [82AC4A50 coverage: calls=%llu n=0: %llu, n<=64: %llu]\n", (unsigned long long)n_calls,
                (unsigned long long)n_zero, (unsigned long long)n_small);
  }
} stat;
}  // namespace c82AC4A50
fuzz::Registrar reg_82AC4A50({"sub_829730C0", __imp__sub_829730C0, c82AC4A50::nat_w, me::hot::n_82AC4A50::Writes,
                              me::hot::n_82AC4A50::kCmp, c82AC4A50::kWin, c82AC4A50::kLen, c82AC4A50::gen});
}  // namespace

// ORIG: sub_82212540 sub_822124E0
// NATIVE: n_82212540.h
// sub_82212540: TArray<uint32>::Add. Fast path (num < max) native, slow path (grow: critical section + a virtual call per
// element + RtlLeaveCriticalSection) declined by the native version, i.e. the adapter runs the original.
// Environment: the Rtl* critical section imports are stubs (TryEnter fails twice, then succeeds, to cover the retry loop of
// sub_822124E0), the global object / its vtable / the virtual method live in the window / the dispatch table (guest code
// address 0x82210000 + 4 * k -> host stub), the globals at 0x82EB0000 - 1208/-1204/-1200 are written into both arenas.
extern "C" void __imp__sub_82212540(PPCContext&, uint8_t*);
extern "C" void __imp__RtlTryEnterCriticalSection(PPCContext& c, uint8_t*);
extern "C" void __imp__RtlLeaveCriticalSection(PPCContext& c, uint8_t*);
namespace rex::runtime {
__attribute__((weak)) PPCFunc* ResolveIndirectFunction(uint32_t) { return nullptr; }
}  // namespace rex::runtime
namespace {
namespace c82212540 {
constexpr uint32_t kWin = 0x40000000, kGlob = 0x82EB0000u - 1208u;
constexpr uint32_t kCs = kWin + 0x1E000, kQ = kWin + 0x1E100, kVt = kWin + 0x1E200, kTarget = 0x82210000u + 4 * 77;
uint32_t g_try = 0;
uint64_t n_fast = 0, n_slow = 0, n_alias = 0, n_try_fail = 0, n_vcalls = 0;
}  // namespace c82212540
}  // namespace
extern "C" void __imp__RtlTryEnterCriticalSection(PPCContext& c, uint8_t*) {
  using namespace c82212540;
  const bool ok = (g_try++ % 3) == 2;
  if (!ok) ++n_try_fail;
  c.r3.u64 = ok ? 1 : 0;
  c.r4.u64 = 0xDEAD0004DEAD0004ull; c.r5.u64 = 0xDEAD0005DEAD0005ull; c.r11.u64 = 0xDEAD0011DEAD0011ull;
}
extern "C" void __imp__RtlLeaveCriticalSection(PPCContext& c, uint8_t*) {
  c.r3.u64 = 0xDEAD0003DEAD0003ull; c.r4.u64 = 0xDEAD0004DEAD0004ull; c.r11.u64 = 0xDEAD0011DEAD0011ull;
}
namespace {
namespace c82212540 {
void VStub(PPCContext& c, uint8_t*) {  // virtual method of the global object: (obj, element)
  ++n_vcalls;
  c.r3.u64 = 0xDEAD0003DEAD0003ull; c.r4.u64 = 0xDEAD0004DEAD0004ull; c.r10.u64 = 0xDEAD0010DEAD0010ull;
  c.r11.u64 = 0xDEAD0011DEAD0011ull;
}
void Env(uint8_t* base) {
  me::hot::St32(base, kGlob, kCs);       // r11 = [-1208]: critical section holder (r11 + 4 = the lock)
  me::hot::St32(base, kGlob + 4, kQ);    // [-1204]: global object
  me::hot::St32(base, kGlob + 8, 0);     // [-1200]: spin counter (a plain global; changes only on failed TryEnter)
  *reinterpret_cast<PPCFunc**>(base + 0x82000000ull + 0xFC0000ull + (uint64_t(kTarget) - 0x82210000ull) * 2) = VStub;
}
void orig_w(PPCContext& ctx, uint8_t* base) { Env(base); g_try = 0; __imp__sub_82212540(ctx, base); }
void nat_w(PPCContext& ctx, uint8_t* base) {
  Env(base);
  g_try = 0;
  fuzz::NativeAdapter<me::hot::n_82212540::Native, __imp__sub_82212540>(ctx, base);
}
void gen(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  const uint32_t a = kWin + 0x100 + r.below(0x400) * 16;
  const uint32_t data = kWin + 0x8000 + r.below(0x1000) * 4;
  int32_t max = r.chance(0.7) ? int32_t(r.range(1, 100)) : int32_t(r.u32());
  int32_t num = r.chance(0.8) ? int32_t(r.below(max > 0 && max < 200 ? uint32_t(max) : 100)) : int32_t(r.u32());
  if (r.chance(0.1)) num = max;
  if (r.chance(0.05)) num = max - 1;
  if (r.chance(0.03)) num = max = 0;
  if (r.chance(0.02)) { num = INT32_MAX; max = INT32_MIN; }
  // (the benchmark repeats a call on the changed state: keep num small so that growth never walks a huge array)
  if (num < max && (num > 250 || num < -300)) num = int32_t(r.below(100)) - (r.chance(0.2) ? 100 : 0);
  uint32_t d = data;
  if (r.chance(0.05)) d = a + 12 - (uint32_t(num) << 2);  // data[num] aliases the num word
  if (r.chance(0.04)) d = a + 16 - (uint32_t(num) << 2);  // aliases max
  if (r.chance(0.04)) d = a - (uint32_t(num) << 2);       // aliases the data pointer
  // the fast-path store and the slow-path element reads must stay in the window
  auto inwin = [&](uint32_t x) { return x >= kWin + 0x100 && x + 4 <= kWin + 0x1D000; };
  if (num < max) {
    if (!inwin(d + (uint32_t(num) << 2))) { d = data; if (num < 0 || num > 200) num = int32_t(r.below(50)); if (num >= max) max = num + 1; }
  } else {  // slow path: the grow loop walks the elements d[0..num): keep them in the window
    if (!(num >= 0 && num < 100 && inwin(d) && inwin(d + 4u * uint32_t(num)))) {
      d = data;
      num = int32_t(r.below(60));
      max = num - int32_t(r.below(2));
    }
  }
  me::hot::St32(base, a + 0, d);
  me::hot::St32(base, a + 12, uint32_t(num));
  me::hot::St32(base, a + 16, uint32_t(max));
  Env(base);
  me::hot::St32(base, kQ, kVt);  // vtable of the global object
  for (int i = 0; i < 8; ++i) me::hot::St32(base, kVt + 4 * i, i == 3 ? kTarget : r.u32());
  for (int i = 0; i < 8; ++i) me::hot::St32(base, kCs + 4 * i, r.u32());
  if (num < max) ++n_fast; else ++n_slow;
  if (num < max && uint32_t(d + (uint32_t(num) << 2)) - a < 20) ++n_alias;
  ctx.r3.u64 = a;
  ctx.r4.u64 = r.u64();
}
struct Stat {
  ~Stat() {
    std::printf("   [82212540 coverage: fast=%llu slow=%llu (declined, original ran) fast-store-aliases-header=%llu "
                "TryEnter-failures=%llu virtual-calls=%llu]\n", (unsigned long long)n_fast, (unsigned long long)n_slow,
                (unsigned long long)n_alias, (unsigned long long)n_try_fail, (unsigned long long)n_vcalls);
  }
} stat;
}  // namespace c82212540
fuzz::Registrar reg_82212540({"sub_82212540", c82212540::orig_w, c82212540::nat_w, me::hot::n_82212540::Writes,
                              me::hot::n_82212540::kCmp, 0x40000000, 0x20000, c82212540::gen});
}  // namespace

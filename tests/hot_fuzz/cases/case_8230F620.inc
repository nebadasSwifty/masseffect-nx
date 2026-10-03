// ORIG: sub_8230F620
// NATIVE: n_8230F620.h
// sub_8230F620: hash lookup (owner, 64-bit key, class filter) over bucket chains; the native version declines when a visited
// node has its key not built yet ([node+4] == -1: the original calls sub_82389648 to build it).
// Environment: the two bucket tables (0x82E87480 / 0x82E8F480) are image data outside the window: the generator draws the
// bucket word and the wrappers put it into the arena the call runs on. sub_82389648 is a stub that writes a drawn 64-bit key
// to the guest address in r3 (the caller's stack scratch) and trashes the volatile registers; the harness additionally checks
// that the native version declines exactly when the original called the stub.
extern "C" void __imp__sub_8230F620(PPCContext&, uint8_t*);
extern "C" void __imp__sub_82389648(PPCContext& c, uint8_t* base);
extern "C" void sub_82389648(PPCContext& c, uint8_t* base) { __imp__sub_82389648(c, base); }  // see tools/direct_calls.py
namespace {
namespace c8230F620 {
constexpr uint32_t kWin = 0x40000000, kNodes = kWin + 0x1000, kClass = kWin + 0x3000, kMarker = kWin + 0x1F800;
uint32_t g_bucket_addr = 0, g_bucket_val = 0;
uint64_t g_stub_key = 0;
int g_stub_calls = 0, g_orig_stub_calls = 0;
uint64_t n_calls = 0, n_declined = 0, n_t1 = 0, n_t2 = 0, n_hit = 0, n_miss = 0, n_hit_filter0 = 0, n_hit_exact = 0, n_hit_chain = 0,
         n_empty_bucket = 0, n_mask_all = 0, n_bad = 0, n_stub_hit = 0, n_r7_zero = 0, n_exact_garbage = 0;
void Env(uint8_t* base) { me::hot::St32(base, g_bucket_addr, g_bucket_val); }
void orig_w(PPCContext& ctx, uint8_t* base) {
  Env(base);
  g_stub_calls = 0;
  __imp__sub_8230F620(ctx, base);
  g_orig_stub_calls = g_stub_calls;
  if (ctx.r3.u32 != 0) ++n_hit; else ++n_miss;
}
void nat_w(PPCContext& ctx, uint8_t* base) {
  Env(base);
  g_stub_calls = 0;
  if (!me::hot::n_8230F620::Native(ctx, base)) {
    ++n_declined;
    if (g_orig_stub_calls == 0) { ++n_bad; me::hot::St32(base, kMarker, 0xBADBAD01u); }
    __imp__sub_8230F620(ctx, base);
  } else if (g_orig_stub_calls != 0) {
    ++n_bad;
    me::hot::St32(base, kMarker, 0xBADBAD02u);
  }
}
}  // namespace c8230F620
}  // namespace
extern "C" void __imp__sub_82389648(PPCContext& c, uint8_t* base) {
  using namespace c8230F620;
  ++g_stub_calls;
  me::hot::St64(base, c.r3.u32, g_stub_key);
  c.r3.u64 = 0xDEAD0003DEAD0003ull; c.r4.u64 = 0xDEAD0004DEAD0004ull; c.r5.u64 = 0xDEAD0005DEAD0005ull;
  c.r6.u64 = 0xDEAD0006DEAD0006ull; c.r7.u64 = 0xDEAD0007DEAD0007ull; c.r8.u64 = 0xDEAD0008DEAD0008ull;
  c.r9.u64 = 0xDEAD0009DEAD0009ull; c.r10.u64 = 0xDEAD0010DEAD0010ull; c.r11.u64 = 0xDEAD0011DEAD0011ull;
  c.r12.u64 = 0xDEAD0012DEAD0012ull;
}
namespace {
namespace c8230F620 {
void gen(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  ++n_calls;
  constexpr int kN = 12, kC = 16;
  uint32_t cls[kC];
  for (int k = 0; k < kC; ++k) {
    cls[k] = kClass + 64 * k;
    me::hot::St32(base, cls[k] + 60, (k == 0 || r.chance(0.25)) ? 0 : cls[r.below(k)]);  // acyclic chains
  }
  const bool second = r.chance(0.45);  // owner == 0: second table
  const uint32_t owner = second ? 0 : (r.chance(0.1) ? 0xFFFFFFFFu : (kWin + 0x100 + 4 * r.below(0x400)));
  second ? ++n_t2 : ++n_t1;
  uint64_t key = r.chance(0.1) ? (r.chance(0.5) ? 0 : ~0ull) : r.u64();
  const uint32_t hi = uint32_t(key >> 32), lo = uint32_t(key);
  const uint32_t filter = r.chance(0.2) ? 0 : (r.chance(0.9) ? cls[r.below(kC)] : kWin + 0x1E000 + 4 * r.below(100));
  uint64_t exact = r.chance(0.5) ? 0 : 1;
  if (r.chance(0.3)) { exact = (exact ? r.u32() | 1 : 0) | (uint64_t(r.u32()) << 32); if (uint32_t(exact) == 0 && (exact >> 32)) ++n_exact_garbage; }
  uint64_t r7 = r.chance(0.5) ? 0 : r.u64();
  if (r.chance(0.2)) r7 = uint64_t(r.u32()) << 32;  // low word zero, upper garbage (s32 compare)
  if (uint32_t(r7) == 0) ++n_r7_zero;
  uint64_t mask;
  switch (r.below(8)) {
    case 0: mask = ~0ull; ++n_mask_all; break;
    case 1: case 2: case 3: mask = 0; break;
    case 4: mask = 0x0000000200000000ull; break;
    default: mask = r.u64() & r.u64() & r.u64(); break;
  }
  // chain nodes
  uint32_t node[kN];
  for (int k = 0; k < kN; ++k) node[k] = kNodes + 64 * k;
  const int len = r.chance(0.1) ? 0 : int(r.range(1, kN));
  for (int k = 0; k < kN; ++k) {
    // two independent chains: next1 at +20 (table 1), next2 at +16 (table 2)
    // (the two chains differ, so that following the wrong next-word offset is visible)
    const uint32_t n1 = (k + 1 < len) ? (r.chance(0.15) ? 0 : node[k + 1]) : 0;
    const uint32_t n2 = (k + 2 < len && r.chance(0.5)) ? node[k + 2] : ((k + 1 < len && r.chance(0.9)) ? node[k + 1] : 0);
    me::hot::St32(base, node[k] + 20, n1);
    me::hot::St32(base, node[k] + 16, n2);
    // [+4]: -1 (key not built) is rare
    me::hot::St32(base, node[k] + 4, r.chance(0.03) ? 0xFFFFFFFFu : (r.chance(0.5) ? r.below(100) : r.u32() & ~1u));
    // key at +44
    uint32_t khi = hi, klo = lo;
    switch (r.below(8)) {
      case 0: khi ^= 1u << r.below(32); break;
      case 1: klo ^= 1u << r.below(32); break;
      case 2: khi = r.u32(); break;
      default: break;  // matching key
    }
    me::hot::St32(base, node[k] + 44, khi);
    me::hot::St32(base, node[k] + 48, klo);
    // flags at +8 (64 bit): mostly not intersecting the mask
    uint64_t fl = r.chance(0.7) ? (r.u64() & ~mask) : r.u64();
    me::hot::St64(base, node[k] + 8, fl);
    // owner word at +40
    me::hot::St32(base, node[k] + 40, second ? (r.chance(0.7) ? 0 : r.u32()) : (r.chance(0.75) ? owner : r.u32()));
    // class pointer at +52
    uint32_t cp;
    switch (r.below(4)) {
      case 0: cp = filter; break;
      case 1: cp = cls[r.below(kC)]; break;
      case 2: cp = 0; break;
      default: cp = cls[kC - 1 - r.below(4)]; break;
    }
    me::hot::St32(base, node[k] + 52, cp);
  }
  // sometimes force the interesting nodes to be visited: a fully matching node somewhere in the chain
  if (len > 0 && r.chance(0.5)) {
    const int k = int(r.below(uint32_t(len)));
    me::hot::St32(base, node[k] + 4, r.below(100));
    me::hot::St32(base, node[k] + 44, hi);
    me::hot::St32(base, node[k] + 48, lo);
    me::hot::St64(base, node[k] + 8, (mask == ~0ull) ? r.u64() : (r.u64() & ~mask));
    me::hot::St32(base, node[k] + 40, second ? (r.chance(0.8) ? 0 : r.u32()) : owner);
    me::hot::St32(base, node[k] + 52, r.chance(0.5) ? filter : cls[r.below(kC)]);
  }
  uint32_t head = len ? node[0] : 0;
  if (!head) ++n_empty_bucket;
  const uint32_t x = second ? (lo ^ hi) : (lo ^ hi ^ owner);
  g_bucket_addr = (second ? me::hot::n_8230F620::kTable2 : me::hot::n_8230F620::kTable1) + ((x << 2) & 0x7FFCu);
  g_bucket_val = head;
  g_stub_key = r.chance(0.5) ? key : r.u64();
  if (g_stub_key == key) ++n_stub_hit;
  Env(base);
  ctx.r3.u64 = filter;
  ctx.r4.u64 = owner;
  ctx.r5.u64 = key;
  ctx.r6.u64 = exact;
  ctx.r7.u64 = r7;
  ctx.r8.u64 = mask;
}
struct Stat {
  ~Stat() {
    std::printf("   [8230F620 coverage: calls=%llu table1=%llu table2=%llu empty-bucket=%llu | original returned node=%llu 0=%llu | "
                "declined=%llu (stub called) | mask==-1: %llu, r7 low word 0: %llu, r6 low word 0 with upper bits: %llu | "
                "native/original decline disagreements: %llu]\n",
                (unsigned long long)n_calls, (unsigned long long)n_t1, (unsigned long long)n_t2, (unsigned long long)n_empty_bucket,
                (unsigned long long)n_hit, (unsigned long long)n_miss, (unsigned long long)n_declined,
                (unsigned long long)n_mask_all, (unsigned long long)n_r7_zero, (unsigned long long)n_exact_garbage,
                (unsigned long long)n_bad);
  }
} stat;
}  // namespace c8230F620
fuzz::Registrar reg_8230F620({"sub_8230F620", c8230F620::orig_w, c8230F620::nat_w, me::hot::n_8230F620::Writes,
                              me::hot::n_8230F620::kCmp, 0x40000000, 0x20000, c8230F620::gen});
}  // namespace

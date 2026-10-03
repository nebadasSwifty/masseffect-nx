// ORIG: sub_82B5F0E8
// NATIVE: n_82B5F0E8.h
// sub_82B5F0E8: bounding-volume overlap test with two virtual getter calls and a box loop (see src/native/hot/n_82B5F0E8.h).
// Environment: the two virtual getters are host stubs reached through the dispatch table the generated REX_CALL_INDIRECT_FUNC
// reads (guest code address 0x82210000 + 4 * k -> table at 0x82FC0000 + 8 * k in the arena): the stub reads the s32 result
// from the object (obj + 0x40 / + 0x44), returns it in the low half of r3 with garbage in the upper half, scribbles into its own
// stack frame, trashes r4-r12, and logs everything the callee can observe (r1, lr, r3-r12); the log of the original and of the
// native run must be identical.
extern "C" void __imp__sub_82B5F0E8(PPCContext&, uint8_t*);
namespace rex::runtime {
__attribute__((weak)) PPCFunc* ResolveIndirectFunction(uint32_t) { return nullptr; }
}  // namespace rex::runtime
namespace {
namespace c82B5F0E8 {
constexpr uint32_t kWin = 0x40000000, kMarker = kWin + 0x1F800;
constexpr uint32_t kT1 = 0x82210000u + 4 * 100, kT2 = 0x82210000u + 4 * 101;
struct Call {
  uint64_t r[12];  // r1, lr, r3..r12 : as seen by the callee on entry
  uint32_t which;
};
Call g_log[4], g_orig_log[4];
int g_n = 0, g_orig_n = 0;
uint64_t n_calls = 0, n_ret0_g1 = 0, n_ret0_g2 = 0, n_ret0_loop = 0, n_ret1 = 0, n_empty = 0, n_log_bad = 0, n_misaligned = 0,
         n_hit_block = 0, n_hit_tail = 0, n_ext = 0, n_big = 0, n_noentry_ret = 0;
uint64_t g_hit_idx = ~0ull;
void Stub(PPCContext& c, uint8_t* base, uint32_t which) {
  if (g_n < 4) {
    Call& l = g_log[g_n];
    l.r[0] = c.r1.u64; l.r[1] = c.lr; l.r[2] = c.r3.u64; l.r[3] = c.r4.u64; l.r[4] = c.r5.u64; l.r[5] = c.r6.u64; l.r[6] = c.r7.u64;
    l.r[7] = c.r8.u64; l.r[8] = c.r9.u64; l.r[9] = c.r10.u64; l.r[10] = c.r11.u64; l.r[11] = c.r12.u64;
    l.which = which;
  }
  ++g_n;
  const uint32_t obj = c.r3.u32;
  const uint32_t v = me::hot::Ld32(base, obj + (which == 1 ? 0x40 : 0x44));
  me::hot::St32(base, c.r1.u32 - 64, 0xA5A5A5A5u);  // the callee's own frame
  c.r3.u64 = (uint64_t(v * 0x9E3779B1u + which) << 32) | v;
  c.r4.u64 = 0xDEAD0004DEAD0004ull; c.r5.u64 = 0xDEAD0005DEAD0005ull; c.r6.u64 = 0xDEAD0006DEAD0006ull;
  c.r7.u64 = 0xDEAD0007DEAD0007ull; c.r8.u64 = 0xDEAD0008DEAD0008ull; c.r9.u64 = 0xDEAD0009DEAD0009ull;
  c.r10.u64 = 0xDEAD0010DEAD0010ull; c.r11.u64 = 0xDEAD0011DEAD0011ull; c.r12.u64 = 0xDEAD0012DEAD0012ull;
}
void Stub1(PPCContext& c, uint8_t* b) { Stub(c, b, 1); }
void Stub2(PPCContext& c, uint8_t* b) { Stub(c, b, 2); }
void Env(uint8_t* base) {
  *reinterpret_cast<PPCFunc**>(base + 0x82000000ull + 0xFC0000ull + (uint64_t(kT1) - 0x82210000ull) * 2) = Stub1;
  *reinterpret_cast<PPCFunc**>(base + 0x82000000ull + 0xFC0000ull + (uint64_t(kT2) - 0x82210000ull) * 2) = Stub2;
}
void orig_w(PPCContext& ctx, uint8_t* base) {
  Env(base);
  g_n = 0;
  __imp__sub_82B5F0E8(ctx, base);
  g_orig_n = g_n;
  std::memcpy(g_orig_log, g_log, sizeof g_log);
  if (ctx.r3.u32 == 1) ++n_ret1;
}
void nat_w(PPCContext& ctx, uint8_t* base) {
  Env(base);
  g_n = 0;
  me::hot::n_82B5F0E8::Native(ctx, base);
  bool bad = g_n != g_orig_n;
  for (int i = 0; !bad && i < g_n && i < 4; ++i) {
    bad = g_log[i].which != g_orig_log[i].which;
    for (int k = 0; k < 12 && !bad; ++k) bad = g_log[i].r[k] != g_orig_log[i].r[k];
  }
  if (bad) {
    ++n_log_bad;
    me::hot::St32(base, kMarker, 0xBADBAD01u);
  }
}
int32_t ival(fuzz::Rng& r, int span) {
  switch (r.below(40)) {
    case 0: return INT32_MIN;
    case 1: return INT32_MAX;
    case 2: return INT32_MIN + 1;
    case 3: return INT32_MAX - 1;
    default: return int32_t(r.below(uint32_t(2 * span + 1))) - span;
  }
}
void gen(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  ++n_calls;
  const uint32_t self = kWin + 0x100 + r.below(0x40) * 16;
  const uint32_t obj = kWin + 0x2000 + r.below(0x40) * 16;
  const uint32_t vt = kWin + 0x3000 + r.below(0x40) * 16;
  const uint32_t q = kWin + 0x1000 + r.below(0x40) * 16;
  const uint32_t ent = kWin + 0x8000 + (r.chance(0.2) ? r.below(4) : 0) + r.below(8) * 16;
  const uint32_t count = r.chance(0.08) ? 0 : (r.chance(0.8) ? r.below(24) : r.below(400));
  if (ent & 3) ++n_misaligned;
  if (count == 0) ++n_empty;
  me::hot::St32(base, self + 0, obj);
  me::hot::St32(base, self + 20, ent);
  me::hot::St32(base, self + 24, count);
  me::hot::St32(base, obj + 0, vt);
  me::hot::St32(base, vt + 20, kT1);
  me::hot::St32(base, vt + 28, kT2);
  // q: box (x range p0..p1, y range p2..p3) in a small coordinate space; the getters bound it from above
  int32_t p0 = ival(r, 40), p1 = ival(r, 40), p2 = ival(r, 40), p3 = ival(r, 40);
  if (r.chance(0.8)) { if (p1 < p0) std::swap(p0, p1); if (p3 < p2) std::swap(p2, p3); }
  me::hot::St32(base, q + 0, uint32_t(p0));
  me::hot::St32(base, q + 4, uint32_t(p1));
  me::hot::St32(base, q + 8, uint32_t(p2));
  me::hot::St32(base, q + 12, uint32_t(p3));
  // getter 1 limit must be >= p1 (else return 0 at once), getter 2 limit >= p3
  int32_t g1 = r.chance(0.12) ? ival(r, 40) : (p1 == INT32_MAX ? p1 : p1 + int32_t(r.below(20)));
  int32_t g2 = r.chance(0.12) ? ival(r, 40) : (p3 == INT32_MAX ? p3 : p3 + int32_t(r.below(20)));
  if (r.chance(0.05)) g1 = p1;  // equal: not greater
  if (r.chance(0.05)) g2 = p3;
  me::hot::St32(base, obj + 0x40, uint32_t(g1));
  me::hot::St32(base, obj + 0x44, uint32_t(g2));
  if (p1 > g1) ++n_ret0_g1; else if (p3 > g2) ++n_ret0_g2;
  // entries: all non-overlapping except (maybe) one at a random index
  const int32_t hitidx = (count && r.chance(0.5)) ? int32_t(r.below(count)) : -1;
  for (uint32_t i = 0; i < count; ++i) {
    int32_t e0 = ival(r, 60), e1 = ival(r, 60), e2 = ival(r, 60), e3 = ival(r, 60);
    if (r.chance(0.8)) { if (e1 < e0) std::swap(e0, e1); if (e3 < e2) std::swap(e2, e3); }
    if (int32_t(i) == hitidx) {
      // force an overlap: p0 < e1 && p1 > e0 && p2 < e3 && p3 > e2
      if (p0 == INT32_MAX || p1 == INT32_MIN || p2 == INT32_MAX || p3 == INT32_MIN) { /* cannot overlap */ }
      else {
        e1 = int32_t(std::min<int64_t>(int64_t(p0) + 1 + r.below(30), INT32_MAX));
        e0 = int32_t(std::max<int64_t>(int64_t(p1) - 1 - r.below(30), INT32_MIN));
        e3 = int32_t(std::min<int64_t>(int64_t(p2) + 1 + r.below(30), INT32_MAX));
        e2 = int32_t(std::max<int64_t>(int64_t(p3) - 1 - r.below(30), INT32_MIN));
      }
    } else {
      // force a miss by violating exactly one (random) condition
      bool ov = int64_t(p0) < e1 && int64_t(p1) > e0 && int64_t(p2) < e3 && int64_t(p3) > e2;
      if (ov) {
        switch (r.below(4)) {
          case 0: e1 = p0; break;                   // p0 < e1 false
          case 1: e0 = p1; break;                   // p1 > e0 false
          case 2: e3 = p2; break;                   // p2 < e3 false
          default: e2 = p3; break;                  // p3 > e2 false
        }
        // the exact-boundary value (equal) is also what the "<" / ">" must reject
      }
    }
    me::hot::St32(base, ent + 16 * i + 0, uint32_t(e0));
    me::hot::St32(base, ent + 16 * i + 4, uint32_t(e1));
    me::hot::St32(base, ent + 16 * i + 8, uint32_t(e2));
    me::hot::St32(base, ent + 16 * i + 12, uint32_t(e3));
  }
  if (!(p1 > g1) && !(p3 > g2)) {
    if (hitidx >= 0) { ++n_ret0_loop; ((uint32_t(hitidx) / 4) < (count / 4) ? ++n_hit_block : ++n_hit_tail); }
  }
  ctx.r3.u64 = self;
  ctx.r4.u64 = q;
  Env(base);
}
struct Stat {
  ~Stat() {
    std::printf("   [82B5F0E8 coverage: calls=%llu getter1-limit-exceeded=%llu getter2-limit-exceeded=%llu | loop-hit(intended)=%llu "
                "(in a 4-block=%llu, in the tail=%llu) | original returned 1 (no overlap)=%llu | empty arrays=%llu misaligned entry "
                "arrays=%llu | callee-visible-state log mismatches=%llu]\n",
                (unsigned long long)n_calls, (unsigned long long)n_ret0_g1, (unsigned long long)n_ret0_g2,
                (unsigned long long)n_ret0_loop, (unsigned long long)n_hit_block, (unsigned long long)n_hit_tail,
                (unsigned long long)n_ret1, (unsigned long long)n_empty, (unsigned long long)n_misaligned,
                (unsigned long long)n_log_bad);
  }
} stat;
}  // namespace c82B5F0E8
fuzz::Registrar reg_82B5F0E8({"sub_82B5F0E8", c82B5F0E8::orig_w, c82B5F0E8::nat_w, me::hot::n_82B5F0E8::Writes,
                              me::hot::n_82B5F0E8::kCmp, 0x40000000, 0x20000, c82B5F0E8::gen});
}  // namespace

// ORIG: sub_8264ADA0 sub_82210298
// NATIVE: n_8264ADA0.h
// sub_8264ADA0: skin cache re-bind: per section, AddUninitialized(1) + 12-word transposed matrix copy per bone (see
// src/native/hot/n_8264ADA0.h). Environment of the case:
//  * the virtual call [[prev]+0](prev, 1) goes through the dispatch table the generated REX_CALL_INDIRECT_FUNC reads (guest code
//    address 0x82210000 + 4 * 100 -> table at 0x82FC0000 + 8 * 100 in the arena): a host stub that writes no guest memory (except
//    its own stack frame), returns a hash of its argument in r3 (64 bits, garbage upper half), trashes r4-r12, and logs what the
//    callee can observe (r1, lr, r3-r12);
//  * the allocator sub_822100F8 is a stub that writes no memory and returns a deterministic heap address model(ptr, size)
//    (garbage in the upper half of r3), logging (r1, r3, r4, r5, r6); original and native logs must be identical (a mismatch
//    writes a marker word into the arena, which the harness reports). The native header's alloc_model gives Writes() the same model.
//  * float data: the copy is a raw word move; the matrices contain unique ids, ordinary floats, inf/denormal/NaN patterns and random
//    words (the 12 lfs/stfs of the original are bit copies; sNaN included here to prove it).
// Coverage (printed after the run): prev object / no prev, empty/negative section counts, empty/negative bone counts, no-grow /
// grow (1..N allocator calls per section), negative capacity, data == 0, [info+48] <= 4 and > 4, source/destination overlap
// (output array inside the matrix array), three aliasing shapes: shared header between sections, data array overlapping its own
// header (count word clobbered by matrix floats: reaches the add <= 0 path), see "alias".  In the aliasing-with-header runs the
// registered Writes() is widened to the whole window (the dry run of Writes() assumes unmodified metadata; the real Writes()
// reports overflow for that shape).
// Unreachable by construction: the allocator-skip (data == 0 and newcap == 0) inside AddUninitialized (newcap >= 4 because
// the new count is always i + 1 >= 1).
extern "C" void __imp__sub_8264ADA0(PPCContext&, uint8_t*);
extern "C" void sub_822100F8(PPCContext& c, uint8_t*);
extern "C" void __imp__sub_822100F8(PPCContext& c, uint8_t*);
namespace rex::runtime {
__attribute__((weak)) PPCFunc* ResolveIndirectFunction(uint32_t) { return nullptr; }
}  // namespace rex::runtime
namespace {
namespace c8264ADA0 {
constexpr uint32_t kWin = 0x40000000, kHeap = kWin + 0xC000, kMarker = kWin + 0x1F800;
constexpr uint32_t kT = 0x82210000u + 4 * 100;

struct Vcall {
  uint64_t r[12];
};
struct Acall {
  uint64_t r[5];
};
Vcall g_vlog[4], g_orig_vlog[4];
Acall g_alog[24], g_orig_alog[24];
int g_nv = 0, g_na = 0, g_orig_nv = 0, g_orig_na = 0;
bool g_alias = false;
bool g_pure = getenv("ME_BENCH_PURE") != nullptr;
uint64_t n_gen = 0, n_prev = 0, n_noprev = 0, n_ns_le0 = 0, n_ns_multi = 0, n_n_le0 = 0, n_sec = 0, n_bones = 0, n_m_gt4 = 0,
         n_m_le4 = 0, n_neg_cap = 0, n_data0 = 0, n_alias_hdr = 0, n_alias_shared = 0, n_alias_mat = 0, n_addle0 = 0,
         n_log_bad = 0, n_unaligned = 0, n_overlap_copy = 0, n_nogrow_sec = 0;
uint64_t n_alloc[4] = {};

uint32_t Model(uint32_t ptr, uint32_t size) { return kHeap + 0x400 * (1 + ((ptr >> 4) + size) % 24); }
uint64_t Mix(uint64_t x) {
  x ^= x >> 31;
  x *= 0x9E3779B97F4A7C15ull;
  x ^= x >> 29;
  return x;
}
void VStub(PPCContext& c, uint8_t* base) {
  if (g_nv < 4) {
    Vcall& l = g_vlog[g_nv];
    l.r[0] = c.r1.u64; l.r[1] = c.lr; l.r[2] = c.r3.u64; l.r[3] = c.r4.u64; l.r[4] = c.r5.u64; l.r[5] = c.r6.u64;
    l.r[6] = c.r7.u64; l.r[7] = c.r8.u64; l.r[8] = c.r9.u64; l.r[9] = c.r10.u64; l.r[10] = c.r11.u64; l.r[11] = c.r12.u64;
  }
  ++g_nv;
  me::hot::St32(base, c.r1.u32 - 64, 0xA5A5A5A5u);  // the callee's own frame
  c.r3.u64 = Mix(c.r3.u64 + c.r4.u64);
  c.r4.u64 = 0xDEAD0004DEAD0004ull; c.r5.u64 = 0xDEAD0005DEAD0005ull; c.r6.u64 = 0xDEAD0006DEAD0006ull;
  c.r7.u64 = 0xDEAD0007DEAD0007ull; c.r8.u64 = 0xDEAD0008DEAD0008ull; c.r9.u64 = 0xDEAD0009DEAD0009ull;
  c.r10.u64 = 0xDEAD0010DEAD0010ull; c.r11.u64 = 0xDEAD0011DEAD0011ull; c.r12.u64 = 0xDEAD0012DEAD0012ull;
}
void AStub(PPCContext& c) {
  if (g_na < 24) {
    Acall& l = g_alog[g_na];
    l.r[0] = c.r1.u64; l.r[1] = c.r3.u64; l.r[2] = c.r4.u64; l.r[3] = c.r5.u64; l.r[4] = c.r6.u64;
  }
  ++g_na;
  c.r3.u64 = (Mix(c.r3.u64) & 0xFFFFFFFF00000000ull) | Model(c.r3.u32, uint32_t(c.r4.u64));
  c.r4.u64 = 0xDEAD0004DEAD0004ull; c.r5.u64 = 0xDEAD0005DEAD0005ull; c.r6.u64 = 0xDEAD0006DEAD0006ull;
  c.r7.u64 = 0xDEAD0007DEAD0007ull; c.r8.u64 = 0xDEAD0008DEAD0008ull; c.r9.u64 = 0xDEAD0009DEAD0009ull;
  c.r10.u64 = 0xDEAD0010DEAD0010ull; c.r11.u64 = 0xDEAD0011DEAD0011ull; c.r12.u64 = 0xDEAD0012DEAD0012ull;
  c.f0.u64 = 0x7FF8DEAD0000F000ull; c.f1.u64 = 0x7FF8DEAD0000F001ull;
}
void Env(uint8_t* base) {
  *reinterpret_cast<PPCFunc**>(base + 0x82000000ull + 0xFC0000ull + (uint64_t(kT) - 0x82210000ull) * 2) = VStub;
}

void Classify(const PPCContext& ctx, const uint8_t* base) {
  using namespace me::hot;
  const uint32_t obj = ctx.r3.u32, arg = ctx.r4.u32;
  if (Ld32(base, obj + 48)) ++n_prev; else ++n_noprev;
  const uint32_t lod4 = Ld32(base, arg + 28) << 2;
  const uint32_t r25 = Ld32(base, Ld32(base, Ld32(base, obj + 12) + 160) + lod4);
  const int32_t ns = int32_t(Ld32(base, r25 + 16));
  if (ns <= 0) ++n_ns_le0; else if (ns > 1) ++n_ns_multi;
}

void orig_w(PPCContext& ctx, uint8_t* base) {
  Env(base);
  if (g_pure) return __imp__sub_8264ADA0(ctx, base);
  Classify(ctx, base);
  g_nv = 0; g_na = 0;
  __imp__sub_8264ADA0(ctx, base);
  g_orig_nv = g_nv; g_orig_na = g_na;
  std::memcpy(g_orig_vlog, g_vlog, sizeof g_vlog);
  std::memcpy(g_orig_alog, g_alog, sizeof g_alog);
  ++n_alloc[std::min(g_na, 3)];
}
void nat_w(PPCContext& ctx, uint8_t* base) {
  Env(base);
  if (g_pure) return me::hot::n_8264ADA0::Native(ctx, base);
  g_nv = 0; g_na = 0;
  me::hot::n_8264ADA0::Native(ctx, base);
  bool bad = g_nv != g_orig_nv || g_na != g_orig_na;
  for (int i = 0; !bad && i < g_nv && i < 4; ++i)
    for (int k = 0; k < 12 && !bad; ++k) bad = g_vlog[i].r[k] != g_orig_vlog[i].r[k];
  for (int i = 0; !bad && i < g_na && i < 24; ++i)
    for (int k = 0; k < 5 && !bad; ++k) bad = g_alog[i].r[k] != g_orig_alog[i].r[k];
  if (bad) {
    ++n_log_bad;
    me::hot::St32(base, kMarker, 0xBADBAD01u);
  }
}
void writes_w(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) {
  if (g_alias) {  // metadata is modified by the run itself: the dry run cannot predict it (see the header comment)
    w.Add(kWin, 0x1F000);
    return;
  }
  me::hot::n_8264ADA0::Writes(ctx, base, w);
}

uint32_t fl(fuzz::Rng& r) {
  switch (r.below(10)) {
    case 0: return r.float_bits();
    case 1: return r.u32();
    case 2: return 0x7FC00000u;
    default: {
      float f = float(int32_t(r.u32() % 200001) - 100000) * (1.0f / 128.0f);
      uint32_t b;
      std::memcpy(&b, &f, 4);
      return b;
    }
  }
}
void put_matrices(fuzz::Rng& r, uint8_t* base, uint32_t M) {
  using namespace me::hot;
  for (uint32_t m = 0; m < 64; ++m) {
    const bool uid = r.chance(0.4), small = !uid && r.chance(0.3);  // unique ids / small integers (count-word clobbering) / floats
    for (uint32_t w = 0; w < 16; ++w) St32(base, M + m * 64 + w * 4, uid ? (0x3F000000u | (m << 8) | w) : small ? r.below(24) : fl(r));
  }
}

void gen(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  using namespace me::hot;
  ++n_gen;
  g_alias = false;
  const uint32_t O = kWin + 0x100, P = kWin + 0x180, VT = kWin + 0x1C0, G = kWin + 0x200, A = kWin + 0x240, T = kWin + 0x300,
                 B = kWin + 0x340, L = kWin + 0x400, Sx = kWin + 0x500, Qa = kWin + 0x800, Q0 = kWin + 0x900,
                 SI = kWin + 0x1400, IX = kWin + 0x1800, D = kWin + 0x6000;
  uint32_t M = kWin + 0x4000;
  const uint32_t lod = r.below(4);
  St32(base, O + 12, A);
  St32(base, O + 20, B);
  St32(base, A + 160, T);
  for (uint32_t k = 0; k < 16; ++k) St32(base, T + 4 * k, k == lod ? L : r.u32());
  for (uint32_t k = 0; k < 16; ++k) St32(base, B + 4 * k, k == lod ? Sx : r.u32());
  const bool unaligned = r.chance(0.05);
  if (unaligned) { M += 4 * (1 + r.below(7)); ++n_unaligned; }
  St32(base, G + 0, VT);  // arg is a valid object too: the next call (benchmark repeats) makes it the previous object
  St32(base, G + 28, lod);
  St32(base, G + 4, M);
  St32(base, O + 48, r.chance(0.55) ? P : 0);
  St32(base, P + 0, VT);
  St32(base, VT + 0, kT);
  put_matrices(r, base, kWin + 0x4000);
  // section count
  int32_t ns;
  const uint32_t k1 = r.below(100);
  if (k1 < 45) ns = 1;
  else if (k1 < 75) ns = int32_t(r.range(2, 3));
  else if (k1 < 85) ns = int32_t(r.range(4, 8));
  else if (k1 < 93) ns = 0;
  else ns = -int32_t(r.range(1, 3));
  St32(base, L + 16, uint32_t(ns));
  St32(base, L + 12, SI);
  St32(base, Sx + 236, Qa);
  const uint32_t alias_kind = r.chance(0.15) ? 1 + r.below(3) : 0;  // 1 header overlap, 2 shared header, 3 matrix overlap
  for (uint32_t k = 0; k < 8; ++k) St32(base, Qa + 4 * k, (alias_kind == 2 && (k & 1)) ? Q0 + 0x120 * (k - 1) : Q0 + 0x120 * k);
  const uint32_t nmat = r.chance(0.9) ? 24 : 60;
  const int nsec = ns > 0 ? int(std::min(ns, 8)) : 0;
  for (int s = 0; s < nsec; ++s) {
    const uint32_t si = SI + 52 * s, ix = IX + 0x100 * s, hdr = Q0 + 0x120 * s + 248;
    int32_t n;
    const uint32_t kn = r.below(100);
    if (kn < 70) n = int32_t(r.range(1, 12));
    else if (kn < 80) n = 0;
    else if (kn < 85) n = -int32_t(r.range(1, 3));
    else n = int32_t(r.range(13, 20));
    St32(base, si + 32, uint32_t(n));
    St32(base, si + 28, ix);
    const uint32_t km = r.below(10);
    St32(base, si + 48, km < 5 ? r.below(9) : km < 7 ? 4 : km < 8 ? 5 : km < 9 ? uint32_t(-int32_t(r.range(0, 4))) : r.u32());
    for (uint32_t j = 0; j < 64; ++j) St16(base, ix + 2 * j, uint16_t(r.below(nmat)));
    // header {data, count, capacity, maxInfl}
    const uint32_t kd = r.below(20);
    uint32_t data = D + 0x400 * s;
    if (kd == 0) data = 0;
    else if (kd < 4) data = kHeap + 0x400 * r.below(24);
    else if (kd < 6) data = D + 0x400 * s + 4 * r.below(8);
    if (alias_kind == 3 && s == 0) { data = kWin + 0x4000 + 4 * r.below(0x300); ++n_alias_mat; }
    if (alias_kind == 1 && s == 0 && n > 0) {
      const uint32_t i = r.below(uint32_t(n)), kw = r.below(12), q = r.chance(0.6) ? 1 : r.below(4);
      data = hdr + 4 * q - 48 * i - 4 * kw;
      g_alias = true;
      ++n_alias_hdr;
    }
    if (alias_kind == 2 && s == 0 && nsec > 1) { g_alias = true; ++n_alias_shared; }
    uint32_t cap;
    const uint32_t kc = r.below(20);
    if (kc < 8) cap = uint32_t(n > 0 ? n : 0) + r.below(3);
    else if (kc < 16) cap = r.below(uint32_t(n > 0 ? n : 1));
    else if (kc < 17) cap = uint32_t(-int32_t(r.range(1, 40)));
    else if (kc < 18) cap = 0x80000000u | r.below(20);
    else if (kc < 19) cap = 0;
    else cap = r.u32();
    if (int32_t(cap) < 0) ++n_neg_cap;
    if (data == 0) ++n_data0;
    St32(base, hdr + 0, data);
    St32(base, hdr + 4, r.u32());
    St32(base, hdr + 8, cap);
    St32(base, hdr + 12, r.u32());
    ++n_sec;
    if (n <= 0) ++n_n_le0; else n_bones += uint32_t(n);
    const int32_t m = int32_t(Ld32(base, si + 48));
    if (m > 4) ++n_m_gt4; else ++n_m_le4;
    if (data >= kWin + 0x4000 && data < kWin + 0x5000) ++n_overlap_copy;
    if (int32_t(cap) >= n) ++n_nogrow_sec;
  }
  ctx.r3.u64 = O | (r.chance(0.2) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0);
  ctx.r4.u64 = G | (r.chance(0.2) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0);
  Env(base);
}
struct Stat {
  ~Stat() {
    std::printf("   [8264ADA0 coverage: gen=%llu prev-object(virtual call)=%llu no-prev=%llu sections<=0=%llu multi-section=%llu | sections=%llu "
                "(bones<=0=%llu, no-grow=%llu, [info+48]>4=%llu, <=4=%llu, neg-cap=%llu, data=0=%llu) bones=%llu | allocator calls per "
                "call 0/1/2/3+: %llu/%llu/%llu/%llu | alias: header-overlap=%llu shared-header=%llu out-in-matrices=%llu | "
                "unaligned-matrices=%llu | callee-visible-state log mismatches=%llu]\n",
                (unsigned long long)n_gen, (unsigned long long)n_prev, (unsigned long long)n_noprev, (unsigned long long)n_ns_le0,
                (unsigned long long)n_ns_multi, (unsigned long long)n_sec, (unsigned long long)n_n_le0, (unsigned long long)n_nogrow_sec,
                (unsigned long long)n_m_gt4, (unsigned long long)n_m_le4, (unsigned long long)n_neg_cap, (unsigned long long)n_data0,
                (unsigned long long)n_bones, (unsigned long long)n_alloc[0], (unsigned long long)n_alloc[1],
                (unsigned long long)n_alloc[2], (unsigned long long)n_alloc[3], (unsigned long long)n_alias_hdr,
                (unsigned long long)n_alias_shared, (unsigned long long)n_alias_mat, (unsigned long long)n_unaligned,
                (unsigned long long)n_log_bad);
  }
} stat;
struct Init {
  Init() {
    me::hot::n_8264ADA0::virt_unbounded = false;
    me::hot::n_8264ADA0::alloc_unbounded = false;
    me::hot::n_8264ADA0::alloc_model = Model;
  }
} init;
}  // namespace c8264ADA0
}  // namespace
extern "C" void sub_822100F8(PPCContext& c, uint8_t*) { c8264ADA0::AStub(c); }
extern "C" void __imp__sub_822100F8(PPCContext& c, uint8_t*) { c8264ADA0::AStub(c); }
namespace {
fuzz::Registrar reg_8264ADA0({"sub_8264ADA0", c8264ADA0::orig_w, c8264ADA0::nat_w, c8264ADA0::writes_w, me::hot::n_8264ADA0::kCmp,
                              0x40000000, 0x20000, c8264ADA0::gen});
}  // namespace

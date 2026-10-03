// ORIG: sub_826545D0 sub_82210368 sub_82210298
// NATIVE: n_826545D0.h
// sub_826545D0: UE3 skeletal mesh bone matrices (see src/native/hot/n_826545D0.h).
// Environment of the case:
//  * the allocator sub_822100F8 (virtual Realloc through a global object) is replaced by a stub that writes no memory,
//    returns kHeap in r3 and trashes the other volatile registers; its (r3, r4, r5) arguments are logged for the original
//    and for the native run and compared (a mismatch scribbles a marker word into the arena, which the harness reports);
//  * the 64-byte identity constant at 0x82E64F30 lives outside the window: the generator writes it into arena A and the
//    native wrapper into arena B (random contents: sometimes the identity, sometimes arbitrary floats).
extern "C" void __imp__sub_826545D0(PPCContext&, uint8_t*);
extern "C" void sub_822100F8(PPCContext& c, uint8_t*);
extern "C" void __imp__sub_822100F8(PPCContext& c, uint8_t*);
namespace {
namespace c826545D0 {

constexpr uint32_t kWin = 0x40000000, kHeap = kWin + 0x10000, kMarker = kWin + 0x1F800;
struct Call {
  uint64_t r3, r4, r5;
};
uint32_t g_stub_ret = kHeap;  // what the allocator stub returns (random in the aliased-header cases, where no output is written)
Call g_log[8], g_orig_log[8];
int g_n = 0, g_orig_n = 0;
uint32_t g_ident[16];
uint64_t g_ident_serial = 0, g_ident_put = ~0ull;
uint64_t n_nan_tol = 0;
uint64_t n_calls[4] = {};
uint64_t n_gen = 0, n_remap = 0, n_direct = 0, n_empty = 0, n_alloc1 = 0, n_alloc2 = 0, n_ok = 0, n_fb_idx = 0, n_fb_neg = 0,
         n_fb_bound = 0, n_fb_obj = 0, n_alias_dest = 0, n_special = 0, n_noremap_ptr = 0, n_neg_cnt = 0, n_log_bad = 0;
struct Stat {
  ~Stat() {
    std::printf("   [826545D0 coverage: gen=%llu remap-path=%llu direct-path=%llu (remap ptr null=%llu) empty-list=%llu alloc1=%llu alloc2=%llu "
                "bones: ok=%llu fb(idx>=nb)=%llu fb(map<0)=%llu fb(map>=bound)=%llu fb(idx>=objbound)=%llu dest-aliases-src=%llu "
                "allocator-calls 0/1/2/3+: %llu/%llu/%llu/%llu special-float-matrices=%llu neg-count=%llu nan-payload-tolerated-words=%llu log-mismatch=%llu]\n",
                (unsigned long long)n_gen, (unsigned long long)n_remap, (unsigned long long)n_direct,
                (unsigned long long)n_noremap_ptr, (unsigned long long)n_empty, (unsigned long long)n_alloc1,
                (unsigned long long)n_alloc2, (unsigned long long)n_ok, (unsigned long long)n_fb_idx,
                (unsigned long long)n_fb_neg, (unsigned long long)n_fb_bound, (unsigned long long)n_fb_obj,
                (unsigned long long)n_alias_dest, (unsigned long long)n_calls[0], (unsigned long long)n_calls[1],
                (unsigned long long)n_calls[2], (unsigned long long)n_calls[3], (unsigned long long)n_special, (unsigned long long)n_neg_cnt,
                (unsigned long long)n_nan_tol, (unsigned long long)n_log_bad);
  }
} stat;

void Stub(PPCContext& c) {
  if (g_n < 8) g_log[g_n] = {c.r3.u64, c.r4.u64, c.r5.u64};
  ++g_n;
  c.r3.u64 = g_stub_ret;
  c.r4.u64 = 0xDEAD0004DEAD0004ull; c.r5.u64 = 0xDEAD0005DEAD0005ull; c.r6.u64 = 0xDEAD0006DEAD0006ull;
  c.r7.u64 = 0xDEAD0007DEAD0007ull; c.r8.u64 = 0xDEAD0008DEAD0008ull; c.r9.u64 = 0xDEAD0009DEAD0009ull;
  c.r10.u64 = 0xDEAD0010DEAD0010ull; c.r11.u64 = 0xDEAD0011DEAD0011ull; c.r12.u64 = 0xDEAD0012DEAD0012ull;
  c.f0.u64 = 0x7FF8DEAD0000F000ull; c.f1.u64 = 0x7FF8DEAD0000F001ull;
}
void put_ident(uint8_t* base) {
  for (int i = 0; i < 16; ++i) me::hot::St32(base, me::hot::n_826545D0::kIdent + 4 * i, g_ident[i]);
}

// NaN payload tolerance. Which payload / sign a NaN result carries when two different NaNs (e.g. a quiet NaN input and the
// default NaN of inf * 0) meet in one fmul / faddp depends on the operand order the C++ compiler picked for that
// instruction (ARM: the first NaN operand wins), and GCC picks a different order for different rows of the very same
// original (checked in the Switch disassembly). That is not a property of the game's semantics, so where original and
// native both produce a NaN but with different bits, the native word is replaced by the original's (and counted).
// Every other difference (NaN vs number, any bit of a non-NaN) still fails. ME_NOPAYLOAD=1 makes the generator use only
// 0x7FC00000 as NaN, in which case no tolerance is ever needed (counter stays 0).
constexpr uint32_t kSnapLo = kWin + 0x2000, kSnapLen = 0x10000;
uint8_t g_snap[kSnapLen];
bool g_pure = getenv("ME_BENCH_PURE") != nullptr;  // wrappers without the extra checks (benchmark)
void orig_w(PPCContext& ctx, uint8_t* base) {
  if (g_pure) return __imp__sub_826545D0(ctx, base);
  g_n = 0;
  __imp__sub_826545D0(ctx, base);
  g_orig_n = g_n;
  std::memcpy(g_orig_log, g_log, sizeof g_log);
  std::memcpy(g_snap, base + kSnapLo, kSnapLen);
}
bool is_nan_be(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  v = __builtin_bswap32(v);
  return (v & 0x7F800000u) == 0x7F800000u && (v & 0x7FFFFFu) != 0;
}
void nat_w(PPCContext& ctx, uint8_t* base) {
  if (g_pure) return me::hot::n_826545D0::Native(ctx, base);
  if (g_ident_put != g_ident_serial) {
    put_ident(base);
    g_ident_put = g_ident_serial;
  }
  static uint8_t pre[kSnapLen];
  std::memcpy(pre, base + kSnapLo, kSnapLen);
  g_n = 0;
  me::hot::n_826545D0::Native(ctx, base);
  n_calls[std::min(g_orig_n, 3)]++;
  bool bad = g_n != g_orig_n;
  for (int i = 0; !bad && i < g_n && i < 8; ++i)
    bad = g_log[i].r3 != g_orig_log[i].r3 || g_log[i].r4 != g_orig_log[i].r4 || g_log[i].r5 != g_orig_log[i].r5;
  for (uint32_t o = 0; o < kSnapLen; o += 4) {
    uint8_t* q = base + kSnapLo + o;
    if (std::memcmp(q, g_snap + o, 4) != 0 && is_nan_be(q) && is_nan_be(g_snap + o)) {
      if (getenv("ME_NANDBG") && n_nan_tol < 12) {
        uint32_t a, b;
        std::memcpy(&a, g_snap + o, 4);
        std::memcpy(&b, q, 4);
        uint32_t c;
        std::memcpy(&c, pre + o, 4);
        std::printf("   nan-tol: off %#x orig=%08x native=%08x initial=%08x\n", o, __builtin_bswap32(a), __builtin_bswap32(b), __builtin_bswap32(c));
      }
      std::memcpy(q, g_snap + o, 4);
      n_nan_tol++;
    }
  }
  if (bad) {
    n_log_bad++;
    me::hot::St32(base, kMarker, 0xBADBAD01u);
  }
}

uint32_t fb_ordinary(fuzz::Rng& r) {
  // wide exponent range: products reach denormals / overflow (flush mode matters)
  const int e = int(r.below(120)) - 60;
  float f = float(1 + r.below(1000)) * (1.0f / 512.0f) * std::ldexp(1.0f, e);
  if (r.chance(0.5)) f = -f;
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}
void put_matrix(fuzz::Rng& r, uint8_t* base, uint32_t addr) {
  uint32_t m[16];
  uint32_t kind = r.below(10);
  if (getenv("ME_KIND")) kind = atoi(getenv("ME_KIND"));
  for (int i = 0; i < 16; ++i) {
    switch (kind) {
      case 0: case 1: case 2: m[i] = fb_ordinary(r); break;
      case 3: m[i] = (i % 5 == 0) ? 0x3F800000u : 0; break;                     // identity
      case 4: m[i] = r.chance(0.2) ? r.float_bits() : fb_ordinary(r); break;    // some special values
      case 5: m[i] = r.float_bits(); break;                                     // everything special
      case 6: m[i] = r.chance(0.3) ? (0x7F800000u | (r.u32() & 0x807FFFFFu)) : fb_ordinary(r); break;  // NaN payloads / inf
      case 7: m[i] = r.chance(0.5) ? r.below(0x800000u) | (r.chance(0.5) ? 0x80000000u : 0) : fb_ordinary(r); break;  // denormals
      case 8: m[i] = r.chance(0.5) ? 0x1A000000u + r.below(0x03000000u) : fb_ordinary(r); break;  // tiny/underflow
      default: {
        float f = float(int32_t(r.below(41)) - 20) * (1.0f / 8.0f);  // exactly representable small values
        std::memcpy(&m[i], &f, 4);
      }
    }
  }
  if (getenv("ME_NOPAYLOAD"))
    for (int i = 0; i < 16; ++i)
      if ((m[i] & 0x7F800000u) == 0x7F800000u && (m[i] & 0x7FFFFFu)) m[i] = 0x7FC00000u;
  if (kind == 4 || kind == 5 || kind == 6 || kind == 7) n_special++;
  for (int i = 0; i < 16; ++i) me::hot::St32(base, addr + 4 * i, m[i]);
}

void gen(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  using me::hot::St16;
  using me::hot::St32;
  using me::hot::Ld32;
  const uint32_t obj = kWin + 0x100, mesh = kWin + 0x500, remap = kWin + 0x700, arr = kWin + 0xA00, tab = kWin + 0xB00,
                 lobj = kWin + 0xC00, lst = kWin + 0x1000, mapt = kWin + 0x1200;
  const uint32_t matsR = kWin + 0x2000, matsO = kWin + 0x4000, matsM = kWin + 0x6000, dest = kWin + 0x8000;
  // element count n (= [mesh+176]), the mesh-matrix bound
  uint32_t n;
  const uint32_t kn = r.below(20);
  if (kn < 14) n = r.range(1, 40);
  else if (kn < 16) n = 0;
  else if (kn < 18) n = r.range(100, 130);
  else if (kn < 19) n = 0x80000000u | r.below(100);  // negative
  else n = r.below(3) ? 0xFFFFFFFFu : 0x7FFFFFFFu;    // -1 / INT_MAX
  if (int32_t(n) < 0) n_neg_cnt++;
  St32(base, mesh + 176, n);
  const uint32_t bound = (int32_t(n) > 0 && n < 140) ? n : r.range(1, 130);
  // array header {data, count, capacity}
  uint32_t data = dest;
  const uint32_t kd = r.below(20);
  if (kd == 0) data = 0;
  else if (kd < 3) { data = matsM + 4 * r.below(0x300); n_alias_dest++; }   // output overlaps the mesh matrices
  else if (kd == 3) { data = matsO + 4 * r.below(0x300); n_alias_dest++; }
  else if (kd == 4) { data = matsR + 4 * r.below(0x300); n_alias_dest++; }
  else if (kd == 5) data = dest + 4 * r.below(0x20);
  St32(base, arr + 0, data);
  St32(base, arr + 4, r.u32());
  const uint32_t kc = r.below(10);
  St32(base, arr + 8, kc < 5 ? n : (kc < 6 ? 0 : (kc < 7 ? n + 1 : r.u32())));  // equal capacity (no reallocation) is the common case
  // objects
  St32(base, obj + 640, mesh);
  St32(base, obj + 720, r.chance(0.85) ? remap : 0);
  if (Ld32(base, obj + 720) == 0) n_noremap_ptr++;
  const uint32_t v128 = r.u32();
  St32(base, mesh + 128, v128);
  St32(base, obj + 728, r.chance(0.7) ? v128 : r.u32());
  St32(base, obj + 724, mapt);
  St32(base, mesh + 160, tab);
  St32(base, mesh + 172, matsM);
  uint32_t bR = r.chance(0.9) ? bound + r.below(3) : r.below(140);  // [remap+688]
  uint32_t bO = r.chance(0.9) ? bound + r.below(3) : r.below(140);  // [obj+688]
  if (r.chance(0.05)) bR = 0x80000000u | r.below(10);
  if (r.chance(0.05)) bO = 0x80000000u | r.below(10);
  St32(base, remap + 688, bR);
  St32(base, obj + 688, bO);
  St32(base, remap + 684, r.chance(0.97) ? matsR : matsR + 4 * r.below(0x40));
  St32(base, obj + 684, r.chance(0.97) ? matsO : matsO + 4 * r.below(0x40));
  // table of list objects: tab[idx] = lobj
  const uint32_t lod = r.below(16);
  for (int i = 0; i < 16; ++i) St32(base, tab + 4 * i, (uint32_t(i) == lod) ? lobj : r.u32());
  // list object: u16 bone indices
  int32_t nl;
  const uint32_t kl = r.below(20);
  if (kl < 14) nl = int32_t(r.range(1, 24));
  else if (kl < 16) nl = 0;
  else if (kl < 17) nl = -int32_t(r.range(1, 5));
  else nl = int32_t(r.range(25, 100));
  if (r.chance(0.04)) nl = 0;
  if (int32_t(nl) <= 0) n_empty++;
  St32(base, lobj + 24, lst);
  St32(base, lobj + 28, uint32_t(nl));
  const uint32_t nb = std::min<uint32_t>(std::max<int32_t>(int32_t(n), 1), 128);
  for (int i = 0; i < 100; ++i) {
    uint32_t idx;
    const uint32_t k = r.below(20);
    if (k < 15) idx = r.below(nb);
    else if (k < 18) idx = r.below(128);
    else idx = nb + r.below(4);
    if (idx > 127) idx = 127;
    St16(base, lst + 2 * i, uint16_t(idx));
  }
  // remap table: idx -> matrix index in the other object
  for (int i = 0; i < 128; ++i) {
    uint32_t m;
    const uint32_t k = r.below(25);
    if (k < 19) m = r.below(std::min<uint32_t>(std::max<int32_t>(int32_t(bR), 1), 130));
    else if (k < 21) m = uint32_t(-int32_t(r.range(1, 5)));
    else if (k < 23) m = bR + r.below(4);
    else m = r.chance(0.5) ? 0x80000000u : (r.u32() & 0x7F);
    St32(base, mapt + 4 * i, m);
  }
  // matrices
  for (uint32_t i = 0; i < 130; ++i) {
    put_matrix(r, base, matsR + 64 * i);
    put_matrix(r, base, matsO + 64 * i);
    put_matrix(r, base, matsM + 64 * i);
  }
  if (getenv("ME_NOPAYLOAD"))  // every NaN word in the output/table area (incl. random garbage) becomes the default NaN
    for (uint32_t o = 0x2000; o < 0x12000; o += 4) {
      const uint32_t v = Ld32(base, kWin + o);
      if ((v & 0x7F800000u) == 0x7F800000u && (v & 0x7FFFFFu)) St32(base, kWin + o, 0x7FC00000u);
    }
  // identity constant (outside the window): written to A here, to B by nat_w
  const bool ident = r.chance(0.7);
  for (int i = 0; i < 16; ++i) g_ident[i] = ident ? ((i % 5 == 0) ? 0x3F800000u : 0u) : r.float_bits();
  if (getenv("ME_NOPAYLOAD"))
    for (int i = 0; i < 16; ++i)
      if ((g_ident[i] & 0x7F800000u) == 0x7F800000u && (g_ident[i] & 0x7FFFFFu)) g_ident[i] = 0x7FC00000u;
  put_ident(base);
  g_ident_serial++;
  // a few iterations: alias the array header onto the mesh object (count/capacity/data fields shared with [mesh+172..180])
  if (r.chance(0.05)) {
    const uint32_t a2 = mesh + 168 + 4 * r.below(4);  // arr = mesh + 168..180: {data,count,cap} overlays the mesh fields
    g_stub_ret = r.chance(0.5) ? r.u32() : (r.chance(0.5) ? uint32_t(-int32_t(r.range(1, 70))) : r.range(0, 70));
    ctx.r3.u64 = a2;
    St32(base, lobj + 28, 0);  // no bones: the helpers' behaviour on aliased memory only
    if (r.chance(0.5)) St32(base, a2 + 8, r.u32());
    n_empty++;
  } else {
    g_stub_ret = kHeap;
    ctx.r3.u64 = arr | (r.chance(0.2) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0);
  }
  ctx.r4.u64 = obj | (r.chance(0.2) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0);
  ctx.r5.u64 = lod | (r.chance(0.3) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0);
  // coverage classification from the memory state (the original's own decisions, recomputed)
  {
    const uint32_t mesh_ = Ld32(base, obj + 640), remap_ = Ld32(base, obj + 720);
    const uint32_t cnt = Ld32(base, mesh_ + 176);
    const uint32_t a = ctx.r3.u32;
    const uint32_t capv = Ld32(base, a + 8), datav = Ld32(base, a);
    if (capv != cnt && (datav != 0 || cnt != 0)) n_alloc1++;
    const bool use_remap = remap_ != 0 && Ld32(base, obj + 728) == Ld32(base, mesh_ + 128);
    const uint32_t lo = Ld32(base, Ld32(base, mesh_ + 160) + (ctx.r5.u32 << 2));
    const int32_t nn = int32_t(Ld32(base, lo + 28));
    if (nn > 0) {
      (use_remap ? n_remap : n_direct)++;
      for (int32_t k = 0; k < nn; ++k) {
        const uint32_t idx = me::hot::Ld16(base, Ld32(base, lo + 24) + 2 * k);
        const int32_t nbv = int32_t(Ld32(base, mesh_ + 176));
        if (int32_t(idx) >= nbv) { n_fb_idx++; continue; }
        if (use_remap) {
          const int32_t m = int32_t(Ld32(base, Ld32(base, obj + 724) + idx * 4));
          if (m < 0) { n_fb_neg++; continue; }
          if (m >= int32_t(Ld32(base, remap_ + 688))) { n_fb_bound++; continue; }
        } else if (int32_t(idx) >= int32_t(Ld32(base, obj + 688))) { n_fb_obj++; continue; }
        n_ok++;
      }
    }
  }
  n_gen++;
}

struct Init {
  Init() {
    me::hot::n_826545D0::alloc_unbounded = false;
    me::hot::n_826545D0::alloc_hint = kHeap;
  }
} init;

}  // namespace c826545D0
}  // namespace

extern "C" void sub_822100F8(PPCContext& c, uint8_t*) { c826545D0::Stub(c); }
extern "C" void __imp__sub_822100F8(PPCContext& c, uint8_t*) { c826545D0::Stub(c); }

namespace {
fuzz::Registrar reg_826545D0({"sub_826545D0", c826545D0::orig_w, c826545D0::nat_w, me::hot::n_826545D0::Writes,
                              me::hot::n_826545D0::kCmp, 0x40000000, 0x20000, c826545D0::gen});
}  // namespace

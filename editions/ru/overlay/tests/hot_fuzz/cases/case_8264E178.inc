// ORIG: sub_8264EBB0 sub_82210290 sub_82210360 sub_82654348
// NATIVE: n_8264E178.h
// Russian edition case: the originals are the RU functions (sub_8264EBB0 = English sub_8264E178, FDynamicSpriteEmitterData::
// Render). Run it against the RU generated code (out/edition-ru/app/generated/default), see the native header.
// Environment of the case:
//  * the five virtual calls (material proxy -> GetMaterial, material vfuncs 60 / 56 / 28, this->RenderDebug) go through the
//    dispatch table the generated REX_CALL_INDIRECT_FUNC reads: host stubs that return scripted values (garbage upper halves),
//    may write one scripted word into the emitter / proxy objects (ordering of the reloads), trash r4-r12 / f0-f13, and log
//    what the callee can observe (r1, r3-r10);
//  * sub_822100F8 (allocator), sub_82654A68 (sort), sub_82973160 (memcpy), __imp__sub_8250C628 (DrawRichMesh) are stubs
//    that log their inputs (the sort a hash of the array, DrawRichMesh r1, r3-r10, f1-f13 and a hash of the mesh element in
//    the caller's frame r1+80..r1+392 and of the vertex factory): original and native logs must be identical (a mismatch
//    writes a marker word into the window, which the harness reports). memcpy copies for real.
//  * sub_82210290 / sub_82210360 (TArray AddUninitialized / Empty) and sub_82654348 (axis lock values) are the recompiled
//    originals; the constants outside the window (0.0 / 1.0 words, identity matrix) are refilled identically for both runs.
// Coverage (printed): render modes, material decisions, local / world space, particle counts, exact capacity / realloc,
// aliasing of the order array with the indices, the particle records, the emitter object and the proxy matrices.
extern "C" void __imp__sub_8264EBB0(PPCContext&, uint8_t*);
namespace rex::runtime {
__attribute__((weak)) PPCFunc* ResolveIndirectFunction(uint32_t) { return nullptr; }
}  // namespace rex::runtime
namespace {
namespace c8264E178 {
using namespace me::hot;
constexpr uint32_t kWin = 0x40000000, kLen = 0x20000;
constexpr uint32_t THIS = kWin + 0x100, VF = kWin + 0x200, PROXY = kWin + 0x300, VIEW = kWin + 0x480, MATP = kWin + 0x600,
                   VTMP = kWin + 0x640, MAT = kWin + 0x680, VTM = kWin + 0x6C0, VTT = kWin + 0x700, PDI = kWin + 0x780,
                   IDX = kWin + 0x1000, PD = kWin + 0x2000, ORD = kWin + 0xE000, HEAP = kWin + 0x10000, kMarker = kWin + 0x1A000;
constexpr uint32_t kT0 = 0x82210000u + 4 * 200, kT60 = kT0 + 4, kT56 = kT0 + 8, kT28 = kT0 + 12, kTDbg = kT0 + 16;

std::vector<uint64_t> g_log, g_orig_log;
uint64_t g_seed = 0;
struct Script {
  uint64_t ret[8];
  uint32_t waddr[8], wval[8];
  bool write[8];
} g_s;
int g_calls = 0;
bool g_pure = getenv("ME_BENCH_PURE") != nullptr;
uint64_t n_gen = 0, n_mode[4] = {}, n_nomat = 0, n_sort = 0, n_nosort = 0, n_local = 0, n_world = 0, n_n0 = 0, n_nbig = 0,
         n_exact = 0, n_realloc = 0, n_alias[5] = {}, n_lock = 0, n_log_bad = 0, n_particles = 0;

uint64_t Mix(uint64_t x) {
  x ^= x >> 31;
  x *= 0x9E3779B97F4A7C15ull;
  x ^= x >> 29;
  x *= 0xBF58476D1CE4E5B9ull;
  return x ^ (x >> 32);
}
uint64_t Hash(const uint8_t* base, uint32_t a, uint32_t len) {
  uint64_t h = 0x12345;
  if (a < kWin || uint64_t(a) + len > uint64_t(kWin) + kLen) return Mix(a) ^ len;
  for (uint32_t k = 0; k < len; ++k) h = Mix(h + base[a + k] + (uint64_t(k) << 40));
  return h;
}
void Trash(PPCContext& c, uint64_t tag) {
  c.r4.u64 = 0xDEAD0004DEAD0004ull ^ tag; c.r5.u64 = 0xDEAD0005DEAD0005ull ^ tag; c.r6.u64 = 0xDEAD0006DEAD0006ull ^ tag;
  c.r7.u64 = 0xDEAD0007DEAD0007ull ^ tag; c.r8.u64 = 0xDEAD0008DEAD0008ull ^ tag; c.r9.u64 = 0xDEAD0009DEAD0009ull ^ tag;
  c.r10.u64 = 0xDEAD0010DEAD0010ull ^ tag; c.r11.u64 = 0xDEAD0011DEAD0011ull ^ tag; c.r12.u64 = 0xDEAD0012DEAD0012ull ^ tag;
  PPCRegister* f[14] = {&c.f0, &c.f1, &c.f2, &c.f3, &c.f4, &c.f5, &c.f6, &c.f7, &c.f8, &c.f9, &c.f10, &c.f11, &c.f12, &c.f13};
  for (int i = 0; i < 14; ++i) f[i]->u64 = 0x3FF0000000000000ull + (tag & 0xFFFF) * 16 + i;
}
void LogRegs(const PPCContext& c, uint64_t id) {
  if (g_pure) return;  // benchmark: stubs as cheap as possible
  g_log.push_back(id);
  g_log.push_back(c.r1.u64);
  const PPCRegister* r[8] = {&c.r3, &c.r4, &c.r5, &c.r6, &c.r7, &c.r8, &c.r9, &c.r10};
  for (auto* x : r) g_log.push_back(x->u64);
}
// Virtual-call stubs: call k of the run returns g_s.ret[k] and may write one word.
void VCommon(PPCContext& c, uint8_t* base, uint64_t id) {
  LogRegs(c, id);
  const int k = g_calls < 8 ? g_calls : 7;
  ++g_calls;
  St32(base, c.r1.u32 - 64, 0xA5A5A5A5u);
  if (g_s.write[k]) St32(base, g_s.waddr[k], g_s.wval[k]);
  c.r3.u64 = g_s.ret[k];
  Trash(c, id);
}
void V0(PPCContext& c, uint8_t* b) { VCommon(c, b, 100); }
void V60(PPCContext& c, uint8_t* b) { VCommon(c, b, 160); }
void V56(PPCContext& c, uint8_t* b) { VCommon(c, b, 156); }
void V28(PPCContext& c, uint8_t* b) { VCommon(c, b, 128); }
void VDbg(PPCContext& c, uint8_t* b) { VCommon(c, b, 104); }

void Env(uint8_t* base) {
  auto slot = [&](uint32_t t, PPCFunc* f) {
    *reinterpret_cast<PPCFunc**>(base + 0x82000000ull + 0xFC0000ull + (uint64_t(t) - 0x82210000ull) * 2) = f;
  };
  slot(kT0, V0); slot(kT60, V60); slot(kT56, V56); slot(kT28, V28); slot(kTDbg, VDbg);
  // constants outside the window: 0.0 / 1.0 words and the identity matrix (random bit patterns per iteration)
  uint64_t s = g_seed;
  auto nx = [&]() { s = Mix(s + 0x9E37); return uint32_t(s); };
  const uint32_t zero = (nx() & 7) ? 0u : nx(), one = (nx() & 7) ? 0x3F800000u : nx();
  St32(base, 0x820B9098u, zero);
  St32(base, 0x820BCD44u, one);
  for (uint32_t k = 0; k < 16; ++k) St32(base, 0x82E64F50u + 4 * k, (nx() & 3) ? ((k % 5) ? 0u : 0x3F800000u) : nx());
}

void Run(PPCContext& ctx, uint8_t* base, bool orig) {
  Env(base);
  g_log.clear();
  g_calls = 0;
  if (orig) __imp__sub_8264EBB0(ctx, base);
  else me::hot::n_8264E178::Native(ctx, base);
}
void orig_w(PPCContext& ctx, uint8_t* base) {
  if (g_pure) { Env(base); g_calls = 0; return __imp__sub_8264EBB0(ctx, base); }
  Run(ctx, base, true);
  g_orig_log = g_log;
}
void nat_w(PPCContext& ctx, uint8_t* base) {
  if (g_pure) { Env(base); g_calls = 0; return me::hot::n_8264E178::Native(ctx, base); }
  Run(ctx, base, false);
  if (g_log != g_orig_log) {
    ++n_log_bad;
    St32(base, kMarker, 0xBADBAD01u);
  }
}
void writes_w(const PPCContext&, const uint8_t*, me::hot::Writes& w) { w.Add(kWin, kLen); }

uint32_t fl(fuzz::Rng& r) {
  // benchmark: realistic finite data (NaN inputs send the keys through the generated statements, see the header)
  switch (g_pure ? 4 + r.below(8) : r.below(12)) {
    case 0: return r.float_bits();
    case 1: return r.u32();
    case 2: return 0x7FC00000u;
    case 3: return r.below(1u << 23);  // denormal
    default: {
      float f = float(int32_t(r.u32() % 200001) - 100000) * (1.0f / 64.0f);
      uint32_t b;
      std::memcpy(&b, &f, 4);
      return b;
    }
  }
}

void gen(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  ++n_gen;
  g_seed = r.u64();
  // render mode
  const uint32_t km = r.below(100);
  const int32_t mode = km < 82 ? 0 : km < 88 ? 1 : km < 94 ? 2 : (r.chance(0.5) ? 3 : -1);
  ++n_mode[mode == 0 ? 0 : mode == 1 ? 1 : mode == 2 ? 2 : 3];
  St32(base, THIS + 0, VTT);
  St32(base, VTT + 4, kTDbg);
  St32(base, THIS + 72, uint32_t(mode));
  St8(base, THIS + 52, uint8_t(r.u32()));
  const bool lock = r.chance(0.5);
  n_lock += lock;
  St32(base, THIS + 60, lock ? (r.chance(0.5) ? 1 + r.below(6) : r.u32()) : 0);
  St8(base, THIS + 64, uint8_t(r.below(8)));  // read by sub_82654348 (axis lock mode)
  St32(base, THIS + 128, VF);
  St32(base, THIS + 44, MATP);
  St32(base, MATP + 0, VTMP);
  St32(base, VTMP + 0, kT0);
  St32(base, MAT + 0, VTM);
  St32(base, VTM + 60, kT60);
  St32(base, VTM + 56, kT56);
  St32(base, VTM + 28, kT28);
  const bool local = r.chance(0.5);
  (local ? n_local : n_world)++;
  St32(base, THIS + 56, local ? (r.chance(0.8) ? 1 : r.u32()) : 0);
  St32(base, THIS + 96, r.u32());
  // proxy matrices / view column
  for (uint32_t k = 0; k < 80; ++k) St32(base, PROXY + 16 + 4 * k, fl(r));
  St32(base, PROXY + 16, PDI);
  for (uint32_t k = 0; k < 80; ++k) St32(base, VIEW + 4 * k, fl(r));
  // particles
  int32_t n;
  const uint32_t kn = r.below(100);
  if (kn < 70) n = int32_t(r.range(1, 64));
  else if (kn < 85) n = int32_t(r.range(65, 400));
  else if (kn < 92) n = 0;
  else if (kn < 96) n = -int32_t(r.range(1, 5));
  else n = int32_t(r.range(1, 8));
  if (n <= 0) ++n_n0;
  if (n > 64) ++n_nbig;
  St32(base, THIS + 8, uint32_t(n));
  int32_t stride;
  const uint32_t ks = r.below(20);
  if (ks < 15) stride = int32_t(4 * r.range(8, 40));
  else if (ks < 17) stride = int32_t(r.range(28, 100));  // odd strides
  else if (ks < 18) stride = 0;
  else stride = -int32_t(4 * r.range(1, 8));
  const uint32_t pdata = stride < 0 ? PD + 0x8000 : PD;
  St32(base, THIS + 16, uint32_t(stride));
  St32(base, THIS + 20, pdata);
  St32(base, THIS + 24, IDX);
  const uint32_t lim = stride > 0 ? std::min<uint32_t>(0xA000u / uint32_t(stride), 2000u) : 64u;
  for (uint32_t k = 0; k < 0x800; ++k) St16(base, IDX + 2 * k, uint16_t(r.below(lim)));
  for (uint32_t a = PD; a < PD + 0xB800; a += 4) St32(base, a, fl(r));
  if (n > 0) n_particles += uint32_t(n);
  // order array {data, count, capacity}
  uint32_t data = ORD + 8 * r.below(4), cap = n > 0 ? uint32_t(n) : r.below(4);
  const uint32_t ka = r.below(g_pure ? 75 : 100);  // benchmark repeats: no aliasing (the stores would hit the vtables)
  if (ka < 60) { ++n_exact; }
  else if (ka < 75) { cap = r.below(600); ++n_realloc; }
  else if (ka < 80) { ++n_alias[0]; data = IDX + 2 * r.below(64); }                  // overlaps the indices
  else if (ka < 85) { ++n_alias[1]; data = PD + 4 * r.below(0x400); }               // overlaps the particle records
  else if (ka < 90) { ++n_alias[2]; data = THIS + 4 * r.below(40) - 8 * r.below(4); }  // overlaps the emitter object
  else if (ka < 95) { ++n_alias[3]; data = PROXY + 4 * r.below(30); }               // overlaps the proxy matrices
  else { ++n_alias[4]; data = VIEW + 240 + 4 * r.below(10); }                        // overlaps the view column
  St32(base, THIS + 116, data);
  St32(base, THIS + 120, r.u32());
  St32(base, THIS + 124, cap);
  // vertex factory
  for (uint32_t k = 0; k < 16; ++k) St32(base, VF + 160 + 4 * k, r.u32());
  // scripted virtual calls: GetMaterial, vfunc60, vfunc56, vfunc28 (in that order on the path that reaches them)
  const uint64_t garb = r.chance(0.3) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0;
  g_s.ret[0] = r.chance(0.1) ? garb : (MAT | garb);
  if (g_s.ret[0] == garb) ++n_nomat;
  g_s.ret[1] = (r.chance(0.8) ? 2 : r.u32()) | garb;
  g_s.ret[2] = (r.chance(0.5) ? 2 : r.below(4)) | garb;
  g_s.ret[3] = (r.chance(0.5) ? 0 : r.chance(0.5) ? 1 : r.u32()) | garb;
  for (int k = 4; k < 8; ++k) g_s.ret[k] = r.u64();
  static const uint32_t kAddr[] = {THIS + 8, THIS + 56, THIS + 16, THIS + 24, THIS + 120, THIS + 124, PROXY + 52, VIEW + 280,
                                   THIS + 60, IDX + 4};
  for (int k = 0; k < 8; ++k) {
    g_s.write[k] = r.chance(0.15);
    g_s.waddr[k] = kAddr[r.below(10)];
    g_s.wval[k] = r.chance(0.5) ? r.below(8) : fl(r);
  }
  const bool sorts = mode == 0 && g_s.ret[0] != garb && int32_t(uint32_t(g_s.ret[1])) == 2 &&
                     (int32_t(uint32_t(g_s.ret[2])) == 2 || int32_t(uint32_t(g_s.ret[3])) != 0);
  (sorts ? n_sort : n_nosort)++;
  // registers: r3 = proxy, r4 = this, r5 = PDI, r6 = view, r7 = DPG (garbage upper halves sometimes)
  auto g = [&](uint32_t v) { return uint64_t(v) | (r.chance(0.2) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0); };
  ctx.r3.u64 = g(PROXY);
  ctx.r4.u64 = g(THIS);
  ctx.r5.u64 = g(PDI);
  ctx.r6.u64 = g(VIEW);
  ctx.r7.u64 = r.chance(0.5) ? r.below(4) : r.u64();
  Env(base);
}

struct Stat {
  ~Stat() {
    std::printf("   [8264E178 coverage: gen=%llu mode 0/1/2/other=%llu/%llu/%llu/%llu no-material=%llu sorted=%llu not-sorted=%llu "
                "local=%llu world=%llu | n<=0=%llu n>64=%llu particles=%llu | order array: exact-capacity=%llu realloc=%llu "
                "alias indices/records/emitter/proxy/view=%llu/%llu/%llu/%llu/%llu | lock-axis=%llu | callee-visible-state log "
                "mismatches=%llu]\n",
                (unsigned long long)n_gen, (unsigned long long)n_mode[0], (unsigned long long)n_mode[1],
                (unsigned long long)n_mode[2], (unsigned long long)n_mode[3], (unsigned long long)n_nomat,
                (unsigned long long)n_sort, (unsigned long long)n_nosort, (unsigned long long)n_local,
                (unsigned long long)n_world, (unsigned long long)n_n0, (unsigned long long)n_nbig,
                (unsigned long long)n_particles, (unsigned long long)n_exact, (unsigned long long)n_realloc,
                (unsigned long long)n_alias[0], (unsigned long long)n_alias[1], (unsigned long long)n_alias[2],
                (unsigned long long)n_alias[3], (unsigned long long)n_alias[4], (unsigned long long)n_lock,
                (unsigned long long)n_log_bad);
  }
} stat;
}  // namespace c8264E178
}  // namespace

// Callee stubs (both runs call the same ones).
extern "C" void sub_822100F8(PPCContext& c, uint8_t*) {  // allocator: Realloc(r3, r4, r5)
  using namespace c8264E178;
  LogRegs(c, 200);
  c.r3.u64 = (Mix(c.r3.u64) & 0xFFFFFFFF00000000ull) | (HEAP + 0x800 * uint32_t(((c.r3.u32 >> 4) + c.r4.u32) % 12));
  Trash(c, 200);
}
extern "C" void __imp__sub_822100F8(PPCContext& c, uint8_t* b) { sub_822100F8(c, b); }
extern "C" void sub_82654A68(PPCContext& c, uint8_t* base) {  // sort: log the array
  using namespace c8264E178;
  LogRegs(c, 300);
  const int32_t n = c.r4.s32;
  if (!g_pure) g_log.push_back(Hash(base, c.r3.u32, n > 0 ? uint32_t(std::min(n, 1024)) * 8 : 0));
  me::hot::St32(base, c.r1.u32 - 32, 0x5A5A5A5Au);
  c.r3.u64 = 0xDEAD0003DEAD0003ull;
  Trash(c, 300);
}
extern "C" void sub_82973160(PPCContext& c, uint8_t* base) {  // memcpy(r3, r4, r5)
  using namespace c8264E178;
  LogRegs(c, 400);
  for (uint32_t k = 0; k < c.r5.u32; ++k) me::hot::St8(base, c.r3.u32 + k, me::hot::Ld8(base, c.r4.u32 + k));
  Trash(c, 400);
}
extern "C" void __imp__sub_8250C628(PPCContext& c, uint8_t* base) {  // DrawRichMesh
  using namespace c8264E178;
  LogRegs(c, 500);
  const PPCRegister* f[13] = {&c.f1, &c.f2, &c.f3, &c.f4, &c.f5, &c.f6, &c.f7, &c.f8, &c.f9, &c.f10, &c.f11, &c.f12, &c.f13};
  if (!g_pure) {
    for (auto* x : f) g_log.push_back(x->u64);
    g_log.push_back(Hash(base, c.r1.u32 + 80, 312));
    g_log.push_back(Hash(base, VF + 160, 64));
    g_log.push_back(c.fpscr.csr);
  }
  me::hot::St32(base, c.r1.u32 - 48, 0x5A5A5A5Au);
  c.r3.u64 = Mix(c.r3.u64 ^ g_log.size());
  Trash(c, 500);
}
namespace {
fuzz::Registrar reg_8264E178({"sub_8264EBB0(8264E178)", c8264E178::orig_w, c8264E178::nat_w, c8264E178::writes_w,
                              me::hot::n_8264E178::kCmp, c8264E178::kWin, c8264E178::kLen, c8264E178::gen,
                              me::hot::n_8264E178::Writes});  // guard replay: the console guard's write set
}  // namespace

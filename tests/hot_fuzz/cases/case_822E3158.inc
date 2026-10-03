// ORIG: sub_822E3158 sub_822DDC80
// NATIVE: n_822E3158.h
// Cast<T> family (see n_822E3158.h). The static-class singleton word lives at 0x82EAF480 (outside the fuzz window), so this case
// copies it into arena B itself (generator -> snapshot -> native wrapper). The singleton is never 0 for a non-null object
// (that first call is delegated to the original and declared as Writes() overflow); null objects are tested with both.
extern "C" void __imp__sub_822E3158(PPCContext&, uint8_t*);
// The getter's first-call path (singleton still null) is never executed here (see above); stubs only satisfy the linker.
extern "C" __attribute__((weak)) void __imp__sub_827EA158(PPCContext&, uint8_t*) { __builtin_trap(); }
extern "C" __attribute__((weak)) void __imp__sub_82308A20(PPCContext&, uint8_t*) { __builtin_trap(); }
extern "C" __attribute__((weak)) void __imp__sub_8255A5D0(PPCContext&, uint8_t*) { __builtin_trap(); }
namespace {
namespace c822E3158 {
uint32_t g_sing = 0;
uint64_t g_gen = 0, g_synced = ~0ull;
uint64_t g_cov[8];

void put32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}

void gen(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  ++g_gen;
  const uint32_t win = 0x40000000;
  // class nodes: 96 bytes each, super pointer at +60 (strictly increasing index or 0: the chains terminate)
  const int nodes = 24;
  const uint32_t cbase = win + 0x1000 + r.below(8) * 4;
  auto node = [&](int i) { return cbase + 96u * uint32_t(i); };
  for (int i = 0; i < nodes; ++i) {
    uint32_t sup = 0;
    if (i + 1 < nodes && !r.chance(0.12)) sup = node(i + 1 + int(r.below((nodes - i - 1 < 3 ? uint32_t(nodes - i - 1) : 3u))));
    put32(base, node(i) + 60, sup);
  }
  std::memset(base + win + 0xA000, 0, 0x400);  // foreign class pointers point to zeroed memory (chain ends)
  // objects
  const uint32_t obj = win + 0x8000 + r.below(0x100) * 16;
  uint32_t cls_of_obj;
  switch (r.below(8)) {
    case 0: cls_of_obj = 0; break;                                  // no class
    case 1: cls_of_obj = win + 0xA000 + r.below(64) * 4; break;     // class pointer outside the node array
    default: cls_of_obj = node(int(r.below(nodes)));
  }
  put32(base, obj + 52, cls_of_obj);
  // singleton: a node (hit when it is on the chain), a foreign pointer, or the object's own class
  uint32_t sing;
  switch (r.below(6)) {
    case 0: sing = cls_of_obj ? cls_of_obj : node(0); break;
    case 1: sing = win + 0xA100 + r.below(64) * 4; break;
    case 2: sing = obj; break;
    default: sing = node(int(r.below(nodes)));
  }
  if (r.chance(0.03)) sing = r.u32() | 1u;
  g_sing = sing;
  put32(base, 0x82EAF480, sing);
  uint32_t o = r.chance(0.12) ? 0 : obj;
  if (o == 0 && r.chance(0.5)) g_sing = 0, put32(base, 0x82EAF480, 0);  // null object with a null singleton: no getter call
  if (o != 0 && g_sing == 0) g_sing = 1, put32(base, 0x82EAF480, 1);
  ctx.r3.u64 = o | (r.chance(0.3) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0);
  if (o == 0 && r.chance(0.5)) ctx.r3.u64 = r.u64() & 0xFFFFFFFF00000000ull;
}

void nat(PPCContext& ctx, uint8_t* baseB) {
  if (g_synced != g_gen) {
    put32(baseB, 0x82EAF480, g_sing);
    g_synced = g_gen;
  }
  me::hot::n_822E3158::Native(ctx, baseB);
}
fuzz::Registrar reg_822E3158({"sub_822E3158", __imp__sub_822E3158, nat, me::hot::n_822E3158::Writes,
                            me::hot::n_822E3158::kCmp, 0x40000000, 0x20000, gen});
}  // namespace c822E3158
}  // namespace

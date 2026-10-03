// ORIG: sub_82BB0748 sub_82BAFF58
// NATIVE: n_82BB0748.h
// Hash table "find" wrapper (see n_82BB0748.h, n_82BAFF58.h): same table generator as case_82BAFF58.inc; r3 = out words,
// r4 = table slot (sometimes pointing at a null table), r5 = object slot. All memory is inside the window; the output
// words sometimes overlap the table / slots.
extern "C" void __imp__sub_82BB0748(PPCContext&, uint8_t*);
namespace {
namespace c82BB0748 {

void put32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}
// Builds a random chained hash table (see n_82BAFF58.h) in the window and returns the interesting pointers.
struct Tab {
  uint32_t slotA, slotB, ebase, mask, b;  // b = object looked up
  uint32_t key;                            // bucket of b
};
Tab build(fuzz::Rng& r, uint8_t* base) {
  const uint32_t win = 0x40000000;
  Tab t;
  t.slotA = win + 0x100 + r.below(8) * 4;
  t.slotB = win + 0x200 + r.below(8) * 4;
  t.ebase = win + 0x1000 + r.below(8) * 4;
  const uint32_t size = 8u << r.below(3);
  t.mask = r.chance(0.9) ? size - 1 : (size - 1) | (r.chance(0.5) ? 0x20 : 0);
  const int kObj = 40, kEnt = 64;
  const uint32_t obase = win + 0x4000;
  uint32_t hash[kObj];
  for (int k = 0; k < kObj; ++k) {
    hash[k] = r.u32();
    put32(base, obase + 32 * k + 4, hash[k]);
  }
  put32(base, t.slotA, t.ebase);
  put32(base, t.ebase + 4, t.mask);
  auto ent = [&](int i) { return t.ebase + 8 + 32u * uint32_t(i); };
  int objOf[kEnt];
  int nxt[kEnt];
  for (int i = 0; i < kEnt; ++i) {
    // next: -1, a larger index (chains always terminate)
    uint32_t next;
    if (i + 1 >= kEnt || r.chance(0.45)) next = 0xFFFFFFFFu;
    else next = uint32_t(i + 1 + int(r.below(uint32_t(kEnt - i - 1) < 6 ? uint32_t(kEnt - i - 1) : 6u)));
    if (r.chance(0.04)) next = 0xFFFFFFFEu;  // -2: only special as a bucket head
    nxt[i] = (next >= 0xFFFFFFFEu) ? -1 : int(next);
    // object: for bucket heads mostly one that hashes there
    int k = int(r.below(kObj));
    if (uint32_t(i) <= t.mask && r.chance(0.8)) {
      for (int tries = 0; tries < 20 && (hash[k] & t.mask) != uint32_t(i); ++tries) k = int(r.below(kObj));
    }
    objOf[i] = k;
    put32(base, ent(i), next);
    put32(base, ent(i) + 8, obase + 32 * k);
    put32(base, ent(i) + 4, r.u32());
    put32(base, ent(i) + 12, r.u32());
  }
  put32(base, ent(-2), 0xFFFFFFFFu);  // a -2 'next' read as an index lands here: terminate (no cycles)
  // object b and its bucket
  int bk = int(r.below(kObj));
  t.b = obase + 32 * bk;
  t.key = hash[bk] & t.mask;
  put32(base, t.slotB, t.b);
  if (r.chance(0.25)) put32(base, ent(-2) + 8, t.b);  // a node reached through a '-2' next index matches
  // plant b into the chain of its bucket (most of the time)
  if (r.chance(0.7) && t.key < uint32_t(kEnt)) {
    int cur = int(t.key);
    int len = 0, pick = int(r.below(4));
    for (;;) {
      if (len == pick || nxt[cur] < 0) break;
      cur = nxt[cur];
      ++len;
    }
    put32(base, ent(cur) + 8, t.b);
    if (r.chance(0.3) && cur != int(t.key)) {  // and make the head belong to the bucket
      put32(base, ent(int(t.key)) + 8, obase + 32 * int(r.below(kObj)));
    }
  }
  return t;
}

void gen(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  Tab t = build(r, base);
  const uint32_t win = 0x40000000;
  uint32_t out = win + 0x8000 + r.below(0x40) * 8;
  if (r.chance(0.05)) out = t.slotA;
  if (r.chance(0.03)) out = t.slotB;
  if (r.chance(0.03)) out = t.ebase + 4;
  ctx.r3.u64 = out | (r.chance(0.2) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0);
  ctx.r4.u64 = t.slotA;
  ctx.r5.u64 = t.slotB;
  if (r.chance(0.1)) put32(base, t.slotA, 0);  // null table
  if (r.chance(0.03)) ctx.r5.u64 = t.slotA;
}
fuzz::Registrar reg_82BB0748({"sub_82BB0748", __imp__sub_82BB0748, me::hot::n_82BB0748::Native,
                              me::hot::n_82BB0748::Writes, me::hot::n_82BB0748::kCmp, 0x40000000, 0x20000, gen});
}  // namespace c82BB0748
}  // namespace

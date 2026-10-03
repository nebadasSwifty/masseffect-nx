// ORIG: sub_82210970
// NATIVE: n_82210970.h
// sub_82210970: object iterator advance with an IsA chain test (see src/native/hot/n_82210970.h).
// Environment: the object array global {data, count} at 0x82EC12E0 and the flag word at 0x82EAC8CC are outside the window:
// the generator writes them into arena A and the wrappers put them into the arena the call runs on.
extern "C" void __imp__sub_82210970(PPCContext&, uint8_t*);
namespace {
namespace c82210970 {
constexpr uint32_t kWin = 0x40000000;
uint32_t g_arr_data = 0, g_arr_count = 0, g_flag = 0;
uint64_t n_iters = 0, n_negstart = 0, n_wrap = 0, n_flag0 = 0, n_alias_it = 0;
void Env(uint8_t* base) {
  me::hot::St32(base, me::hot::n_82210970::kArr + 0, g_arr_data);
  me::hot::St32(base, me::hot::n_82210970::kArr + 4, g_arr_count);
  me::hot::St32(base, me::hot::n_82210970::kFlag, g_flag);
}
bool g_aliased = false;
uint64_t n_ret_end = 0, n_ret_chain = 0, n_ret_any = 0, n_skip_null = 0, n_skip_mask = 0, n_skip_chain = 0;
void orig_w(PPCContext& ctx, uint8_t* base) {
  Env(base);
  const uint32_t it = ctx.r3.u32;
  const uint32_t start = me::hot::Ld32(base, it + 4);
  const uint32_t mask = g_flag == 0 ? 0x402u : 0x2u;
  const uint32_t want = me::hot::Ld32(base, it + 0);
  __imp__sub_82210970(ctx, base);
  const uint32_t end = me::hot::Ld32(base, it + 4);
  // classify the skipped entries (only when the iterator does not alias the array)
  if (!g_aliased) {
    for (uint32_t i = start + 1; i != end && int32_t(i) < int32_t(g_arr_count) && i - start < 300; ++i) {
      const uint32_t obj = me::hot::Ld32(base, g_arr_data + (i << 2));
      if (!obj) { ++n_skip_null; continue; }
      if (me::hot::Ld32(base, obj + 8) & mask) { ++n_skip_mask; continue; }
      ++n_skip_chain;
    }
    if (!(int32_t(end) < int32_t(g_arr_count))) ++n_ret_end;
    else if (want == 0) ++n_ret_any;
    else ++n_ret_chain;
  }
}
void nat_w(PPCContext& ctx, uint8_t* base) { Env(base); me::hot::n_82210970::Native(ctx, base); }
void gen(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  ++n_iters;
  const uint32_t it = kWin + 0x100 + r.below(0x100) * 16;
  const uint32_t arr = kWin + 0x2000;                 // data array (u32 pointers)
  const uint32_t objs = kWin + 0x4000;                // objects, 96 bytes each
  const uint32_t cls = kWin + 0xC000;                 // class nodes, 64 bytes each: +60 next
  const uint32_t count = r.chance(0.9) ? r.below(40) : r.below(200);
  const uint32_t nobj = 24, ncls = 24;
  // class nodes: random chain structure without cycles (next index is always smaller, or 0)
  uint32_t cnode[24];
  for (uint32_t k = 0; k < ncls; ++k) {
    cnode[k] = cls + 64 * k;
    me::hot::St32(base, cnode[k] + 60, (k == 0 || r.chance(0.2)) ? 0 : (r.chance(0.1) ? r.u32() & 0xFFFF0 | 0 : cnode[r.below(k)]));
  }
  // a wild next pointer would leave the window: only allow window/zero values
  for (uint32_t k = 0; k < ncls; ++k) {
    const uint32_t nx = me::hot::Ld32(base, cnode[k] + 60);
    if (nx != 0 && !(nx >= cls && nx < cls + 64 * ncls)) me::hot::St32(base, cnode[k] + 60, 0);
  }
  const bool flag0 = r.chance(0.5);
  g_flag = flag0 ? 0 : (r.chance(0.5) ? 1 : r.u32() | 1);
  if (flag0) ++n_flag0;
  const uint32_t masks[] = {0, 0, 0, 2, 0x400, 0x402, 0x1, 0x80000000u, 0xFFFFFFFFu, 0x200, 0x4};
  for (uint32_t k = 0; k < nobj; ++k) {
    const uint32_t o = objs + 96 * k;
    me::hot::St32(base, o + 8, r.chance(0.7) ? masks[r.below(11)] : r.u32());
    me::hot::St32(base, o + 12, r.u32());
    me::hot::St32(base, o + 52, r.chance(0.15) ? 0 : cnode[r.below(ncls)]);
  }
  // the filter (class pointer): one of the class nodes, zero, or something not in any chain
  const uint32_t want = r.chance(0.15) ? 0 : (r.chance(0.8) ? cnode[r.below(ncls)] : (kWin + 0x1F000 + 4 * r.below(100)));
  me::hot::St32(base, it + 0, want);
  uint32_t start;
  switch (r.below(10)) {
    case 0: start = 0xFFFFFFFFu; break;                 // -1 -> first index 0
    case 1: start = 0x7FFFFFFFu; ++n_wrap; break;        // index wraps to negative (index << 2 wraps to 0)
    case 2: start = uint32_t(-int32_t(r.range(1, 5))); ++n_negstart; break;
    case 3: start = 0xFFFFFFFFu - r.below(3); break;
    default: start = r.below(count + 3); break;
  }
  me::hot::St32(base, it + 4, start);
  // data array
  g_arr_data = (r.chance(0.5)) ? arr : (arr + 4 * r.below(8));
  for (uint32_t k = 0; k < 200 + 8; ++k) {
    const uint32_t v = r.chance(0.15) ? 0 : objs + 96 * r.below(nobj);
    me::hot::St32(base, arr + 4 * k, v);
  }
  g_arr_count = count;
  if (r.chance(0.03)) g_arr_count = uint32_t(-int32_t(r.range(0, 4)));  // negative / zero count: returns at once
  // an iteration from the wrapped index can only terminate by a match: guarantee one (entry 0 matches)
  if (start == 0x7FFFFFFFu) {
    const uint32_t o0 = objs;
    me::hot::St32(base, o0 + 8, 0);
    me::hot::St32(base, o0 + 52, want);
    for (uint32_t k = 0; k < 8; ++k) me::hot::St32(base, g_arr_data + 4 * k, o0);
  }
  uint32_t it_used = it;
  g_aliased = false;
  if (r.chance(0.05) && int32_t(start) >= 0 && start < 20) {
    // the iterator's index word is one of the array slots (data[start + 2]): the loop's own store changes the array
    it_used = g_arr_data + 4 * (start + 2) - 4;
    me::hot::St32(base, it_used + 0, want);
    me::hot::St32(base, it_used + 4, start);
    ++n_alias_it;
    g_aliased = true;
  }
  Env(base);
  ctx.r3.u64 = it_used;
}
struct Stat {
  ~Stat() {
    std::printf("   [82210970 coverage: iters=%llu flag==0: %llu neg-start: %llu wrap-start: %llu iterator-in-array: %llu | returns: "
                "end-of-array=%llu chain-match=%llu filter-0=%llu | skipped entries: null=%llu mask=%llu chain-miss=%llu]\n",
                (unsigned long long)n_iters, (unsigned long long)n_flag0, (unsigned long long)n_negstart, (unsigned long long)n_wrap,
                (unsigned long long)n_alias_it, (unsigned long long)n_ret_end, (unsigned long long)n_ret_chain,
                (unsigned long long)n_ret_any, (unsigned long long)n_skip_null, (unsigned long long)n_skip_mask,
                (unsigned long long)n_skip_chain);
  }
} stat;
}  // namespace c82210970
fuzz::Registrar reg_82210970({"sub_82210970", c82210970::orig_w, c82210970::nat_w, me::hot::n_82210970::Writes,
                              me::hot::n_82210970::kCmp, 0x40000000, 0x20000, c82210970::gen});
}  // namespace

// ORIG: sub_82533028
// NATIVE: n_82533028.h
// sub_82533028: loop over an index array, float compare against a threshold, swap-remove (see src/native/hot/n_82533028.h).
// Environment: the two float constants (threshold, clear value) are image data outside the window: the generator draws them
// and the wrappers put them into the arena the call runs on.
extern "C" void __imp__sub_82533028(PPCContext&, uint8_t*);
namespace {
namespace c82533028 {
constexpr uint32_t kWin = 0x40000000;
uint32_t g_thr = 0, g_clear = 0;
uint64_t n_calls = 0, n_empty = 0, n_neg = 0, n_removed_total = 0, n_remove_calls = 0, n_none = 0, n_nan_f = 0, n_dup = 0,
         n_clear_alias = 0, n_last = 0, n_arr_alias = 0, n_nanthr = 0, n_big = 0;
void Env(uint8_t* base) {
  me::hot::St32(base, me::hot::n_82533028::kThreshold, g_thr);
  me::hot::St32(base, me::hot::n_82533028::kClear, g_clear);
}
void orig_w(PPCContext& ctx, uint8_t* base) {
  Env(base);
  const uint32_t o = ctx.r3.u32;
  const uint32_t n0 = me::hot::Ld32(base, o + 112);
  __imp__sub_82533028(ctx, base);
  const uint32_t n1 = me::hot::Ld32(base, o + 112);
  if (n1 != n0) {
    ++n_remove_calls;
    n_removed_total += uint32_t(n0 - n1);
  } else if (n0 != 0 && int32_t(n0 - 1u) >= 0) {
    ++n_none;
  }
}
void nat_w(PPCContext& ctx, uint8_t* base) {
  Env(base);
  me::hot::n_82533028::Native(ctx, base);
}
uint32_t flt(fuzz::Rng& r, bool allow_special) {
  if (allow_special && r.chance(0.2)) return r.float_bits();
  float f = float(int32_t(r.u32() % 2001) - 1000) * (1.0f / 8.0f);
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}
void gen(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  ++n_calls;
  const uint32_t o = kWin + 0x100 + r.below(0x40) * 16;
  const uint32_t arr = kWin + 0x2000 + r.below(16) * 2;     // u16 index array
  const uint32_t elems = kWin + 0x6000;                    // element block
  uint32_t n = r.chance(0.9) ? r.range(1, 40) : r.below(100);
  if (r.chance(0.03)) n = 0;
  if (r.chance(0.02)) { n = 0x80000001u + r.below(5); ++n_neg; }       // r7 = n - 1 negative as s32: returns at once
  if (r.chance(0.01)) { n = 0xFFFFFFFFu; ++n_neg; }
  if (n == 0) ++n_empty;
  const uint32_t nvalid = (n == 0 || n > 200) ? 8 : n;                 // entries that exist
  const uint32_t nel = r.range(1, 40);
  int32_t stride = r.chance(0.8) ? int32_t(16 + 4 * r.below(12)) : (r.chance(0.5) ? 0 : -int32_t(16 + 4 * r.below(12)));
  uint32_t ebase = elems + (stride < 0 ? 64 * 48 : 0) + r.below(4) * 4;
  uint32_t off;
  switch (r.below(5)) {
    case 0: off = 0; break;                       // clear block on top of the element itself (clobbers the compared float)
    case 1: off = 4 * r.below(8); break;          // partially overlapping
    case 2: off = kWin + 0xC000 - elems + r.below(0x100) * 4; break;  // separate region
    case 3: off = 0x1000 + 4 * r.below(0x80); break;
    default: off = uint32_t(-int32_t(4 * r.below(8))); break;
  }
  const bool arr_alias = r.chance(0.0);  // (see the note in the header: the multiset assumption of Writes())
  (void)arr_alias;
  me::hot::St32(base, o + 112, n);
  me::hot::St32(base, o + 48, arr);
  me::hot::St32(base, o + 108, uint32_t(stride));
  me::hot::St32(base, o + 44, ebase);
  me::hot::St32(base, o + 100, off);
  g_thr = r.chance(0.04) ? r.float_bits() : flt(r, false);
  g_clear = r.chance(0.5) ? flt(r, true) : (r.chance(0.5) ? 0 : r.float_bits());
  // Signalling-NaN clear values are excluded: the Switch (GCC) build folds the lfs/stfs float->double->float round trip into a
  // raw copy (checked in the t246 ELF disassembly: the 5 stores are `str w6`), the host (clang) original really converts
  // and quiets the NaN, which the raw-copy native version cannot and need not reproduce on the host.
  if ((g_clear & 0x7F800000u) == 0x7F800000u && (g_clear & 0x7FFFFFu)) g_clear |= 0x400000u;
  if ((g_thr & 0x7FFFFFFF) > 0x7F800000) ++n_nanthr;
  // element floats at +12: around the threshold so that roughly half qualify; sprinkled with NaN / inf / denormal
  const float thr_f = [&] { float f; std::memcpy(&f, &g_thr, 4); return f; }();
  for (uint32_t k = 0; k < nel; ++k) {
    const uint32_t e = ebase + uint32_t(int32_t(stride) * int32_t(k));
    uint32_t bits;
    switch (r.below(8)) {
      case 0: bits = r.float_bits(); break;
      case 1: bits = g_thr; break;                                // equal: not greater
      case 2: { float f = thr_f + 1.0f; std::memcpy(&bits, &f, 4); break; }
      case 3: { float f = thr_f - 1.0f; std::memcpy(&bits, &f, 4); break; }
      default: bits = flt(r, false); break;
    }
    if ((bits & 0x7FFFFFFF) > 0x7F800000) ++n_nan_f;
    me::hot::St32(base, e + 12, bits);
  }
  // index array: values < nel (some duplicates)
  for (uint32_t k = 0; k < nvalid + 2; ++k) {
    me::hot::St16(base, arr + 2 * k, uint16_t(r.below(nel)));
  }
  {  // duplicates / the very last entry
    if (nvalid >= 2 && r.chance(0.2)) { me::hot::St16(base, arr, me::hot::Ld16(base, arr + 2)); ++n_dup; }
  }
  // keep every possible clear block inside the window (stride may be 0 / negative: element addresses computed above)
  // (checked against the actual indices below)
  bool ok = true;
  for (uint32_t k = 0; k < nvalid && ok; ++k) {
    const uint32_t idx = me::hot::Ld16(base, arr + 2 * k);
    const uint32_t e = uint32_t(int32_t(stride) * int32_t(idx)) + ebase;
    const uint32_t p = e + off;
    if (!(e >= kWin + 0x100 && e + 16 < kWin + 0x1E000 && p >= kWin + 0x100 && p + 20 < kWin + 0x1E000)) ok = false;
  }
  if (!ok) {  // fall back to a harmless layout
    off = kWin + 0xC000 - elems;
    stride = 32;
    ebase = elems;
    me::hot::St32(base, o + 100, off);
    me::hot::St32(base, o + 108, uint32_t(stride));
    me::hot::St32(base, o + 44, ebase);
    for (uint32_t k = 0; k < nel; ++k) me::hot::St32(base, ebase + 32 * k + 12, flt(r, false));
  } else if (off < 64 || off > uint32_t(-64)) {
    ++n_clear_alias;
  }
  ctx.r3.u64 = o;
}
struct Stat {
  ~Stat() {
    std::printf("   [82533028 coverage: calls=%llu n==0: %llu n huge/negative: %llu | calls with removals=%llu (total removed %llu), "
                "calls without any removal=%llu | NaN element floats=%llu NaN thresholds=%llu duplicate indices=%llu "
                "clear-block-overlaps-element=%llu]\n",
                (unsigned long long)n_calls, (unsigned long long)n_empty, (unsigned long long)n_neg,
                (unsigned long long)n_remove_calls, (unsigned long long)n_removed_total, (unsigned long long)n_none,
                (unsigned long long)n_nan_f, (unsigned long long)n_nanthr, (unsigned long long)n_dup,
                (unsigned long long)n_clear_alias);
  }
} stat;
}  // namespace c82533028
fuzz::Registrar reg_82533028({"sub_82533028", c82533028::orig_w, c82533028::nat_w, me::hot::n_82533028::Writes,
                              me::hot::n_82533028::kCmp, 0x40000000, 0x20000, c82533028::gen});
}  // namespace

// ORIG: sub_82654030 sub_82AC4A50
// NATIVE: n_82654030.h
extern "C" void __imp__sub_82654030(PPCContext&, uint8_t*);
namespace {
// Element = {u32 payload (unique index), float key}; sorted by key, descending. Covers: n < 2 (no FPU mode touch), selection
// sort (n <= 8), the partition path with both "left < right" and "left >= right" (random, ascending, descending, all-equal,
// few-distinct and NaN/denormal/inf key patterns), pushes of one or two ranges, deep stacks (sorted runs), misaligned arrays.
void gen_82654030(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  const uint32_t win = 0x40000000;
  int32_t n;
  switch (r.below(10)) {
    case 0: n = int32_t(r.below(4)) - 1; break;                 // -1, 0, 1, 2
    case 1: case 2: case 3: n = int32_t(r.range(2, 9)); break;  // selection sort range / the n == 9 boundary
    case 4: case 5: n = int32_t(r.range(2, 40)); break;
    case 6: case 7: n = int32_t(r.range(9, 200)); break;
    case 8: n = int32_t(r.range(100, 1500)); break;
    default: n = r.chance(0.05) ? -int32_t(r.below(1000)) : int32_t(r.range(8, 3000)); break;
  }
  const uint32_t arr = win + 0x1000 + r.below(0x400) + (r.chance(0.8) ? 0 : r.below(8));  // (almost) any alignment
  const uint32_t cnt = n > 0 ? uint32_t(n) : 0;
  const int mode = int(r.below(8));
  uint32_t distinct[6];
  for (auto& d : distinct) d = r.float_bits();
  const float step = r.chance(0.5) ? 1.0f : 0.25f;
  const bool dir = r.chance(0.5);
  for (uint32_t i = 0; i < cnt; ++i) {
    uint32_t key;
    switch (mode) {
      case 0: key = r.float_bits(); break;                                  // random incl. NaN / denormal / inf
      case 1: { float f = (dir ? float(i) : float(cnt - i)) * step; std::memcpy(&key, &f, 4); break; }  // ascending / descending
      case 2: key = distinct[0]; break;                                     // all equal
      case 3: key = distinct[r.below(3)]; break;                            // few distinct
      case 4: { float f = float(int32_t(r.below(20)) - 10) * 0.5f; std::memcpy(&key, &f, 4); break; }   // many duplicates
      case 5: key = r.chance(0.3) ? (r.chance(0.5) ? 0x7FC00000u | r.below(0x400000) : 0xFFC00000u) : r.float_bits(); break;  // NaN heavy
      case 6: key = r.chance(0.5) ? r.below(1u << 23) : (0x80000000u | r.below(1u << 23)); break;  // denormals / +-0
      default: { float f = float(int32_t(r.below(2001)) - 1000); std::memcpy(&key, &f, 4); break; }
    }
    const uint32_t a = arr + 8 * i;
    uint32_t pay = __builtin_bswap32(0xC0DE0000u + i), k = __builtin_bswap32(key);
    std::memcpy(base + a, &pay, 4);
    std::memcpy(base + a + 4, &k, 4);
  }
  ctx.r3.u64 = (r.chance(0.2) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0) | arr;
  ctx.r4.u64 = (r.chance(0.2) ? (r.u64() & 0xFFFFFFFF00000000ull) : 0) | uint32_t(n);
}
fuzz::Registrar reg_82654030({"sub_82654030", __imp__sub_82654030, me::hot::n_82654030::Native, me::hot::n_82654030::Writes,
                              me::hot::n_82654030::kCmp, 0x40000000, 0x20000, gen_82654030});
}  // namespace

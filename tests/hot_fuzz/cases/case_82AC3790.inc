// ORIG: sub_82AC3790
// NATIVE: n_82AC3790.h
// wcsicmp: the null-pointer paths are declined (the original runs); its callees are stubbed here (errno word outside the
// compared window), they are identical for the original and the declined native call.
extern "C" void __imp__sub_82AC3790(PPCContext&, uint8_t*);
extern "C" void __imp__sub_82ACBDA8(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0x50000010; }
extern "C" void __imp__sub_82ACBC70(PPCContext&, uint8_t*) {}
// the current generated code calls hooked-looking callees as sub_X (tools/direct_calls.py): forward
extern "C" void sub_82ACBDA8(PPCContext& c, uint8_t* b) { __imp__sub_82ACBDA8(c, b); }
extern "C" void sub_82ACBC70(PPCContext& c, uint8_t* b) { __imp__sub_82ACBC70(c, b); }
namespace {
inline uint16_t wi_unit(fuzz::Rng& r) {
  static const uint16_t kPat[] = {0x0001, 0x0100, 0x00FF, 0xFF00, 0x8000, 0x0080, 0x0101, 0xFFFF, 0x7FFF, 0x0002, 0x0200, 0x8001,
                                  0x0180, 0x0040, 0x0041, 0x005A, 0x005B, 0x0060, 0x0061, 0x007A, 0x007B, 0x0141, 0xFF41, 0x00C0,
                                  0x0140, 0x015A, 0x2041};
  uint16_t v;
  switch (r.below(5)) {
    case 0: v = kPat[r.below(sizeof kPat / sizeof kPat[0])]; break;
    case 1: case 2: v = uint16_t(0x41 + r.below(26) + (r.chance(0.5) ? 32 : 0)); break;
    case 3: v = uint16_t(0x20 + r.below(0x5F)); break;
    default: v = uint16_t(r.u32()); break;
  }
  return v ? v : uint16_t(0x0100);
}
inline void wi_put(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
inline uint16_t wi_get(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t wi_addr(fuzz::Rng& r, uint32_t lo, uint32_t span) {
  uint32_t p = lo + r.below(span);
  switch (r.below(6)) {
    case 0: p |= 1; break;
    case 1: p = (p & ~7u) + r.below(4) * 2; break;
    case 2: p = (p & ~0xFFFu) + 0x1000 - 2 * r.range(0, 24); break;
    case 3: p &= ~7u; break;
    default: p &= ~1u; break;
  }
  return p;
}
inline uint16_t wi_swapcase(uint16_t v) {
  if (v >= 'A' && v <= 'Z') return v + 32;
  if (v >= 'a' && v <= 'z') return v - 32;
  return v;
}
void gen_82AC3790(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  const uint32_t win = 0x40000000;
  uint32_t len;
  switch (r.below(6)) {
    case 0: len = r.below(4); break;
    case 1: len = r.below(12); break;
    case 2: len = r.chance(0.2) ? r.below(3000) : r.below(200); break;
    default: len = r.below(40); break;
  }
  const uint32_t pa = wi_addr(r, win + 0x1000, 0x3000);
  const uint32_t pb = r.chance(0.05) ? pa : wi_addr(r, win + 0x8000, 0x3000);
  for (uint32_t i = 0; i < len; ++i) wi_put(base + pa + 2 * i, wi_unit(r));
  wi_put(base + pa + 2 * len, 0);
  if (pb != pa) {
    uint32_t lb = len;
    if (r.chance(0.08)) {  // unrelated
      lb = r.below(20);
      for (uint32_t i = 0; i < lb; ++i) wi_put(base + pb + 2 * i, wi_unit(r));
    } else {
      const double pc = r.chance(0.5) ? 0.5 : 0.05;
      for (uint32_t i = 0; i < len; ++i) {
        uint16_t v = wi_get(base + pa + 2 * i);
        if (r.chance(pc)) v = wi_swapcase(v);
        wi_put(base + pb + 2 * i, v);
      }
      switch (r.below(8)) {
        case 0: case 1: break;  // equal up to case
        case 2: case 3: case 4:
          if (len) {
            const uint32_t k = r.below(len);
            uint16_t v = wi_get(base + pb + 2 * k);
            v = r.chance(0.3) ? uint16_t(v ^ (1u << r.below(16))) : r.chance(0.5) ? uint16_t(v + (r.chance(0.5) ? 1 : -1)) : wi_unit(r);
            wi_put(base + pb + 2 * k, v);
            if (r.chance(0.1)) wi_put(base + pb + 2 * k, 0);
          }
          break;
        case 5:
          if (len) lb = r.below(len);
          break;
        case 6: {
          const uint32_t extra = r.range(1, 6);
          for (uint32_t i = 0; i < extra; ++i) wi_put(base + pb + 2 * (lb++), wi_unit(r));
          break;
        }
        default:
          if (len) wi_put(base + pb + 2 * (len - 1), uint16_t(wi_get(base + pb + 2 * (len - 1)) ^ 0x20));
          break;
      }
    }
    wi_put(base + pb + 2 * lb, 0);
  }
  ctx.r3.u64 = pa | (r.chance(0.3) ? (r.u64() << 32) : 0);
  ctx.r4.u64 = pb | (r.chance(0.3) ? (r.u64() << 32) : 0);
  if (r.chance(0.5)) std::swap(ctx.r3.u64, ctx.r4.u64);
  if (r.chance(0.03)) ctx.r3.u64 = r.chance(0.5) ? 0 : (r.u64() & 0xFFFFFFFF00000000ull);  // null pointer: declined
  if (r.chance(0.03)) ctx.r4.u64 = r.chance(0.5) ? 0 : (r.u64() & 0xFFFFFFFF00000000ull);
}
fuzz::Registrar reg_82AC3790({"sub_82AC3790", __imp__sub_82AC3790,
                              fuzz::NativeAdapter<me::hot::n_82AC3790::Native, __imp__sub_82AC3790>,
                              me::hot::n_82AC3790::Writes, me::hot::n_82AC3790::kCmp, 0x40000000, 0x20000, gen_82AC3790});
}  // namespace

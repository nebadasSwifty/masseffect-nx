// ORIG: sub_82AC4AF0
// NATIVE: n_82AC4AF0.h
// memcpy: the native version may decline (overlap with dst > src, wrap-around); declined calls run the original.
extern "C" void __imp__sub_82AC4AF0(PPCContext&, uint8_t*);
namespace {
void gen_82AC4AF0(fuzz::Rng& r, uint8_t* base, PPCContext& ctx) {
  const uint32_t win = 0x40000000;
  static const uint32_t kSizes[] = {0,  1,  2,  3,  4,  5,   6,   7,   8,   9,   10,  12,  15,  16,  17,  24,  31,  32,  33,  48,
                                    63, 64, 65, 100, 127, 128, 129, 130, 191, 192, 255, 256, 257, 300, 383, 384, 511, 512, 1000, 1024};
  uint32_t n;
  switch (r.below(10)) {
    case 0: n = r.below(130); break;
    case 1: n = r.below(1500); break;
    case 2: n = r.chance(0.1) ? r.below(7000) : r.below(300); break;
    default: n = kSizes[r.below(sizeof kSizes / sizeof kSizes[0])]; break;
  }
  // the copy ranges live in [win + 0x1000, win + 0x1B000)
  auto off = [&]() -> uint32_t {
    uint32_t mis = r.chance(0.5) ? r.below(8) : r.below(128);
    if (r.chance(0.2)) mis = (r.chance(0.5) ? 0 : 4) + r.below(4) * 8;  // 0 / 4 modulo 8 (word / dword paths)
    return mis;
  };
  uint32_t dst, src;
  const uint32_t slack = 8192;
  const uint32_t lo = win + 0x1000, span = 0x19000 - n - slack;
  switch (r.below(12)) {
    case 0: case 1: case 2: {  // independent, src below dst, no overlap
      src = lo + r.below(span / 2) / 128 * 128 + off();
      dst = lo + span / 2 + r.below(span / 2 - 256) / 128 * 128 + off();
      break;
    }
    case 3: case 4: {  // independent, dst below src
      dst = lo + r.below(span / 2) / 128 * 128 + off();
      src = lo + span / 2 + r.below(span / 2 - 256) / 128 * 128 + off();
      break;
    }
    case 5: case 6: {  // overlap, dst < src (forward copy == memmove)
      src = lo + 0x400 + r.below(span / 2) / 128 * 128 + off();
      const uint32_t k = r.chance(0.5) ? r.range(1, 17) : r.range(1, n + 8);
      dst = src - std::min<uint32_t>(k, 0x300);
      break;
    }
    case 7: case 8: {  // overlap, dst > src (the native version declines)
      src = lo + r.below(span / 2) / 128 * 128 + off();
      const uint32_t k = r.chance(0.5) ? r.range(1, 17) : r.range(1, n + 8);
      dst = src + k;
      break;
    }
    case 9: {  // adjacent, no overlap
      src = lo + 0x400 + r.below(span / 2) / 128 * 128 + off();
      const uint32_t k = r.below(5);
      if (r.chance(0.5)) dst = src + n + k; else dst = src - n - k;
      break;
    }
    case 10: {  // identical ranges
      src = lo + r.below(span / 2) / 128 * 128 + off();
      dst = src;
      break;
    }
    default: {  // everything random (any alignment relation)
      src = lo + r.below(span);
      dst = lo + r.below(span);
      break;
    }
  }
  if (r.chance(0.01)) {  // wrap-around at 4 GB: declined for n > 64 (outside the compared window)
    dst = 0xFFFFFFC0u + r.below(0x30);
    n = 65 + r.below(100);
    if (r.chance(0.5)) std::swap(dst, src);
  }
  ctx.r3.u64 = dst | (r.chance(0.3) ? (r.u64() << 32) : 0);
  ctx.r4.u64 = src | (r.chance(0.3) ? (r.u64() << 32) : 0);
  ctx.r5.u64 = n | (r.chance(0.3) ? (r.u64() << 32) : 0);
}
fuzz::Registrar reg_82AC4AF0({"sub_82AC4AF0", __imp__sub_82AC4AF0,
                              fuzz::NativeAdapter<me::hot::n_82AC4AF0::Native, __imp__sub_82AC4AF0>,
                              me::hot::n_82AC4AF0::Writes, me::hot::n_82AC4AF0::kCmp, 0x40000000, 0x20000, gen_82AC4AF0});
}  // namespace

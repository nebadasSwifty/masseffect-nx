// Host test of the 64-byte vertex copy (CopyBlocks64 in masseffect_native_draws.cpp, masseffect_native_fast_copy)
// against the scalar definition of Xenos' GpuSwap per 32-bit word, for the four endian modes, many sizes and unaligned
// source/destination offsets. The NEON kernels below are copied from the renderer (same intrinsics).
//   clang++ -std=c++20 -O2 tests/cpu/test_native_copy_vertices.cpp -o /tmp/test_native_copy_vertices && /tmp/test_native_copy_vertices
#include <arm_neon.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

template <int kOrder>
inline uint8x16_t PermuteBytes(uint8x16_t v) {
  if constexpr (kOrder == 1) return vrev16q_u8(v);
  else if constexpr (kOrder == 2) return vrev32q_u8(v);
  else if constexpr (kOrder == 3) return vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(v)));
  else return v;
}
template <int kOrder>
void CopyBlocks64(const uint8_t* source, uint8_t* target, size_t blocks) {
  for (size_t b = 0; b < blocks; ++b) {
    __builtin_prefetch(source + 640);
    const uint8x16_t a = vld1q_u8(source), c = vld1q_u8(source + 16), d = vld1q_u8(source + 32), e = vld1q_u8(source + 48);
    vst1q_u8(target, PermuteBytes<kOrder>(a));
    vst1q_u8(target + 16, PermuteBytes<kOrder>(c));
    vst1q_u8(target + 32, PermuteBytes<kOrder>(d));
    vst1q_u8(target + 48, PermuteBytes<kOrder>(e));
    source += 64;
    target += 64;
  }
}

// xenos::GpuSwap(uint32_t, Endian) written out
static uint32_t GpuSwap(uint32_t value, int endian) {
  switch (endian) {
    default:
    case 0: return value;
    case 1: return ((value << 8) & 0xFF00FF00u) | ((value >> 8) & 0x00FF00FFu);
    case 2: return __builtin_bswap32(value);
    case 3: return ((value >> 16) & 0xFFFFu) | (value << 16);
  }
}

int main() {
  std::mt19937_64 rng(3);
  int failures = 0;
  for (int endian = 0; endian < 4; ++endian) {
    for (int trial = 0; trial < 20000; ++trial) {
      const size_t blocks = 1 + rng() % 70;
      const size_t words = blocks * 16;
      const size_t skew_s = rng() % 16, skew_d = rng() % 16;
      std::vector<uint8_t> src(words * 4 + 16 + 640), dst(words * 4 + 16, 0xEE), expected(words * 4 + 16, 0xEE);
      for (auto& b : src) b = uint8_t(rng());
      for (size_t i = 0; i < words; ++i) {
        uint32_t v;
        std::memcpy(&v, src.data() + skew_s + i * 4, 4);
        v = GpuSwap(v, endian);
        std::memcpy(expected.data() + skew_d + i * 4, &v, 4);
      }
      switch (endian) {
        case 1: CopyBlocks64<1>(src.data() + skew_s, dst.data() + skew_d, blocks); break;
        case 2: CopyBlocks64<2>(src.data() + skew_s, dst.data() + skew_d, blocks); break;
        case 3: CopyBlocks64<3>(src.data() + skew_s, dst.data() + skew_d, blocks); break;
        default: CopyBlocks64<0>(src.data() + skew_s, dst.data() + skew_d, blocks); break;
      }
      if (dst != expected) {
        std::printf("FAIL endian %d blocks %zu skew %zu/%zu\n", endian, blocks, skew_s, skew_d);
        if (++failures > 10) return 1;
      }
    }
  }
  std::printf("copy kernels: %d failures\n", failures);
  return failures ? 1 : 0;
}

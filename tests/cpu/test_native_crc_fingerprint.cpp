// Host test + benchmark of app/src/native/masseffect/masseffect_crc_fingerprint.h (CRC32C+CRC32 pair fingerprint): single-bit flips,
// lane-confined changes, 1-ulp float changes, 20M-input birthday checks, speed against seeded XXH3.
//   clang++ -std=c++20 -O2 -march=armv8-a+crc -I app/src/native/masseffect -I <xxhash dir> tests/cpu/test_native_crc_fingerprint.cpp -o /tmp/t
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>
#include <unordered_set>
#include <cmath>
#define XXH_INLINE_ALL
#include <xxhash.h>
#include "masseffect_crc_fingerprint.h"
using namespace masseffect::native::crc_fingerprint;
int main() {
  static_assert(kAvailable);
  std::mt19937_64 rng(42);
  // 1. single-bit flips for sizes 1..: always detected
  uint64_t checked = 0, bad = 0;
  for (size_t n : {1,2,3,4,5,7,8,9,15,16,17,24,31,32,33,63,64,100,255,256,1000,4096,4097,16384}) {
    std::vector<uint8_t> v(n); for (auto& b : v) b = uint8_t(rng());
    const uint64_t h0 = Fingerprint64(v.data(), n, 0);
    for (size_t bit = 0; bit < n * 8; ++bit) {
      v[bit >> 3] ^= 1 << (bit & 7);
      ++checked; if (Fingerprint64(v.data(), n, 0) == h0) ++bad;
      v[bit >> 3] ^= 1 << (bit & 7);
    }
  }
  std::printf("single-bit flips: %llu checked, %llu collisions\n", (unsigned long long)checked, (unsigned long long)bad);
  // 2. random changes confined to lane 0 words
  checked = bad = 0;
  for (int t = 0; t < 2000000; ++t) {
    size_t n = 16 + (rng() % 64) * 16;
    std::vector<uint8_t> v(n); for (auto& b : v) b = uint8_t(rng());
    const uint64_t h0 = Fingerprint64(v.data(), n, 0);
    auto w = v;
    int k = 1 + rng() % 4;
    for (int i = 0; i < k; ++i) { size_t word = (rng() % (n / 16)) * 2; uint64_t x = rng() | 1; for (int j = 0; j < 8; ++j) w[word*8+j] ^= uint8_t(x >> (8*j)); }
    ++checked; if (Fingerprint64(w.data(), n, 0) == h0) ++bad;
  }
  std::printf("lane-0-only random changes: %llu checked, %llu collisions\n", (unsigned long long)checked, (unsigned long long)bad);
  // 3. float data with 1-ulp changes
  checked = bad = 0;
  for (int t = 0; t < 1000000; ++t) {
    size_t n = 256;
    std::vector<float> f(n/4); for (auto& x : f) x = float(rng() % 1000) / 17.0f;
    const uint64_t h0 = Fingerprint64((const uint8_t*)f.data(), n, 123);
    size_t i = rng() % f.size(); f[i] = std::nextafter(f[i], 1e9f);
    ++checked; if (Fingerprint64((const uint8_t*)f.data(), n, 123) == h0) ++bad;
  }
  std::printf("1-ulp float changes: %llu checked, %llu collisions\n", (unsigned long long)checked, (unsigned long long)bad);
  // 4. birthday on random inputs
  { std::unordered_set<uint64_t> s; s.reserve(1<<25); uint64_t dup = 0; uint8_t b[64];
    for (int i = 0; i < 20000000; ++i) { for (int j = 0; j < 8; ++j) { uint64_t x = rng(); memcpy(b + j*8, &x, 8);} if (!s.insert(Fingerprint64(b, 64, 0)).second) ++dup; }
    std::printf("20M random 64B inputs: %llu duplicate hashes\n", (unsigned long long)dup); }
  // 5. structured counters
  { std::unordered_set<uint64_t> s; s.reserve(1<<25); uint64_t dup = 0; uint32_t b[4] = {};
    for (uint32_t i = 0; i < 20000000; ++i) { b[0] = i; b[2] = i * 3; if (!s.insert(Fingerprint64((uint8_t*)b, 16, 0)).second) ++dup; }
    std::printf("20M counter inputs: %llu duplicates\n", (unsigned long long)dup); }
  // 6. speed
  for (size_t n : {512, 2048, 4096, 16384}) {
    std::vector<uint8_t> buf(1 << 20); for (auto& b : buf) b = uint8_t(rng());
    const int iters = 200000;
    uint64_t sink = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) sink += Fingerprint64(buf.data() + ((i * 4096ull) & ((1 << 20) - 16384)), n, sink);
    auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) sink += XXH3_64bits_withSeed(buf.data() + ((i * 4096ull) & ((1 << 20) - 16384)), n, sink);
    auto t2 = std::chrono::steady_clock::now();
    auto ns = [](auto a, auto b) { return std::chrono::duration<double, std::nano>(b - a).count(); };
    std::printf("%5zu B: crc %.1f ns (%.2f GB/s)  xxh3 seeded %.1f ns (%.2f GB/s) sink %llx\n", n, ns(t0, t1) / iters, n / (ns(t0, t1) / iters), ns(t1, t2) / iters, n / (ns(t1, t2) / iters), (unsigned long long)sink);
  }
}

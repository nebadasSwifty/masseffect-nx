// Host test of app/src/native/me_texture_coherency.h (masseffect_native_texture_coherency): a range is reported
// clean after a stamp only if no mark touched it since; every mark that overlaps a range makes it dirty (including
// ranges that touch the 4 KB slack, ranges past the end of memory and huge sizes); size 0 marks nothing; nothing is
// recorded while disabled; concurrent marks never leave a page with an older stamp than the newest mark.
//   clang++ -std=c++20 -O2 -I app/src/native tests/cpu/test_native_texture_coherency.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

#include "me_texture_coherency.h"

namespace tc = me::native::texture_coherency;

static int g_failures = 0;
#define CHECK(c)                                       \
  do {                                                 \
    if (!(c)) {                                        \
      std::printf("FAIL line %d: %s\n", __LINE__, #c); \
      ++g_failures;                                    \
    }                                                  \
  } while (0)

static bool Overlap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn) { return a < b + bn && b < a + an; }

int main() {
  // Disabled: nothing recorded.
  CHECK(tc::Mark(0x100000, 0x1000, tc::kSourceBaseWrite) == 0);
  CHECK(tc::Newest(0x100000, 0x1000) == 0);
  tc::Enable(true);
  // Size 0: nothing.
  CHECK(tc::Mark(0x100000, 0, tc::kSourceBaseWrite) == 0);
  CHECK(tc::Clean(0x100000, 0x1000, 0));
  // Simple mark and clean after it.
  uint32_t s = tc::Current();
  CHECK(tc::Clean(0x200000, 0x10000, s));
  const uint32_t m = tc::Mark(0x208000, 0x1000, tc::kSourceBaseWrite);
  CHECK(m > s);
  CHECK(!tc::Clean(0x200000, 0x10000, s));
  CHECK(tc::Clean(0x200000, 0x10000, tc::Current()));
  // Slack: a mark 4 KB before a page boundary dirties the next page too.
  s = tc::Current();
  tc::Mark(0x30B000, 0x1000, tc::kSourceWait);  // ends at 0x30C000 = page boundary; slack reaches 0x30D000
  CHECK(!tc::Clean(0x30C000, 0x4000, s));
  // Virtual addresses are masked to physical.
  s = tc::Current();
  tc::Mark(0xA0400000u, 0x1000, tc::kSourceBaseWrite);
  CHECK(!tc::Clean(0x400000, 0x1000, s));
  // Past the end wraps to the start; a huge size marks everything.
  s = tc::Current();
  tc::Mark(0x1FFFF000u, 0x3000, tc::kSourceBaseWrite);
  CHECK(!tc::Clean(0x1FFFC000u, 0x4000, s));
  CHECK(!tc::Clean(0, 0x1000, s));
  s = tc::Current();
  tc::Mark(0, 0xFFFFFFFFu, tc::kSourceMmio);
  CHECK(!tc::Clean(0x12345000, 0x1000, s));
  CHECK(!tc::Clean(0x1FFFC000u, 0x4000, s));

  // Random model: textures (ranges with a stamp) against random declared writes. A range is clean after its stamp
  // iff (allowing page granularity and slack) no write overlapped it; the property tested is the safe direction:
  // an overlapping write always makes it dirty.
  std::mt19937_64 rng(12345);
  struct Tex {
    uint64_t start, bytes;
    uint32_t stamp;
    bool written;
  };
  std::vector<Tex> textures(2000);
  for (auto& t : textures) {
    t.start = (rng() % (0x20000000ull - 0x400000)) & ~0xFFFull;
    t.bytes = 0x1000 + (rng() % 0x200000);
    t.stamp = tc::Current();
    t.written = false;
  }
  uint64_t dirty_checked = 0, false_dirty = 0;
  for (int round = 0; round < 20000; ++round) {
    const uint32_t a = uint32_t(rng() % 0x20000000ull) & ~0xFFFu;
    const uint32_t n = uint32_t(rng() % 0x40000) + 1;
    tc::Mark(a, n, tc::kSourceBaseWrite);
    for (int k = 0; k < 4; ++k) {
      Tex& t = textures[rng() % textures.size()];
      if (Overlap(t.start, t.bytes, a, n)) t.written = true;
    }
    if (round % 50 == 0) {
      for (auto& t : textures) {
        // Recompute "written" exactly is expensive; the per-round sampling above marks only some. Check the
        // strong direction on the sampled ones: written implies dirty.
        if (t.written) {
          ++dirty_checked;
          CHECK(!tc::Clean(t.start, t.bytes, t.stamp));
        } else if (!tc::Clean(t.start, t.bytes, t.stamp)) {
          ++false_dirty;  // overlapped by a write not sampled, or only by page granularity: allowed
        }
        // Re-hash: new stamp.
        if (rng() % 4 == 0) {
          t.stamp = tc::Current();
          t.written = false;
        }
      }
    }
  }
  CHECK(dirty_checked > 0);

  // Exhaustive overlap check for a single write against many ranges.
  for (int round = 0; round < 2000; ++round) {
    const uint64_t start = (rng() % 0x1FF00000ull);
    const uint64_t bytes = 1 + rng() % 0x80000;
    const uint32_t stamp = tc::Current();
    const uint32_t a = uint32_t(rng() % 0x1FF00000ull);
    const uint32_t n = uint32_t(1 + rng() % 0x80000);
    tc::Mark(a, n, tc::kSourceReadback);
    if (Overlap(start, bytes, a, n)) CHECK(!tc::Clean(start, bytes, stamp));
    // Far apart (more than a page plus the slack on each side): clean.
    if (a >= start + bytes + 0x8000 || start >= uint64_t(a) + n + 0x8000) CHECK(tc::Clean(start, bytes, stamp));
  }

  // Concurrency: 4 threads mark the same pages; afterwards every page holds the newest stamp handed out to it.
  {
    const uint32_t before = tc::Current();
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
      threads.emplace_back([] {
        for (int i = 0; i < 20000; ++i) tc::Mark(0x08000000u, 0x10000, tc::kSourceMmio);
      });
    }
    for (auto& t : threads) t.join();
    const uint32_t newest = tc::Current();
    CHECK(newest == before + 80000);
    CHECK(tc::Newest(0x08000000u, 0x10000) == newest);
  }

  std::printf("%s (%llu dirty ranges checked, %llu dirty by granularity or unsampled writes)\n",
              g_failures ? "FAILED" : "OK", (unsigned long long)dirty_checked, (unsigned long long)false_dirty);
  return g_failures ? 1 : 0;
}

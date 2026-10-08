// Host test of app/src/native/me_constants_dirty.h (masseffect_native_constants_dirty): a random sequence of constant
// writes (changed values, rewritten equal values, values that go back to an earlier content), draws that read random
// prefixes of the bank, upload buffer resets and desync events. The tracker's decision must be exactly the full
// compare's (the bank prefix equals the last upload or not) and the bound upload must always hold the registers.
//   clang++ -std=c++20 -O2 -I app/src/native tests/cpu/test_native_constants_dirty.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "me_constants_dirty.h"

using namespace me::native;

static int g_failures = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL line %d: %s\n", __LINE__, #c);         \
      if (++g_failures > 20) return 1;                         \
    }                                                          \
  } while (0)

int main() {
  std::mt19937_64 rng(11);
  std::vector<uint32_t> regs(kConstantBankWords, 0), shadow(kConstantBankWords, 0);
  ConstantDirtyBits dirty;
  ConstantBankTracker tracker;
  uint64_t generation = 0;
  // the "upload buffer": blocks of the current epoch
  std::vector<std::vector<uint32_t>> blocks;
  int bound = -1;
  uint32_t bound_bytes = 0;
  uint64_t reuses = 0, uploads = 0, compared_total = 0, shadow_copied = 0, full_equal_reuse_missed = 0;
  const uint32_t stage = uint32_t(rng() % 2);  // the bank under test (VS or PS bits)
  for (int step = 0; step < 2000000; ++step) {
    // writes: a few vectors, sometimes equal values, sometimes back to the shadow (A -> B -> A)
    const uint32_t writes = uint32_t(rng() % 7);
    bool bumped = false;
    for (uint32_t w = 0; w < writes; ++w) {
      const uint32_t word = uint32_t(rng() % (rng() % 4 == 0 ? kConstantBankWords : 64));
      uint32_t value;
      switch (rng() % 4) {
        case 0: value = regs[word]; break;    // rewritten with the same value
        case 1: value = shadow[word]; break;  // back to the uploaded content
        default: value = uint32_t(rng() % 3); break;
      }
      if (regs[word] != value) {
        regs[word] = value;
        dirty.Mark(kConstantRegisterFirst + stage * kConstantBankWords + word);
        bumped = true;
      }
    }
    if (bumped) {  // one generation bump per run, as the ring sink does
      ++generation;
      ++dirty.bumps[stage];
    }
    if (rng() % 500 == 0) {  // new upload buffer
      blocks.clear();
      bound = -1;
    }
    uint32_t bytes = uint32_t(rng() % 4 == 0 ? rng() % 4097 : 16 * (1 + rng() % 24));
    tracker.Take(dirty, stage, generation);
    const bool full_equal = bound >= 0 && bytes <= bound_bytes &&
                            std::memcmp(blocks[bound].data(), regs.data(), bytes) == 0;
    bool reuse = false;
    if (rng() % 3000 == 0) tracker.Desync();  // a patched upload elsewhere
    if (tracker.synced() && bound >= 0 && bytes <= tracker.shadow_bytes()) {
      uint32_t compared = 0;
      reuse = tracker.Same(shadow.data(), regs.data(), bytes, compared);
      compared_total += compared;
      CHECK(reuse == full_equal);
      if (reuse) tracker.Reused(bytes);
    } else if (tracker.synced() && full_equal) {
      ++full_equal_reuse_missed;  // only after an upload buffer reset (bound < 0) or a larger read
    }
    if (reuse) {
      ++reuses;
      bound_bytes = bytes;
    } else {
      blocks.emplace_back(regs.begin(), regs.begin() + (bytes + 3) / 4);
      bound = int(blocks.size() - 1);
      bound_bytes = bytes;
      ++uploads;
      if (tracker.synced()) shadow_copied += tracker.Uploaded(shadow.data(), regs.data(), bytes);
      else tracker.Resync(shadow.data(), regs.data(), bytes);
    }
    // what the draw binds holds the registers
    CHECK(std::memcmp(blocks[bound].data(), regs.data(), bytes) == 0);
    // the shadow equals the bound block over the valid prefix
    CHECK(tracker.shadow_bytes() == bytes);
    CHECK(std::memcmp(shadow.data(), blocks[bound].data(), bytes) == 0);
  }
  // A write path that bumps the generation without bits loses the sync.
  ++generation;
  tracker.Take(dirty, stage, generation);
  CHECK(!tracker.synced());
  std::printf("%llu reuses, %llu uploads, %.2f vectors compared per draw, %.1f shadow bytes copied per upload, "
              "%llu equal blocks not reusable (new buffer or larger read), %d failures\n",
              (unsigned long long)reuses, (unsigned long long)uploads, double(compared_total) / 2000000.0,
              double(shadow_copied) / double(uploads ? uploads : 1), (unsigned long long)full_equal_reuse_missed,
              g_failures);
  return g_failures ? 1 : 0;
}

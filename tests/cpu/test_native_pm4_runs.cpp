// Host test of app/src/native/me_pm4_runs.h: ApplyRunRaw (NEON, from guest big-endian words) against the original
// per-word loop (ApplyRunReference) over random runs, including runs that straddle the VS/PS/fetch boundaries, runs
// that change nothing, runs with a single changed word at every position, and unaligned guest pointers. Also
// ApplyRunRawDirty (masseffect_native_constants_dirty): same result plus exactly the changed constant vectors marked.
//   clang++ -std=c++20 -O2 -I app/src/native tests/cpu/test_native_pm4_runs.cpp -o /tmp/test_native_pm4_runs && /tmp/test_native_pm4_runs
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "me_pm4_runs.h"

using namespace me::native;

static int g_failures = 0;
#define CHECK(c)                                                           \
  do {                                                                     \
    if (!(c)) {                                                            \
      std::printf("FAIL line %d: %s\n", __LINE__, #c);                     \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

int main() {
  std::mt19937_64 rng(7);
  constexpr uint32_t kFirst = 0x4000, kEnd = 0x48C0, kRegs = 0x5003;
  std::vector<uint32_t> a(kRegs), b(kRegs), c(kRegs);
  uint64_t runs = 0, changed_runs = 0;
  for (int iteration = 0; iteration < 3000000; ++iteration) {
    for (uint32_t i = kFirst - 8; i < kEnd + 8; ++i) a[i] = b[i] = c[i] = uint32_t(rng() % 5 == 0 ? rng() : 0x11111111u);
    uint32_t count = 1 + uint32_t(rng() % 40);
    if (iteration % 17 == 0) count = 1 + uint32_t(rng() % 600);
    uint32_t index;
    switch (rng() % 4) {  // straddle the boundaries often
      case 0: index = 0x4400 - uint32_t(rng() % 9); break;
      case 1: index = 0x4800 - uint32_t(rng() % 9); break;
      case 2: index = kFirst + uint32_t(rng() % 9); break;
      default: index = kFirst + uint32_t(rng() % (kEnd - kFirst)); break;
    }
    if (index + count > kEnd) count = kEnd - index;
    if (!count) continue;
    // guest words: mostly equal to the registers (big-endian), some different; at an unaligned byte offset
    std::vector<uint8_t> raw(count * 4 + 8);
    const uint32_t skew = uint32_t(rng() % 4);
    const uint32_t mode = uint32_t(rng() % 4);
    std::vector<uint32_t> guest_host(count);
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t v = a[index + i];
      if (mode == 1 || (mode == 2 && rng() % 8 == 0) || (mode == 3 && i == count / 2)) v = uint32_t(rng());
      guest_host[i] = v;
      const uint32_t be = __builtin_bswap32(v);
      std::memcpy(raw.data() + skew + i * 4, &be, 4);
    }
    // aligned copy for the NEON path (the production caller passes 4-byte aligned guest memory)
    std::vector<uint32_t> aligned(count);
    std::memcpy(aligned.data(), raw.data() + skew, count * 4);
    uint32_t i = 0;
    const RunChange want = ApplyRunReference(&b[index], index, count, [&] { return guest_host[i++]; });
    const RunChange got = ApplyRunRaw(a.data(), index, count, aligned.data());
    // ApplyRunRawDirty: same registers and RunChange, and exactly the vectors with a changed word marked
    uint64_t dirty[8] = {}, expected_dirty[8] = {};
    for (uint32_t k = 0; k < count; ++k) {
      const uint32_t reg = index + k;
      if (reg < 0x4800 && c[reg] != guest_host[k]) {
        const uint32_t v = (reg - 0x4000) >> 2;
        expected_dirty[v >> 6] |= uint64_t(1) << (v & 63);
      }
    }
    const RunChange got_dirty = ApplyRunRawDirty(c.data(), index, count, aligned.data(), dirty);
    CHECK(want == got_dirty);
    CHECK(c == b);
    CHECK(std::memcmp(dirty, expected_dirty, sizeof(dirty)) == 0);
    ++runs;
    changed_runs += want.vs || want.ps || want.fetch;
    CHECK(want == got);
    CHECK(a == b);
    if (g_failures > 20) break;
  }
  // single changed word at every position of a 70-word run crossing 0x4400
  for (uint32_t pos = 0; pos < 70; ++pos) {
    for (uint32_t i = kFirst; i < kEnd; ++i) a[i] = b[i] = 0x22222222u;
    std::vector<uint32_t> guest(70, __builtin_bswap32(0x22222222u));
    guest[pos] = __builtin_bswap32(0x33333333u);
    uint32_t i = 0;
    const RunChange want = ApplyRunReference(&b[0x4400 - 35], 0x4400 - 35, 70, [&] { return __builtin_bswap32(guest[i++]); });
    const RunChange got = ApplyRunRaw(a.data(), 0x4400 - 35, 70, guest.data());
    CHECK(want == got);
    CHECK(a == b);
    CHECK((pos < 35) == got.vs && (pos >= 35) == got.ps && !got.fetch);
  }
  std::printf("%llu runs (%llu with changes), %d failures\n", (unsigned long long)runs, (unsigned long long)changed_runs, g_failures);
  return g_failures ? 1 : 0;
}

// Host test of app/src/native/me_ring_partition.h (masseffect_native_ring_partition): the phases partition the
// ring thread's time (their sum equals the elapsed time between two Take calls), nested scopes give the time back
// to the enclosing phase, Scope(kNone) and scopes on other threads record nothing, and Take resets the totals.
//   clang++ -std=c++20 -O2 -I app/src/native tests/cpu/test_native_ring_partition.cpp -o /tmp/t && /tmp/t
#include <chrono>
#include <cstdio>
#include <thread>

#include "me_ring_partition.h"

namespace rp = me::native::ring_partition;

static int g_failures = 0;
#define CHECK(c)                                       \
  do {                                                 \
    if (!(c)) {                                        \
      std::printf("FAIL line %d: %s\n", __LINE__, #c); \
      ++g_failures;                                    \
    }                                                  \
  } while (0)

static void Busy(std::chrono::microseconds d) {
  const auto end = std::chrono::steady_clock::now() + d;
  while (std::chrono::steady_clock::now() < end) {
  }
}

int main() {
  std::array<uint64_t, rp::kCount> ticks{}, entries{};
  // Off: nothing is recorded.
  rp::BindThisThread(false);
  {
    rp::Scope s(rp::kTextures);
    Busy(std::chrono::microseconds(200));
  }
  CHECK(!rp::Active());
  CHECK(rp::g_state.entries[rp::kTextures] == 0);

  rp::BindThisThread(true);
  rp::Take(ticks, entries);  // start a clean interval
  const uint64_t t0 = rp::Now();
  Busy(std::chrono::microseconds(1000));  // parse
  {
    rp::Scope draw(rp::kDrawVulkan);
    Busy(std::chrono::microseconds(2000));
    {
      rp::Scope textures(rp::kTextures);
      Busy(std::chrono::microseconds(3000));
      rp::Scope none(rp::kNone);  // no effect
      Busy(std::chrono::microseconds(500));
    }
    Busy(std::chrono::microseconds(1000));  // back in the Vulkan draw
  }
  std::thread other([] {  // not the ring thread: no effect
    rp::Scope s(rp::kWait);
    Busy(std::chrono::microseconds(300));
  });
  other.join();
  rp::Take(ticks, entries);
  const uint64_t t1 = rp::Now();
  uint64_t sum = 0;
  for (uint64_t t : ticks) sum += t;
  const double us = 1e6 / rp::TicksPerSecond();
  std::printf("parse %.0f us, Vulkan draw %.0f us, textures %.0f us, wait %.0f us, sum %.0f of %.0f us\n",
              ticks[rp::kParse] * us, ticks[rp::kDrawVulkan] * us, ticks[rp::kTextures] * us, ticks[rp::kWait] * us,
              sum * us, (t1 - t0) * us);
  CHECK(sum <= t1 - t0);
  CHECK(double(sum) >= 0.99 * double(t1 - t0) - 2000.0 / us);  // only the time after Take is outside
  CHECK(ticks[rp::kTextures] * us >= 3400 && ticks[rp::kTextures] * us < 3400 * 1.5);
  CHECK(ticks[rp::kDrawVulkan] * us >= 3000 && ticks[rp::kDrawVulkan] * us < 3000 * 1.5);
  CHECK(ticks[rp::kParse] * us >= 1000);
  CHECK(ticks[rp::kWait] == 0);
  CHECK(entries[rp::kDrawVulkan] == 1 && entries[rp::kTextures] == 1 && entries[rp::kWait] == 0);
  CHECK(rp::g_state.phase == rp::kParse);
  rp::Take(ticks, entries);  // reset: almost nothing since the previous Take
  CHECK(entries[rp::kTextures] == 0 && ticks[rp::kTextures] == 0);
  if (g_failures) return 1;
  std::printf("PASS\n");
  return 0;
}

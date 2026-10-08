// Host test of app/src/native/me_frame_coherence.h (masseffect_native_coherence_stats): identical frames match 100 %
// at the same ordinal and at any position, a reordered frame matches 100 % at any position only, a constant change
// is attributed to the constants component, an unfingerprinted range never matches, frame pairs follow `every`,
// and off records nothing.
//   clang++ -std=c++20 -O2 -I app/src/native -I sdk/thirdparty/xxHash tests/cpu/test_native_frame_coherence.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <vector>

#define XXH_INLINE_ALL
#include "me_frame_coherence.h"

namespace co = me::native::coherence;

static int g_failures = 0;
#define CHECK(c)                                       \
  do {                                                 \
    if (!(c)) {                                        \
      std::printf("FAIL line %d: %s\n", __LINE__, #c); \
      ++g_failures;                                    \
    }                                                  \
  } while (0)

static std::vector<uint32_t> regs(0x5003, 0);
static int vs_a, ps_a;

static void OneDraw(uint32_t i, bool fingerprinted = true) {
  if (!co::Recording()) return;
  co::BeginDraw();
  regs[0x4000] = i;  // a per-draw VS constant (world matrix)
  co::NoteContent(0x1000 + i * 64, 64, fingerprinted ? 0xABC0 + i : 0, nullptr, 0, false);
  co::NoteStage(0, 1000);
  co::Inputs in;
  in.registers = regs.data();
  in.vs = &vs_a;
  in.ps = &ps_a;
  in.vs_constant_words = 16;
  in.ps_constant_words = 8;
  in.generation_constants_vs = 1000 + i;  // changes per draw
  in.initiator = 4 | (36u << 16);
  co::EndDraw(in);
}

static uint64_t Field(const co::Window& w, uint32_t k) { return w.any[k]; }

int main() {
  // Off: nothing recorded.
  co::Configure(false, 1);
  CHECK(!co::Recording());
  std::string a, b;
  CHECK(!co::EndFrame(a, b));

  // Every frame, identical frames.
  co::Configure(true, 1);
  for (int f = 0; f < 3; ++f) {
    for (uint32_t i = 0; i < 100; ++i) OneDraw(i);
    co::EndFrame(a, b);
  }
  CHECK(co::g.w.frames == 2);
  CHECK(co::g.w.draws == 200);
  CHECK(co::g.w.any[co::kFull] == 200 && co::g.w.ordinal[co::kFull] == 200);
  CHECK(co::g.w.distinct[co::kFull] == 200);  // 100 per compared frame
  CHECK(co::g.w.distinct[co::kShaders] == 2);

  // Reversed order: any position 100 %, ordinal only the middle one... none (100 draws, no fixed point).
  co::Configure(true, 1);
  for (uint32_t i = 0; i < 100; ++i) OneDraw(i);
  co::EndFrame(a, b);
  for (uint32_t i = 0; i < 100; ++i) OneDraw(99 - i);
  co::EndFrame(a, b);
  CHECK(co::g.w.any[co::kFull] == 100);
  CHECK(co::g.w.ordinal[co::kFull] == 0);

  // One constant changed in draw 7: constants differ in one draw only, attributed to constants.
  co::Configure(true, 1);
  for (uint32_t i = 0; i < 50; ++i) OneDraw(i);
  co::EndFrame(a, b);
  for (uint32_t i = 0; i < 50; ++i) {
    if (i == 7) {
      co::BeginDraw();
      regs[0x4000] = 12345;
      co::NoteContent(0x1000 + i * 64, 64, 0xABC0 + i, nullptr, 0, false);
      co::Inputs in;
      in.registers = regs.data();
      in.vs = &vs_a;
      in.ps = &ps_a;
      in.vs_constant_words = 16;
      in.ps_constant_words = 8;
      in.generation_constants_vs = 99999;
      in.initiator = 4 | (36u << 16);
      co::EndDraw(in);
    } else {
      OneDraw(i);
    }
  }
  co::EndFrame(a, b);
  CHECK(co::g.w.any[co::kFull] == 49);
  CHECK(co::g.w.only_one[co::kConstants] == 1);
  CHECK(Field(co::g.w, co::kShaders) == 50);

  // Unfingerprinted range: never matches.
  co::Configure(true, 1);
  for (int f = 0; f < 2; ++f) {
    for (uint32_t i = 0; i < 10; ++i) OneDraw(i, i != 3);
    co::EndFrame(a, b);
  }
  CHECK(co::g.w.any[co::kFull] == 9);
  CHECK(co::g.w.unfingerprinted_draws == 1);

  // A range without a fingerprint but with data is hashed by sampling and matches when the bytes are equal.
  {
    std::vector<uint8_t> big(100000, 7);
    co::Configure(true, 1);
    for (int f = 0; f < 3; ++f) {
      co::BeginDraw();
      if (f == 2) big[0] = 9;  // first block changed: no match
      co::NoteContent(0x8000, 100000, 0, big.data(), big.size(), false);
      co::Inputs in;
      in.registers = regs.data();
      co::EndDraw(in);
      co::EndFrame(a, b);
    }
    CHECK(co::g.w.draws == 2);
    CHECK(co::g.w.any[co::kFull] == 1);
    CHECK(co::g.w.sampled_ranges == 2);
    CHECK(co::g.w.unfingerprinted_draws == 0);
  }

  // every = 4: frames 0,1 recorded (1 compared), 2,3 not, 4,5 recorded (5 compared).
  co::Configure(true, 4);
  for (int f = 0; f < 8; ++f) {
    for (uint32_t i = 0; i < 10; ++i) OneDraw(i);
    co::EndFrame(a, b);
  }
  CHECK(co::g.w.frames == 2);
  CHECK(co::g.w.frames_recorded == 4);
  CHECK(co::g.w.frames_all == 8);
  CHECK(co::g.w.any[co::kFull] == 20);

  // Report text builds.
  co::BuildReport(me::native::ring_partition::Now() + 1, a, b);
  CHECK(a.find("frame coherence") != std::string::npos);
  CHECK(b.find("estimate") != std::string::npos);
  std::printf("%s\n%s\n", a.c_str(), b.c_str());

  // Overflow cap.
  co::Configure(true, 1);
  for (uint32_t i = 0; i < co::kMaxDraws + 10; ++i) OneDraw(i);
  CHECK(co::g.w.overflow == 10);

  if (g_failures) {
    std::printf("%d failures\n", g_failures);
    return 1;
  }
  std::printf("frame coherence: all checks passed\n");
  return 0;
}

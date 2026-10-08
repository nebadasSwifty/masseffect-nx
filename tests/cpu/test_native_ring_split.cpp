// Host test of app/src/native/me_ring_split.h (steps 0 and 1 of docs/multithread-translation.md):
//  - Journal + Mirror: a simulated front writes a register file the way the ring does (single words, constant runs,
//    runs that continue the previous one, rewrites of one register, microcode changes of both stages) and the back's
//    mirror, synced at random points, must equal the live file and microcode bit for bit at every sync;
//  - malformed journals are rejected; FirstDifference / CountDifferences / VerifySchedule / EffectStats;
//  - a micro-benchmark of the journal's cost per register word (append + apply), printed for the docs.
//   clang++ -std=c++20 -O2 -I app/src/native tests/cpu/test_native_ring_split.cpp -o /tmp/t && /tmp/t
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

#include "me_ring_split.h"

using namespace me::native::ring_split;

static int g_failures = 0;
#define CHECK(c)                                       \
  do {                                                 \
    if (!(c)) {                                        \
      std::printf("FAIL line %d: %s\n", __LINE__, #c); \
      ++g_failures;                                    \
    }                                                  \
  } while (0)

constexpr uint32_t kRegCount = 0x5003;

static void TestMirror() {
  std::mt19937_64 rng(11);
  std::vector<uint32_t> live(kRegCount);
  for (auto& v : live) v = uint32_t(rng());
  std::vector<uint32_t> ucode[2];
  Journal journal;
  Mirror mirror;
  mirror.Reset(live.data(), live.size());
  uint64_t syncs = 0, words = 0;
  for (int step = 0; step < 400000; ++step) {
    const uint32_t what = uint32_t(rng() % 100);
    if (what < 40) {  // single register (type 0 single, WriteRegister)
      const uint32_t i = uint32_t(rng() % kRegCount), v = uint32_t(rng());
      live[i] = v;
      journal.Reg(i, v);
      ++words;
    } else if (what < 70) {  // constant run (0x4000-0x48BF), written then journaled from the live file
      const uint32_t count = 1 + uint32_t(rng() % (step % 13 == 0 ? 600 : 40));
      uint32_t index = 0x4000 + uint32_t(rng() % 0x8C0);
      if (index + count > 0x48C0) index = 0x48C0 - count;
      for (uint32_t k = 0; k < count; ++k) live[index + k] = rng() % 3 ? uint32_t(rng()) : live[index + k];
      journal.Regs(index, live.data() + index, count);
      words += count;
    } else if (what < 85) {  // a run of single writes of consecutive registers (merged into one record)
      const uint32_t count = 1 + uint32_t(rng() % 12);
      const uint32_t index = uint32_t(rng() % (kRegCount - count));
      for (uint32_t k = 0; k < count; ++k) {
        live[index + k] = uint32_t(rng());
        journal.Reg(index + k, live[index + k]);
      }
      words += count;
    } else if (what < 90) {  // the same register written twice (last value must win)
      const uint32_t i = uint32_t(rng() % kRegCount);
      live[i] = uint32_t(rng());
      journal.Reg(i, live[i]);
      live[i] = uint32_t(rng());
      journal.Reg(i, live[i]);
    } else if (what < 95) {  // a stage's microcode changes (sizes 0..966 words)
      const uint32_t stage = uint32_t(rng() & 1);
      ucode[stage].resize(rng() % 967);
      for (auto& w : ucode[stage]) w = uint32_t(rng());
      journal.Microcode(stage, ucode[stage].data(), uint32_t(ucode[stage].size()));
    } else {  // back operation: sync and compare
      CHECK(mirror.Apply(journal));
      journal.Clear();
      ++syncs;
      CHECK(FirstDifference(mirror.registers(), live.data(), kRegCount) == -1);
      CHECK(mirror.microcode(0) == ucode[0]);
      CHECK(mirror.microcode(1) == ucode[1]);
      if (g_failures) return;
    }
  }
  CHECK(syncs > 1000);
  CHECK(journal.register_words() >= words);
  std::printf("mirror: %llu syncs, %llu records, %llu register words, %llu microcode words\n",
              (unsigned long long)syncs, (unsigned long long)journal.records(),
              (unsigned long long)journal.register_words(), (unsigned long long)journal.microcode_words());
}

static void TestMerge() {
  Journal j;
  j.Reg(0x2000, 1);
  j.Reg(0x2001, 2);
  const uint32_t run[3] = {3, 4, 5};
  j.Regs(0x2002, run, 3);
  CHECK(j.records() == 1);
  CHECK(j.size() == 2 + 5);
  j.Reg(0x2000, 9);  // not a continuation: new record
  CHECK(j.records() == 2);
  j.Microcode(0, run, 3);
  j.Reg(0x2001, 7);  // after a microcode record: new record
  CHECK(j.records() == 4);
  Mirror m;
  std::vector<uint32_t> zero(kRegCount, 0);
  m.Reset(zero.data(), zero.size());
  CHECK(m.Apply(j));
  CHECK(m.Register(0x2000) == 9 && m.Register(0x2001) == 7 && m.Register(0x2004) == 5);
  CHECK(m.microcode(0).size() == 3 && m.microcode(0)[2] == 5);
}

static void TestMalformed() {
  Mirror m;
  std::vector<uint32_t> zero(16, 0);
  m.Reset(zero.data(), zero.size());
  const uint32_t past_end[] = {(kRecordRegs << 28) | 2, 15, 1, 2};  // index 15 + 2 > 16
  CHECK(!m.Apply(past_end, 4));
  const uint32_t truncated[] = {(kRecordRegs << 28) | 3, 0, 1};
  CHECK(!m.Apply(truncated, 3));
  const uint32_t bad_kind[] = {(7u << 28) | 0, 0};
  CHECK(!m.Apply(bad_kind, 2));
  const uint32_t bad_stage[] = {(kRecordMicrocode << 28) | 0, 2};
  CHECK(!m.Apply(bad_stage, 2));
  const uint32_t good[] = {(kRecordRegs << 28) | 1, 15, 42};
  CHECK(m.Apply(good, 3) && m.Register(15) == 42);
}

static void TestHelpers() {
  std::vector<uint32_t> a(kRegCount, 5), b(kRegCount, 5);
  CHECK(FirstDifference(a.data(), b.data(), kRegCount) == -1);
  b[0x4123] = 6;
  b[0x5002] = 7;
  CHECK(FirstDifference(a.data(), b.data(), kRegCount) == 0x4123);
  CHECK(CountDifferences(a.data(), b.data(), kRegCount) == 2);
  b[0] = 1;
  CHECK(FirstDifference(a.data(), b.data(), kRegCount) == 0);
  VerifySchedule s{4, 3};
  int hits = 0;
  for (int i = 0; i < 13; ++i) hits += s.Next();  // 0-3, then 4, 7, 10
  CHECK(hits == 7);
  VerifySchedule never{2, 0};
  hits = 0;
  for (int i = 0; i < 10; ++i) hits += never.Next();
  CHECK(hits == 2);
  EffectStats e;
  e.Note(kFence);  // 0 draws before
  for (int i = 0; i < 5; ++i) e.Draw();
  e.Note(kInterrupt);  // 5 draws -> 2-7
  e.BackWork();
  e.Note(kScratch);  // 0 draws, but back work -> a barrier
  e.Note(kWaitMemory);  // nothing in between
  for (int i = 0; i < 200; ++i) e.Draw();
  e.Note(kReadPointer);
  CHECK(e.Total() == 5);
  CHECK(e.gaps[0] == 3 && e.gaps[2] == 1 && e.gaps[5] == 1);
  CHECK(e.barriers == 3);
  CHECK(e.max_gap == 200);
  for (int i = 0; i < 3; ++i) e.Draw();
  e.ResetInterval();
  CHECK(e.Total() == 0 && e.draws_since == 3);
  CHECK(GapBucket(31) == 3 && GapBucket(32) == 4 && GapBucket(127) == 4 && GapBucket(128) == 5);
}

// Cost of the journal per register word, the shape of a heavy ring frame: ~1200 draws, each with ~100 words in
// a few runs and ~20 single writes, a sync (apply) per draw. Compared with the plain stores alone.
static void Benchmark() {
  std::mt19937 rng(3);
  std::vector<uint32_t> live(kRegCount, 0);
  Journal j;
  Mirror m;
  m.Reset(live.data(), live.size());
  struct Op {
    uint32_t index, count;
  };
  std::vector<Op> ops;
  for (int d = 0; d < 1200; ++d) {
    for (int r = 0; r < 4; ++r) ops.push_back({0x4000 + uint32_t(rng() % 0x800), 4 + uint32_t(rng() % 40)});
    for (int s = 0; s < 20; ++s) ops.push_back({0x2000 + uint32_t(rng() % 0x300), 1});
    ops.push_back({0, 0});  // draw: sync
  }
  std::vector<uint32_t> values(64);
  for (auto& v : values) v = rng();
  const int frames = 200;
  uint64_t words = 0;
  auto t0 = std::chrono::steady_clock::now();
  for (int f = 0; f < frames; ++f)
    for (const Op& op : ops) {
      if (!op.count) continue;
      for (uint32_t k = 0; k < op.count; ++k) live[op.index + k] = values[k] + uint32_t(f);
      words += op.count;
    }
  auto t1 = std::chrono::steady_clock::now();
  for (int f = 0; f < frames; ++f)
    for (const Op& op : ops) {
      if (!op.count) {
        m.Apply(j);
        j.Clear();
        continue;
      }
      for (uint32_t k = 0; k < op.count; ++k) live[op.index + k] = values[k] + uint32_t(f);
      if (op.count == 1) j.Reg(op.index, live[op.index]);
      else j.Regs(op.index, live.data() + op.index, op.count);
    }
  auto t2 = std::chrono::steady_clock::now();
  CHECK(FirstDifference(m.registers(), live.data(), kRegCount) == -1);
  const double plain = std::chrono::duration<double, std::nano>(t1 - t0).count();
  const double journaled = std::chrono::duration<double, std::nano>(t2 - t1).count();
  std::printf("benchmark (host): %llu words, %.2f ns/word plain, %.2f ns/word with journal + apply; "
              "journal overhead %.2f us per 1200-draw frame (%.3f us per draw)\n",
              (unsigned long long)words, plain / double(words), journaled / double(words),
              (journaled - plain) / frames / 1000.0, (journaled - plain) / frames / 1200.0 / 1000.0);
}

// Start-up: the ring has run (registers and microcode already non-zero) before the split begins, and a register is
// written outside the journal (the MMIO write pointer, 0x01C5). Seed + JournalIfChanged must make the mirror equal
// at the very first back operation (the console's first ru_split1 run differed on 0x01C5 at draw 1).
static void TestStartup() {
  std::mt19937_64 rng(5);
  std::vector<uint32_t> live(kRegCount);
  for (auto& v : live) v = uint32_t(rng());
  std::vector<uint32_t> vs(120), ps(48);
  for (auto& w : vs) w = uint32_t(rng());
  for (auto& w : ps) w = uint32_t(rng());
  Mirror m;
  m.Seed(live.data(), live.size(), vs, ps);
  CHECK(FirstDifference(m.registers(), live.data(), kRegCount) == -1);
  CHECK(m.microcode(0) == vs && m.microcode(1) == ps);
  Journal j;
  constexpr uint32_t kWptr = 0x01C5;
  live[kWptr] = 0x25;  // MMIO kick after the seed, never journaled by the front
  live[0x2001] = 7;
  j.Reg(0x2001, 7);
  JournalIfChanged(j, m, live.data(), kWptr);
  const size_t before = j.size();
  JournalIfChanged(j, m, live.data(), 0x2002);  // unchanged: nothing appended
  CHECK(j.size() == before);
  CHECK(m.Apply(j));
  j.Clear();
  CHECK(FirstDifference(m.registers(), live.data(), kRegCount) == -1);
  // Without the write-pointer journal the old behaviour reproduces the console DIFFERENCE.
  Mirror stale;
  std::vector<uint32_t> seeded(live);
  seeded[kWptr] = 0;
  stale.Seed(seeded.data(), seeded.size(), vs, ps);
  CHECK(FirstDifference(stale.registers(), live.data(), kRegCount) == kWptr);
}

int main() {
  TestStartup();
  TestMerge();
  TestMalformed();
  TestHelpers();
  TestMirror();
  Benchmark();
  if (g_failures) {
    std::printf("%d failures\n", g_failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}

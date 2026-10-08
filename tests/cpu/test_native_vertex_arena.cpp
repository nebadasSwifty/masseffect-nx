// Host test of app/src/native/masseffect/me_vertex_arena.h (masseffect_native_vertex_arena*, docs/zero-copy-vertices.md).
//
//  - Measure: clean/dirty x same/changed against a reference model of page stamps; the top clean+CHANGED list.
//  - ExemptPages: marks and queries across page boundaries and the end of memory.
//  - Arena: an arena byte is never handed out again while a frame that read it (stored into it or hit it) may still be
//    on the GPU (model of three work slots whose frames complete only when the slot is reused), a live entry always
//    points at the bytes it was stored with, and recycled segments drop their entries.
//   clang++ -std=c++20 -O2 -I app/src/native/masseffect tests/cpu/test_native_vertex_arena.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <vector>

#include "me_vertex_arena.h"

namespace va = me::native::vertex_arena;

static int g_failures = 0;
#define CHECK(c)                                       \
  do {                                                 \
    if (!(c)) {                                        \
      std::printf("FAIL line %d: %s\n", __LINE__, #c); \
      ++g_failures;                                    \
    }                                                  \
  } while (0)

static void TestMeasure() {
  // Page stamps of a toy coherency table.
  std::vector<uint32_t> page(va::kPages, 0);
  uint32_t seq = 0;
  auto mark = [&](uint64_t start, uint64_t length) {
    ++seq;
    for (uint64_t p = start >> va::kPageShift; p <= (start + length - 1) >> va::kPageShift; ++p) page[p] = seq;
  };
  auto clean = [&](uint64_t start, uint64_t length, uint32_t stamp) {
    for (uint64_t p = start >> va::kPageShift; p <= (start + length - 1) >> va::kPageShift; ++p)
      if (page[p] > stamp) return false;
    return true;
  };
  va::Measure m(4);
  const va::RangeKey a{0x100000, 4096, 32, 2}, b{0x200000, 64, 16, 2};
  CHECK(m.Note(a, 1, seq, clean) == va::Measure::kFirst);
  CHECK(m.Note(a, 1, seq, clean) == va::Measure::kCleanSame);
  CHECK(m.Note(a, 2, seq, clean) == va::Measure::kCleanChanged);
  mark(0x100800, 16);
  CHECK(m.Note(a, 2, seq, clean) == va::Measure::kDirtySame);
  mark(0x0FF000, 0x2000);
  CHECK(m.Note(a, 3, seq, clean) == va::Measure::kDirtyChanged);
  CHECK(m.Note(a, 3, seq, clean) == va::Measure::kCleanSame);
  mark(0x300000, 16);  // elsewhere
  CHECK(m.Note(a, 3, seq, clean) == va::Measure::kCleanSame);
  CHECK(m.Note(b, 7, seq, clean) == va::Measure::kFirst);
  for (int i = 0; i < 3; ++i) CHECK(m.Note(b, 8 + i, seq, clean) == va::Measure::kCleanChanged);
  CHECK(m.counts().n[va::Measure::kCleanChanged] == 4);
  CHECK(m.counts().bytes[va::Measure::kCleanChanged] == 4096 + 3 * 64);
  size_t distinct = 0;
  auto top = m.TakeTop(5, distinct);
  CHECK(distinct == 2 && top.size() == 2);
  CHECK(top[0].key == b && top[0].count == 3);
  CHECK(top[1].key == a && top[1].count == 1);
  top = m.TakeTop(5, distinct);
  CHECK(distinct == 0 && top.empty());
  // Bounded table: the fifth distinct range starts over.
  for (uint32_t i = 0; i < 4; ++i) m.Note(va::RangeKey{0x400000 + i * 0x1000, 64, 16, 2}, 1, seq, clean);
  CHECK(m.counts().table_resets == 1);
  CHECK(m.Note(a, 3, seq, clean) == va::Measure::kFirst);
}

static void TestExempt() {
  va::ExemptPages x;
  CHECK(!x.Any(0, 100));
  CHECK(x.Mark(0x4000 - 8, 16) == 2);  // straddles pages 0 and 1
  CHECK(x.Any(0, 1) && x.Any(0x4000, 1) && !x.Any(0x8000, 0x4000));
  CHECK(x.Any(0x7FFF, 2));  // touches page 1
  CHECK(x.Mark(0x0, 0x8000) == 0);
  CHECK(x.Mark(va::kPhysicalBytes - 4, 64) == 1);  // clamped to the end of memory
  CHECK(x.Any(va::kPhysicalBytes - 1, 1));
  CHECK(x.marked() == 3);
}

// Model: kSlots work slots used round robin; a frame on slot s completes only when slot s starts its next frame (the
// client waited for its fence). Each frame stores and hits ranges of a working set larger than the arena; every
// stored copy remembers the frames that read it (its store and its hits).
static void TestArena(uint32_t seed) {
  std::mt19937 rng(seed);
  constexpr uint32_t kSlots = 3;
  constexpr uint64_t kCapacity = 1 << 20;
  constexpr uint32_t kRanges = 120;
  va::Arena arena;
  arena.Configure(kCapacity, 1 << 10);
  CHECK(arena.usable());
  CHECK(arena.max_bytes() == kCapacity / va::Arena::kSegments);
  struct Copy {
    uint32_t end = 0;
    std::vector<uint64_t> readers;  // frames that read it
  };
  std::map<uint32_t, Copy> copies;  // arena offset -> stored copy
  std::vector<bool> complete;       // per frame
  std::vector<uint64_t> slot_frame(kSlots, UINT64_MAX);
  std::vector<uint32_t> version(kRanges, 1), sizes(kRanges);
  for (uint32_t r = 0; r < kRanges; ++r) sizes[r] = ((r % 5 == 0) ? 12000 + (r * 7919) % 40000 : 256 + (r * 4049) % 7000) & ~3u;
  std::vector<uint32_t> content(kCapacity / 4, 0);  // arena words: (range, version) they were stored with
  uint64_t frame = 0, hits = 0, stores = 0, refused = 0;
  for (uint32_t f = 0; f < 4000; ++f, ++frame) {
    const uint32_t slot = f % kSlots;
    if (slot_frame[slot] != UINT64_MAX) complete[slot_frame[slot]] = true;
    slot_frame[slot] = frame;
    complete.resize(frame + 1, false);
    arena.SlotStarted(slot, frame);
    const uint32_t ops = 5 + rng() % 30;
    for (uint32_t i = 0; i < ops; ++i) {
      const uint32_t r = (rng() % 4 == 0) ? rng() % kRanges : rng() % 16;  // a hot set and a cold tail
      const uint32_t address = r * 0x10000, bytes = sizes[r];
      if (rng() % 23 == 0) ++version[r];  // the guest rewrote it (the client sees it dirty or changed)
      const uint32_t tag = (r << 16) ^ version[r];
      va::RangeKey key{address, bytes, 16, 2};
      va::Arena::Entry& e = arena.Slot(key);
      if (va::Arena::Same(e, key) && arena.Live(e) && e.fingerprint == version[r]) {
        // A hit: the arena bytes must still be the ones stored.
        bool intact = true;
        for (uint32_t w = e.offset / 4; w < (e.offset + bytes) / 4; ++w) intact &= content[w] == tag;
        CHECK(intact);
        arena.Use(e.segment);
        copies[e.offset].readers.push_back(frame);
        ++hits;
        continue;
      }
      uint32_t at = 0, segment = 0, epoch = 0;
      if (!arena.Allocate(bytes, 4, at, segment, epoch)) {
        ++refused;
        continue;
      }
      CHECK(at + bytes <= kCapacity);
      CHECK(at / arena.segment_bytes() == segment && (at + bytes - 1) / arena.segment_bytes() == segment);
      // Never overwrite a copy that a frame still on the GPU (or the current frame) reads.
      for (auto it = copies.begin(); it != copies.end();) {
        if (it->first < at + bytes && at < it->second.end) {
          for (uint64_t reader : it->second.readers) CHECK(complete[reader] && reader != frame);
          it = copies.erase(it);
        } else {
          ++it;
        }
      }
      copies[at] = Copy{at + bytes, {frame}};
      for (uint32_t w = at / 4; w < (at + bytes) / 4; ++w) content[w] = tag;
      e = va::Arena::Entry{};
      e.key = key;
      e.state = va::Arena::kStored;
      e.fingerprint = version[r];
      e.offset = at;
      e.segment = segment;
      e.epoch = epoch;
      ++stores;
    }
  }
  CHECK(hits > 10000 && stores > 1000 && arena.recycles() > 10);
  std::printf("  arena seed %u: %llu hits, %llu stores, %llu refused, %llu recycles, %llu waits\n", seed,
              (unsigned long long)hits, (unsigned long long)stores, (unsigned long long)refused,
              (unsigned long long)arena.recycles(), (unsigned long long)arena.waits());
}

static void TestArenaRecycleDrops() {
  va::Arena arena;
  arena.Configure(8 * 4096, 64);
  arena.SlotStarted(0, 0);
  va::RangeKey key{0x1000, 4096, 16, 2};
  va::Arena::Entry& e = arena.Slot(key);
  uint32_t at, segment, epoch;
  CHECK(arena.Allocate(4096, 4, at, segment, epoch) && at == 0 && segment == 0);
  e.key = key;
  e.state = va::Arena::kStored;
  e.segment = segment;
  e.epoch = epoch;
  CHECK(arena.Live(e));
  // Fill the other seven segments, frames on one slot so each completes when the next starts.
  for (uint32_t f = 1; f <= 7; ++f) {
    arena.SlotStarted(0, f);
    CHECK(arena.Allocate(4096, 4, at, segment, epoch) && segment == f);
    // Segment 7 full: segment 0 is next in line and drains (no hits from it any more).
    CHECK(arena.Live(e) == (f < 7));
  }
  // Segment 0 was last used by frame 0 (complete since frame 1 started): recycled now, the entry dies.
  arena.SlotStarted(0, 8);
  CHECK(arena.Allocate(4096, 4, at, segment, epoch) && segment == 0 && at == 0);
  CHECK(!arena.Live(e));
  // A hit in the current frame keeps segment 1 from being recycled until that frame completes.
  arena.Use(1);
  CHECK(!arena.Allocate(4096, 4, at, segment, epoch));  // segment 0 full, segment 1 used by frame 8 (in flight)
  CHECK(arena.waits() == 1);
  arena.SlotStarted(0, 9);  // frame 8 complete
  CHECK(arena.Allocate(4096, 4, at, segment, epoch) && segment == 1);
  CHECK(!arena.Allocate(8 * 4096, 4, at, segment, epoch));  // larger than a segment
  // DropAll kills every entry at once.
  va::Arena::Entry& g = arena.Slot(va::RangeKey{0x9000, 64, 16, 2});
  g.key = va::RangeKey{0x9000, 64, 16, 2};
  g.state = va::Arena::kStored;
  g.segment = segment;
  g.epoch = epoch;
  CHECK(arena.Live(g));
  arena.DropAll();
  CHECK(!arena.Live(g));
}

// Hot ranges in every segment, hit every frame: the arena must keep taking new ranges (draining), never wait forever.
static void TestArenaHotEverywhere() {
  constexpr uint32_t kSlots = 3;
  va::Arena arena;
  arena.Configure(8 * 8192, 256);
  std::vector<va::Arena::Entry> hot;
  uint64_t stores = 0;
  for (uint64_t frame = 0; frame < 400; ++frame) {
    arena.SlotStarted(uint32_t(frame % kSlots), frame);
    for (va::Arena::Entry& h : hot) {
      if (arena.Live(h)) arena.Use(h.segment);
    }
    uint32_t at, segment, epoch;
    if (arena.Allocate(2048, 4, at, segment, epoch)) {
      ++stores;
      va::Arena::Entry h;
      h.key = va::RangeKey{uint32_t(frame) << 12, 2048, 16, 2};
      h.state = va::Arena::kStored;
      h.segment = segment;
      h.epoch = epoch;
      if (hot.size() < 64) hot.push_back(h);
      else hot[frame % 64] = h;
    }
  }
  CHECK(stores > 200);
  CHECK(arena.recycles() > 40);
}

int main() {
  TestMeasure();
  TestExempt();
  TestArenaRecycleDrops();
  TestArenaHotEverywhere();
  for (uint32_t seed = 1; seed <= 4; ++seed) TestArena(seed);
  if (g_failures) {
    std::printf("%d failures\n", g_failures);
    return 1;
  }
  std::printf("vertex arena: all checks passed\n");
  return 0;
}

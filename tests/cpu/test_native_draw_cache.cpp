// Host test of app/src/native/masseffect/me_draw_cache.h (masseffect_native_draw_cache_*, docs/frame-coherence.md).
//
//  - TextureSets: a hit always has exactly the key's shader, registers and fetch words, and its bindings are the ones
//    stored for them; a binding computed for other fetch words is never stored. Random register files where the
//    game changes fetch words, sampler lists of random shaders, slot collisions.
//  - PipelineMemo: a hit always returns the value stored for exactly that key and context (reference map), through
//    the one-entry shortcut and the table, with collisions.
//  - IndexArena: an arena range is never overwritten while a frame that drew with it may still be on the GPU, and a
//    hit always describes the current guest bytes (model of three work slots whose frames complete at random times
//    before the slot is reused, guest rewrites, ranges of every size, resets when full).
//  - Verifier: the first N hits and then 1 in K are checked; a difference turns it off.
//   clang++ -std=c++20 -O2 -I app/src/native/masseffect tests/cpu/test_native_draw_cache.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <tuple>
#include <vector>

#include "me_draw_cache.h"

namespace dc = me::native::draw_cache;

static int g_failures = 0;
#define CHECK(c)                                       \
  do {                                                 \
    if (!(c)) {                                        \
      std::printf("FAIL line %d: %s\n", __LINE__, #c); \
      ++g_failures;                                    \
    }                                                  \
  } while (0)

struct Binding {
  std::array<uint32_t, 6> fetch{};
  uint32_t slot = 0, heap = 0, sampler = 0, width = 0, height = 0;
  uint64_t generation = 0;
};

// The "slow path": a pure function of the fetch words and a generation (what PrepareTexture + SlotSampler give).
static Binding Slow(const uint32_t* f, uint64_t generation) {
  Binding b;
  std::memcpy(b.fetch.data(), f, 24);
  uint64_t h = generation * 0x9E37u;
  for (int i = 0; i < 6; ++i) h = dc::Mix(h, f[i]);
  b.slot = uint32_t(h);
  b.heap = uint32_t(h >> 32) % 3;
  b.sampler = uint32_t(h >> 40) & 0xFF;
  b.width = 1 + uint32_t(h >> 48) % 2048;
  b.height = 1 + uint32_t(h >> 52) % 2048;
  b.generation = generation;
  return b;
}

static void TestTextureSets() {
  std::mt19937_64 rng(5);
  auto sets = std::make_unique<dc::TextureSets<Binding, 8, 32>>();  // small table: collisions
  std::vector<uint32_t> registers(16 * 6);
  for (auto& w : registers) w = uint32_t(rng());
  struct Shader { uint32_t id; std::vector<uint8_t> regs; };
  std::vector<Shader> shaders;
  for (uint32_t s = 0; s < 12; ++s) {
    Shader sh{s + 1, {}};
    const uint32_t n = uint32_t(rng() % 10);  // 9 does not fit: MakeKey refuses
    for (uint32_t i = 0; i < n; ++i) sh.regs.push_back(uint8_t(rng() % 16));
    shaders.push_back(sh);
  }
  // A small pool of fetch constants, so texture sets repeat as in a game.
  std::vector<std::array<uint32_t, 6>> pool(6);
  for (auto& f : pool)
    for (auto& w : f) w = uint32_t(rng());
  uint64_t generation = 1;
  uint64_t hits = 0, misses = 0, clashes = 0, refused = 0, too_long = 0;
  for (int it = 0; it < 300000; ++it) {
    // The game writes some fetch constants.
    const int writes = rng() % 4 == 0 ? 1 : 0;
    for (int w = 0; w < writes; ++w) {
      const uint32_t reg = uint32_t(rng() % 16);
      if (rng() % 8 == 0) {
        registers[reg * 6 + rng() % 6] ^= 1u << (rng() % 32);  // a one-bit change of one word
      } else {
        std::memcpy(&registers[reg * 6], pool[rng() % pool.size()].data(), 24);
      }
    }
    if (rng() % 500 == 0) ++generation;  // an invalidation of everything
    const Shader& sh = shaders[rng() % shaders.size()];
    using Sets = dc::TextureSets<Binding, 8, 32>;
    Sets::Key key;
    if (!Sets::MakeKey(sh.id, sh.regs.data(), uint32_t(sh.regs.size()), registers.data(), key)) {
      CHECK(sh.regs.size() > 8);
      ++too_long;
      continue;
    }
    bool clash = false;
    Sets::Entry* e = sets->Find(key, clash);
    // The client's validity rule: every binding of the current generation.
    bool valid = e != nullptr;
    for (uint32_t i = 0; valid && i < key.n; ++i) valid = e->bindings[i].generation == generation;
    if (e) {
      CHECK(e->shader == sh.id && e->n == sh.regs.size());
      for (uint32_t i = 0; i < key.n; ++i) {
        CHECK(e->registers[i] == sh.regs[i]);
        CHECK(std::memcmp(e->bindings[i].fetch.data(), &registers[sh.regs[i] * 6], 24) == 0);
      }
    }
    if (valid) {
      ++hits;
      for (uint32_t i = 0; i < key.n; ++i) {
        const Binding want = Slow(&registers[sh.regs[i] * 6], generation);
        CHECK(e->bindings[i].slot == want.slot && e->bindings[i].heap == want.heap &&
              e->bindings[i].sampler == want.sampler && e->bindings[i].width == want.width &&
              e->bindings[i].height == want.height);
      }
      continue;
    }
    ++misses;
    clashes += clash ? 1 : 0;
    Binding bindings[8];
    for (uint32_t i = 0; i < key.n; ++i) bindings[i] = Slow(&registers[sh.regs[i] * 6], generation);
    if (key.n && rng() % 50 == 0) {
      // A binding computed for other words (a register rewritten in between) must be refused.
      Binding wrong[8];
      std::copy(bindings, bindings + key.n, wrong);
      wrong[rng() % key.n].fetch[rng() % 6] ^= 4;
      CHECK(!sets->Store(key, wrong));
      ++refused;
    }
    CHECK(sets->Store(key, bindings));
  }
  std::printf("texture sets: %llu hits, %llu misses (%llu clashes), %llu wrong stores refused, %llu too long\n",
              (unsigned long long)hits, (unsigned long long)misses, (unsigned long long)clashes,
              (unsigned long long)refused, (unsigned long long)too_long);
  CHECK(hits > 50000);
  CHECK(clashes > 0);
}

struct Key {
  uint32_t words[34];
};

static void TestPipelineMemo() {
  std::mt19937_64 rng(7);
  auto memo = std::make_unique<dc::PipelineMemo<Key, uint64_t, 32>>();
  std::vector<Key> keys(48);
  for (auto& k : keys) {
    for (auto& w : k.words) w = uint32_t(rng() % 4);  // keys that differ in few words
  }
  std::map<std::pair<size_t, uint32_t>, uint64_t> reference;
  uint64_t hits = 0, misses = 0, clashes = 0;
  size_t last = 0;
  for (int it = 0; it < 400000; ++it) {
    const size_t i = rng() % 3 ? last : rng() % keys.size();  // consecutive draws often repeat
    last = i;
    const uint32_t context = uint32_t(rng() % 3);
    bool clash = false;
    const uint64_t* v = memo->Find(keys[i], context, clash);
    // Keys with equal bytes are the same key for the memo: the reference is by content.
    size_t canonical = i;
    for (size_t j = 0; j < keys.size(); ++j) {
      if (std::memcmp(&keys[j], &keys[i], sizeof(Key)) == 0) {
        canonical = j;
        break;
      }
    }
    const auto ref = reference.find({canonical, context});
    if (v) {
      ++hits;
      CHECK(ref != reference.end() && ref->second == *v);
    } else {
      ++misses;
      clashes += clash ? 1 : 0;
      const uint64_t value = rng() | 1;
      reference[{canonical, context}] = value;
      memo->Store(keys[i], context, value);
    }
  }
  std::printf("pipeline memo: %llu hits (%llu by the last-key shortcut), %llu misses (%llu clashes)\n",
              (unsigned long long)hits, (unsigned long long)memo->last_hits(), (unsigned long long)misses,
              (unsigned long long)clashes);
  CHECK(hits > 200000);
  CHECK(clashes > 0);
}

static void TestIndexArena() {
  std::mt19937_64 rng(13);
  dc::IndexArena arena;
  constexpr uint64_t kCapacity = 1024 * 1024;
  arena.Configure(kCapacity, 256);
  // Guest index ranges: address -> content version (the fingerprint is the version, never 0).
  struct Range { uint64_t key; uint32_t count; uint64_t version; };
  std::vector<Range> ranges;
  for (int i = 0; i < 300; ++i) ranges.push_back({uint64_t(0x1000 + i * 0x100) << 2, 1 + uint32_t(rng() % 2000), 1});
  // What the arena memory holds, per byte region: (key, count, version) written at an offset.
  struct Written { uint32_t offset, bytes; uint64_t key; uint32_t count; uint64_t version; };
  std::vector<Written> memory;  // live writes (later writes overlapping earlier ones replace them)
  // GPU model: frames in flight, with the arena regions each one read.
  constexpr uint32_t kSlots = 3;
  struct Frame { uint64_t frame; uint32_t slot; bool done; std::vector<std::pair<uint32_t, uint32_t>> reads; };
  std::vector<Frame> frames;
  uint64_t frame = 0;
  uint32_t slot = 0;
  frames.push_back({0, 0, false, {}});
  arena.SlotStarted(0, 0);
  uint64_t hits = 0, stored = 0, overwrites_checked = 0;
  for (int it = 0; it < 400000; ++it) {
    // The GPU finishes some frames (in any order), never the one being recorded.
    for (Frame& f : frames)
      if (!f.done && f.frame != frame && rng() % 4 == 0) f.done = true;
    if (rng() % 300 == 0) {
      // New frame on the next slot: the slot's previous frame must be complete first (the client waits its fence).
      slot = (slot + 1) % kSlots;
      for (Frame& f : frames)
        if (f.slot == slot) f.done = true;
      ++frame;
      frames.push_back({frame, slot, false, {}});
      arena.SlotStarted(slot, frame);
      std::vector<Frame> keep;
      for (Frame& f : frames)
        if (!f.done) keep.push_back(std::move(f));
      frames.swap(keep);
    }
    Range& r = ranges[rng() % ranges.size()];
    if (rng() % 40 == 0) ++r.version;  // the game rewrites the indices
    const auto* e = arena.Find(r.key, r.count, r.version);
    Frame& current = frames.back();
    if (e) {
      ++hits;
      // The bytes at the entry's offset are this range's current content.
      bool found = false;
      for (const Written& w : memory) {
        if (w.offset == e->offset) {
          found = true;
          CHECK(w.key == r.key && w.count == r.count && w.version == r.version);
        }
      }
      CHECK(found);
      current.reads.push_back({e->offset, r.count * 2});
      continue;
    }
    uint32_t offset = 0;
    const uint32_t bytes = r.count * 2;
    if (!arena.Allocate(bytes, 4, offset)) continue;  // the draw uses the per-frame upload buffer
    CHECK(uint64_t(offset) + bytes <= kCapacity);
    CHECK(offset % 4 == 0);
    // No frame still on the GPU may have read anything this write overlaps.
    for (const Frame& f : frames) {
      if (f.done) continue;
      for (const auto& [o, b] : f.reads) {
        ++overwrites_checked;
        const bool overlap = offset < o + b && o < offset + bytes;
        if (overlap) {
          // Allowed only if it is the frame being recorded reading what this very frame wrote after the last reset,
          // which cannot happen: an overlap means the space was reused after a reset, and a reset waits for all.
          CHECK(!overlap);
        }
      }
    }
    std::vector<Written> keep;
    for (const Written& w : memory)
      if (!(offset < w.offset + w.bytes && w.offset < offset + bytes)) keep.push_back(w);
    keep.push_back({offset, bytes, r.key, r.count, r.version});
    memory.swap(keep);
    arena.Insert(r.key, r.count, r.version, 0, 0, offset);
    current.reads.push_back({offset, bytes});
    ++stored;
  }
  uint64_t lo = 0, hi = 0;
  const bool dirty = arena.TakeDirty(lo, hi);
  std::printf("index arena: %llu hits, %llu stored, %llu resets, %llu releases, %llu overlap checks\n",
              (unsigned long long)hits, (unsigned long long)stored, (unsigned long long)arena.resets(),
              (unsigned long long)arena.releases(), (unsigned long long)overwrites_checked);
  CHECK(hits > 50000);
  CHECK(arena.resets() > 5);
  CHECK(arena.releases() >= arena.resets() - 1);
  CHECK(dirty && hi > lo && hi <= kCapacity);
  CHECK(!arena.TakeDirty(lo, hi));
  // Promotion: the second sighting of the same content.
  CHECK(!arena.SeenBefore(0x777, 10, 5));
  CHECK(arena.SeenBefore(0x777, 10, 5));
  CHECK(!arena.SeenBefore(0x777, 10, 6));  // other content: first sighting again
  // Fingerprint 0 (range not fully hashed) never hits and is never stored.
  dc::IndexArena small;
  small.Configure(4096, 16);
  uint32_t off = 0;
  CHECK(small.Allocate(64, 4, off));
  small.Insert(8, 32, 0, 0, 0, off);
  CHECK(!small.Find(8, 32, 0));
  // Full: a reset; no allocation until every slot has moved past the frames started before it.
  small.SlotStarted(0, 10);
  small.SlotStarted(1, 11);
  small.Insert(16, 32, 5, 1, 2, off);
  CHECK(small.Find(16, 32, 5) != nullptr);
  CHECK(!small.Allocate(8192, 4, off));  // does not fit: reset
  CHECK(!small.usable());
  CHECK(small.Find(16, 32, 5) == nullptr);
  small.SlotStarted(2, 12);  // slot 2 had no frame before the reset
  CHECK(!small.usable());
  small.SlotStarted(0, 13);  // frame 10 complete
  CHECK(!small.usable());
  small.SlotStarted(1, 14);  // frame 11 complete: everything started before the reset is complete
  CHECK(small.usable());
  CHECK(small.Allocate(64, 4, off) && off == 0);
}

static void TestVerifier() {
  dc::Verifier v;
  v.Configure(true, 10, 100);
  uint64_t due = 0;
  for (int i = 0; i < 1000; ++i) due += v.Due() ? 1 : 0;
  CHECK(due == 10 + 10);  // hits 1-10, then 100, 200, ..., 1000
  for (int i = 0; i < 10; ++i) v.Result(true);
  CHECK(v.FirstChecksDone());
  CHECK(!v.FirstChecksDone());
  CHECK(v.on());
  CHECK(!v.Result(false));
  CHECK(!v.on() && v.differences() == 1);
  dc::Verifier none;
  none.Configure(true, 0, 0);
  for (int i = 0; i < 100; ++i) CHECK(!none.Due());
}

int main() {
  TestTextureSets();
  TestPipelineMemo();
  TestIndexArena();
  TestVerifier();
  if (g_failures) {
    std::printf("%d failures\n", g_failures);
    return 1;
  }
  std::printf("all draw cache checks passed\n");
  return 0;
}

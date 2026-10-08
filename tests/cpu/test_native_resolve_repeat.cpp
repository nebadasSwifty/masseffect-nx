// masseffect_native_resolve_repeat (docs/vulkan-frame-time.md section 11): a resolve is skipped only when its
// destination already holds exactly what it would write.
//
// 1. ResolveRepeatCache against a texel-level model: a texture of 8x8 cells written by hooked resolves (content =
//    function of request key and source signature), by unknown writers (they bump the revision, as ResolvedWritten
//    does for plain copies, swaps, pool wakes) and by hooked resolves into other rectangles (two-segment predicated
//    tiling). Millions of random steps: whenever Repeats() says yes, every cell of the rectangle must already hold
//    the value the resolve would write. A negative control (a cache that ignores the revision) must be caught.
// 2. StencilOnlyVersions against a model of one tile's depth plane: random sequences of depth writes, stencil-only
//    changes, ownership moves and changes made while the bookkeeping was off; whenever the depth version of now
//    equals the one recorded earlier, the depth plane must be the same.
#include "me_resolve_repeat.h"

#include <cstdint>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <vector>

using namespace me::native;

static void Check(bool b, const char* s) {
  if (!b) throw std::runtime_error(s);
}

namespace {

constexpr uint32_t kCells = 8;

struct Model {
  std::vector<uint64_t> cells = std::vector<uint64_t>(kCells * kCells, 0);
  uint64_t revision = 1;
  uint64_t image = 100;
};

uint64_t Content(const ResolveRepeatKey& key, const std::vector<uint64_t>& signature, uint32_t x, uint32_t y) {
  uint64_t h = 1469598103934665603ull;
  for (uint64_t v : key) h = (h ^ v) * 1099511628211ull;
  for (uint64_t v : signature) h = (h ^ v) * 1099511628211ull;
  return (h ^ (uint64_t(x) << 8) ^ y) * 1099511628211ull;
}

bool Holds(const Model& m, const ResolveRepeatKey& key, const std::vector<uint64_t>& signature,
           const std::array<uint32_t, 4>& r) {
  for (uint32_t y = r[1]; y < r[1] + r[3]; ++y)
    for (uint32_t x = r[0]; x < r[0] + r[2]; ++x)
      if (m.cells[y * kCells + x] != Content(key, signature, x, y)) return false;
  return true;
}

// Revision-blind cache for the negative control: same API, ignores revisions entirely.
struct BlindCache {
  std::vector<ResolveRepeatRecord> records;
  bool Repeats(const ResolveRepeatKey& key, const std::vector<uint64_t>& signature, uint64_t destination) const {
    for (const auto& r : records)
      if (r.key == key && r.signature == signature && r.destination == destination) return true;
    return false;
  }
  void Written(ResolveRepeatRecord record) {
    std::vector<ResolveRepeatRecord> kept;
    for (auto& r : records)
      if (!OverlapResolveRect(r.rect, record.rect) && !(r.key == record.key)) kept.push_back(r);
    kept.push_back(record);
    records = kept;
  }
};

template <typename CacheT, bool kBlind>
uint64_t Simulate(uint64_t seed, uint64_t steps, uint64_t& repeats, uint64_t& wrong) {
  std::mt19937_64 rng(seed);
  Model m;
  CacheT cache;
  const std::array<std::array<uint32_t, 4>, 4> rects = {{{0, 0, 8, 5}, {0, 5, 8, 3}, {0, 0, 8, 8}, {2, 2, 3, 3}}};
  uint64_t version = 1;
  repeats = wrong = 0;
  for (uint64_t i = 0; i < steps; ++i) {
    const uint32_t action = uint32_t(rng() % 100);
    if (action < 6) {  // unknown writer (plain copy, swap, crop source change): new revision, random content
      for (auto& c : m.cells) c = rng() | 1;
      ++m.revision;
      continue;
    }
    if (action < 8) {  // pool wake / recreation: new image and revision
      m.image = 100 + rng() % 4;
      for (auto& c : m.cells) c = 0;
      ++m.revision;
      continue;
    }
    // A hooked resolve: small request and source spaces so that repeats are frequent.
    const auto& rect = rects[rng() % (action < 60 ? 2 : rects.size())];
    ResolveRepeatKey key{};
    key[0] = rng() % 2;
    key[3] = rect[0] | (uint64_t(rect[1]) << 32);
    key[4] = rect[2] | (uint64_t(rect[3]) << 32);
    std::vector<uint64_t> signature = {rng() % 3, rng() % 2};
    bool repeat;
    if constexpr (kBlind)
      repeat = cache.Repeats(key, signature, m.image);
    else
      repeat = cache.Repeats(7, key, signature, m.image, m.revision, version);
    if (repeat) {
      ++repeats;
      if (!Holds(m, key, signature, rect)) ++wrong;
      continue;  // skipped: nothing written
    }
    const uint64_t before = m.revision;
    for (uint32_t y = rect[1]; y < rect[1] + rect[3]; ++y)
      for (uint32_t x = rect[0]; x < rect[0] + rect[2]; ++x) m.cells[y * kCells + x] = Content(key, signature, x, y);
    ++m.revision;
    ResolveRepeatRecord record;
    record.key = key;
    record.signature = signature;
    record.rect = rect;
    record.destination = m.image;
    record.revision = m.revision;
    record.version_floor = version;
    if constexpr (kBlind)
      cache.Written(record);
    else
      cache.Written(7, record, before);
    version += rng() % 3;
  }
  return repeats;
}

void TestCacheModel() {
  uint64_t total_repeats = 0;
  for (uint64_t seed = 1; seed <= 40; ++seed) {
    uint64_t repeats = 0, wrong = 0;
    Simulate<ResolveRepeatCache, false>(seed, 100000, repeats, wrong);
    Check(wrong == 0, "a repeat was reported while the texture did not hold the result");
    total_repeats += repeats;
  }
  Check(total_repeats > 100000, "the model produced too few repeats to be meaningful");
  uint64_t repeats = 0, wrong = 0;
  Simulate<BlindCache, true>(99, 100000, repeats, wrong);
  Check(wrong > 0, "negative control (revision ignored) was not caught");
  std::printf("  cache model: %llu repeats checked, 0 wrong; revision-blind control: %llu wrong\n",
              (unsigned long long)total_repeats, (unsigned long long)wrong);
}

void TestCacheBasics() {
  ResolveRepeatCache cache;
  ResolveRepeatKey top{1, 2, 3}, bottom{1, 2, 4};
  const std::vector<uint64_t> sig{5, 6, 7};
  ResolveRepeatRecord a;
  a.key = top;
  a.signature = sig;
  a.rect = {0, 0, 1280, 512};
  a.destination = 9;
  a.revision = 11;
  cache.Written(0x14B38000, a, 10);
  ResolveRepeatRecord b = a;
  b.key = bottom;
  b.rect = {0, 512, 1280, 208};
  b.revision = 12;
  cache.Written(0x14B38000, b, 11);
  Check(cache.Repeats(0x14B38000, top, sig, 9, 12, 0), "the top segment survives the bottom one");
  Check(cache.Repeats(0x14B38000, bottom, sig, 9, 12, 0), "the bottom segment repeats");
  Check(!cache.Repeats(0x14B38000, top, sig, 9, 13, 0), "an unknown write invalidates");
  Check(!cache.Repeats(0x14B38000, top, sig, 8, 12, 0), "another image never matches");
  Check(!cache.Repeats(0x14B38000, top, {5, 6, 8}, 9, 12, 0), "a changed source never matches");
  ResolveRepeatRecord c = a;
  c.key = ResolveRepeatKey{1, 2, 5};
  c.rect = {100, 500, 10, 20};  // overlaps both
  c.revision = 13;
  cache.Written(0x14B38000, c, 12);
  Check(!cache.Repeats(0x14B38000, top, sig, 9, 13, 0) && !cache.Repeats(0x14B38000, bottom, sig, 9, 13, 0),
        "an overlapping write drops both segments");
  Check(cache.Repeats(0x14B38000, c.key, sig, 9, 13, 0), "the new record repeats");
  ResolveRepeatRecord d = c;
  d.version_floor = 1000;
  d.revision = 14;
  cache.Written(0x1000, d, 0);
  Check(!cache.Repeats(0x1000, d.key, sig, 9, 14, 999), "a wrapped version counter never matches");
  Check(cache.Repeats(0x1000, d.key, sig, 9, 14, 1000), "same counter matches");
  ResolveRepeatRecord e = d;
  e.signature.clear();
  e.revision = 15;
  cache.Written(0x1000, e, 14);
  Check(!cache.Repeats(0x1000, d.key, sig, 9, 15, 2000), "same key rewritten (not comparable) drops the old record");
}

// One physical tile. Depth plane content id and the tile's (global, unique) version, as the targets code assigns.
void TestStencilOnlyVersions() {
  std::mt19937_64 rng(5);
  uint64_t checks = 0, equal = 0;
  for (int run = 0; run < 2000; ++run) {
    StencilOnlyVersions versions;
    uint64_t counter = 1, version = 1, depth = 1, next_depth = 2;
    bool on = true;
    struct Snapshot { uint64_t depth_version, depth; };
    std::vector<Snapshot> snapshots;
    for (int step = 0; step < 400; ++step) {
      const uint32_t a = uint32_t(rng() % 100);
      if (a < 5) on = !on;  // the cvar toggles: changes made while off are not noted
      const uint64_t old = version;
      if (a < 30) {  // depth write (draw with depth writes, clear, import, ownership move)
        version = ++counter;
        depth = next_depth++;
      } else if (a < 70) {  // stencil-only change of the same owner and tile
        version = ++counter;
        if (on) versions.Note(0, old, version);
      } else if (a < 80) {  // keep_version move to another owner: version kept, content kept
      } else {  // a resolve reads the depth plane now
        snapshots.push_back({versions.Depth(0, version), depth});
        for (const Snapshot& s : snapshots) {
          ++checks;
          if (s.depth_version == versions.Depth(0, version)) {
            ++equal;
            Check(s.depth == depth, "equal depth versions with a different depth plane");
          }
        }
        if (snapshots.size() > 16) snapshots.erase(snapshots.begin());
      }
    }
  }
  Check(equal > checks / 20, "too few equal depth versions to be meaningful");
  // Negative control: noting a depth write as stencil-only must be caught.
  {
    StencilOnlyVersions versions;
    const uint64_t before = versions.Depth(0, 1);
    versions.Note(0, 1, 2);  // wrongly: this change wrote depth
    Check(versions.Depth(0, 2) == before, "control: a (wrong) stencil-only note keeps the depth version");
  }
  std::printf("  stencil-only versions: %llu comparisons, %llu equal, all with the same depth plane\n",
              (unsigned long long)checks, (unsigned long long)equal);
}

}  // namespace

int main() {
  try {
    TestCacheBasics();
    TestCacheModel();
    TestStencilOnlyVersions();
  } catch (const std::exception& e) {
    std::printf("FAIL: %s\n", e.what());
    return 1;
  }
  std::printf("PASS\n");
  return 0;
}

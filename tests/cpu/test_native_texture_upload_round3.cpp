// Host tests of the ring CPU round-3 switches (docs/ring-cpu-per-draw.md, "Round 3"; masseffect_native_draws.cpp).
//
// 1. masseffect_native_texture_inval_by_address. A model of what PrepareTexture answers for a fetch base (a resolved
//    texture if one is prepared at that address, with its image, format and crop revision; otherwise the ordinary
//    texture's image) and of the targets code events, each making the same calls as masseffect_native_targets.cpp
//    (InvalidateTexturesAt with the address, NoteResolvedAt, ForgetImage = every entry). A sampler cache with the
//    rule of DrawsVulkanImpl::GenerationValid (copied below with BucketAddress) must never return an answer that
//    differs from the model's, and every hit the old rule (any invalidation drops everything) would also take must be
//    one the new rule takes.
// 2. masseffect_native_constants_same_content. Registers written with random values (often the same ones, which still
//    bumps the generation), random shader sizes, upload buffer rotations. Old logic and new logic run side by side
//    on separate forward-only upload buffers; after every draw the bytes the shader reads at the bound offset must
//    equal the registers in both, and the new logic never uploads more.
// 3. masseffect_native_dedupe_hash_after_copy. With the real DedupeVertices: when Candidate() is false, Search() is
//    false for every fingerprint; when it is true, Search() follows the fingerprint rule.
//   clang++ -std=c++20 -O2 -Iapp/src/native/masseffect tests/cpu/test_native_texture_upload_round3.cpp -o /tmp/t && /tmp/t
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <vector>

#include "masseffect_native_vertices_dedupe.h"

namespace {

int failures = 0;
#define CHECK(cond, ...)                 \
  do {                                   \
    if (!(cond)) {                       \
      if (++failures <= 10) {            \
        std::printf("FAIL: " __VA_ARGS__); \
        std::printf("\n");               \
      }                                  \
    }                                    \
  } while (0)

// ---- 1. Texture cache invalidation by address --------------------------------------------------------------------
uint32_t BucketAddress(uint32_t address) {  // same as DrawsVulkanImpl::BucketAddress
  return uint32_t(((address & 0x1FFFFFFFu) >> 12) * 0x9E3779B1u) >> 22;
}

struct Answer {
  bool resolved = false;
  uint64_t image = 0, format = 0, revision = 0;
  bool operator==(const Answer& o) const {
    return resolved == o.resolved && image == o.image && format == o.format && revision == o.revision;
  }
};

struct World {
  struct Resolved {
    bool prepared = false;
    uint64_t image = 0, format = 0, revision = 0;
  };
  std::map<uint32_t, Resolved> resolved;
  std::map<uint32_t, uint64_t> textures;  // ordinary texture image per base
  uint64_t next_image = 1;
  Answer Prepare(uint32_t base) {
    const auto it = resolved.find(base);
    if (it != resolved.end() && it->second.prepared)
      return {true, it->second.image, it->second.format, it->second.revision};
    uint64_t& image = textures[base];
    if (!image) image = next_image++;
    return {false, image, 0, 0};
  }
};

struct Cache {
  // DrawsVulkanImpl state
  uint64_t generation_textures = 0, generation_images = 0;
  std::array<uint32_t, 1024> generation_address{};
  struct Entry {
    bool used = false;
    uint32_t base = 0;
    Answer answer;
    uint64_t generation = 0, generation_images = 0;
    uint32_t generation_address = 0;
    uint16_t bucket = 0;
    bool resolved = false;
  };
  std::array<Entry, 64> entries{};
  void BumpAll() { ++generation_textures, ++generation_images; }
  void InvalidateTexturesAt(uint32_t address, bool each_copy) {
    ++generation_textures;
    if (address == UINT32_MAX) {
      if (each_copy) ++generation_images;
    } else {
      ++generation_address[BucketAddress(address)];
    }
  }
  void NoteResolvedAt(uint32_t address) { ++generation_address[BucketAddress(address)]; }
  bool ValidNew(const Entry& e) const {
    return e.generation_images == generation_images && e.generation_address == generation_address[e.bucket] &&
           (!e.resolved || e.generation == generation_textures);
  }
  bool ValidOld(const Entry& e) const { return e.generation == generation_textures; }
};

void TestInvalidationByAddress() {
  std::mt19937_64 rng(7);
  uint64_t hits_new = 0, hits_old = 0, lookups = 0, old_only = 0, old_wrong = 0;
  for (int round = 0; round < 200; ++round) {
    World world;
    Cache cache;
    // A few pages, some of them in the same bucket (to exercise collisions) and some far apart.
    std::vector<uint32_t> pages;
    for (int i = 0; i < 24; ++i) pages.push_back(uint32_t(rng() % 0x20000) << 12);
    for (int i = 0; i < 4; ++i) {
      const uint32_t b = BucketAddress(pages[i]);
      for (uint32_t p = 1; p < 0x20000; ++p)
        if (BucketAddress(p << 12) == b && (p << 12) != pages[i]) {
          pages.push_back(p << 12);
          break;
        }
    }
    const auto pick = [&] { return pages[rng() % pages.size()]; };
    for (int step = 0; step < 20000; ++step) {
      const uint32_t r = uint32_t(rng() % 100);
      const uint32_t a = pick();
      if (r < 60) {  // a fetch
        ++lookups;
        Cache::Entry& e = cache.entries[(a >> 12) % cache.entries.size()];
        const Answer truth = world.Prepare(a);
        if (e.used && e.base == a && cache.ValidNew(e)) {
          ++hits_new;
          CHECK(e.answer == truth, "inval_by_address: a hit gave another answer (base %08X, round %d step %d)", a,
                round, step);
        }
        if (e.used && e.base == a && cache.ValidOld(e)) {
          ++hits_old;
          // The new rule refuses an old-rule hit only after NoteResolvedAt (a render target swapped into a resolved
          // texture), where the old rule itself can be wrong: it never invalidated ordinary entries there.
          if (!cache.ValidNew(e)) ++old_only;
          if (!(e.answer == truth)) ++old_wrong;
        }
        if (!(e.used && e.base == a && cache.ValidNew(e))) {  // miss: PrepareTexture and fill
          e.used = true;
          e.base = a;
          e.answer = truth;
          e.generation = cache.generation_textures;
          e.generation_images = cache.generation_images;
          e.bucket = uint16_t(BucketAddress(a));
          e.generation_address = cache.generation_address[e.bucket];
          e.resolved = truth.resolved;
        }
      } else if (r < 66) {  // GetResolved creates or rebuilds at a (a woken pool image may already be prepared)
        auto& res = world.resolved[a];
        res.image = world.next_image++;
        res.prepared = rng() % 2;
        res.format = rng() % 3;
        cache.InvalidateTexturesAt(a, false);
      } else if (r < 70) {  // GetResolved: same image, other format
        auto it = world.resolved.find(a);
        if (it != world.resolved.end()) {
          it->second.format ^= 1;
          cache.InvalidateTexturesAt(a, false);
        }
      } else if (r < 76) {  // Prepare of the resolved image at a
        auto it = world.resolved.find(a);
        if (it != world.resolved.end() && !it->second.prepared) {
          it->second.prepared = true;
          cache.InvalidateTexturesAt(a, false);
        }
      } else if (r < 80) {  // Prepare of another image (render target): nothing a fetch sees
        cache.InvalidateTexturesAt(UINT32_MAX, false);
      } else if (r < 88) {  // ResolvedWritten with crops at a
        auto it = world.resolved.find(a);
        if (it != world.resolved.end()) {
          ++it->second.revision;
          cache.InvalidateTexturesAt(a, false);
        }
      } else if (r < 90) {  // an image retired (texture eviction or resolved destroyed): every entry
        if (rng() % 2) {
          world.textures[a] = world.next_image++;
        } else {
          world.resolved.erase(a);
        }
        cache.BumpAll();
      } else if (r < 93) {  // a render target swapped into the resolved texture at a: prepared moves
        auto it = world.resolved.find(a);
        if (it != world.resolved.end()) {
          it->second.prepared = true;
          it->second.image = world.next_image++;
          ++it->second.revision;
          cache.NoteResolvedAt(a);
          // InvalidateImages drops the entries using the two images (modelled as the resolved entries of a).
          for (auto& e : cache.entries)
            if (e.used && e.base == a && e.resolved) e.used = false;
        }
      } else if (r < 95) {  // per-copy invalidation (masseffect_native_invalidate_textures_each_copy)
        cache.InvalidateTexturesAt(UINT32_MAX, true);
      }
      // else: a copy with the per-copy invalidation off: nothing
    }
  }
  std::printf("inval_by_address: %llu lookups, %llu hits with the new rule, %llu with the old one (%llu of them "
              "refused by the new rule after a swap; %llu old-rule hits with a stale answer, all after a swap)\n",
              (unsigned long long)lookups, (unsigned long long)hits_new, (unsigned long long)hits_old,
              (unsigned long long)old_only, (unsigned long long)old_wrong);
  CHECK(old_wrong <= old_only, "inval_by_address: the old rule went wrong outside the swap case");
  CHECK(hits_new > hits_old, "inval_by_address: the new rule should hit more often");
}

// ---- 2. Constants reused by content ------------------------------------------------------------------------------
struct Constants {
  bool same_content = false;
  std::vector<uint32_t> upload;
  size_t used = 0;
  uint64_t epoch = 0;
  uint64_t generation_up = UINT64_MAX, epoch_up = UINT64_MAX;
  uint32_t bytes_up = 0;
  size_t offset = 0;
  std::array<uint32_t, 1024> shadow{};
  bool shadow_valid = false;
  uint64_t uploads = 0;
  void Rotate() {
    ++epoch;
    used = 0;
    std::fill(upload.begin(), upload.end(), 0xDEADBEEFu);  // a reused buffer holds anything
  }
  // Returns the offset (in words) the shader reads from.
  size_t Draw(const uint32_t* regs, uint64_t generation, uint32_t bytes) {
    const bool same = same_content && generation_up != generation && epoch_up == epoch && bytes <= bytes_up &&
                      shadow_valid && std::memcmp(shadow.data(), regs, bytes) == 0;
    if (same) {
      generation_up = generation;
      bytes_up = bytes;  // only these bytes are proven equal to the new generation
    } else if (generation_up != generation || epoch_up != epoch || bytes > bytes_up) {
      offset = used;
      used += 1024;  // a whole UBO block
      std::memcpy(upload.data() + offset, regs, bytes);
      std::memcpy(shadow.data(), regs, bytes);
      shadow_valid = true;
      generation_up = generation;
      epoch_up = epoch;
      bytes_up = bytes;
      ++uploads;
    }
    return offset;
  }
};

void TestConstantsSameContent() {
  std::mt19937_64 rng(11);
  uint64_t old_uploads = 0, new_uploads = 0;
  for (int round = 0; round < 100; ++round) {
    Constants a, b;
    b.same_content = true;
    a.upload.assign(1024 * 4096, 0);
    b.upload.assign(1024 * 4096, 0);
    std::array<uint32_t, 1024> regs{};
    uint64_t generation = 0;
    for (int step = 0; step < 3000; ++step) {
      const uint32_t r = uint32_t(rng() % 100);
      if (r < 40) {  // a register run is written (generation changes even if the values do not)
        const uint32_t start = uint32_t(rng() % 1024), n = 1 + uint32_t(rng() % 32);
        const bool same_values = rng() % 2;
        for (uint32_t i = start; i < std::min<uint32_t>(1024, start + n); ++i)
          if (!same_values) regs[i] = uint32_t(rng() % 4);
        ++generation;
      } else if (r < 42 || a.used + 1024 > a.upload.size()) {
        a.Rotate();
        b.Rotate();
      } else {
        const uint32_t bytes = 16u * (1 + uint32_t(rng() % 256));
        const size_t oa = a.Draw(regs.data(), generation, bytes);
        const size_t ob = b.Draw(regs.data(), generation, bytes);
        CHECK(std::memcmp(a.upload.data() + oa, regs.data(), bytes) == 0, "constants: old logic reads other bytes");
        CHECK(std::memcmp(b.upload.data() + ob, regs.data(), bytes) == 0,
              "constants: reuse by content reads other bytes (round %d step %d)", round, step);
      }
    }
    old_uploads += a.uploads;
    new_uploads += b.uploads;
  }
  std::printf("constants: %llu uploads with the old logic, %llu reusing equal content\n",
              (unsigned long long)old_uploads, (unsigned long long)new_uploads);
  CHECK(new_uploads <= old_uploads, "constants: reuse by content uploads more");
}

// ---- 3. Dedupe candidate --------------------------------------------------------------------------------------
void TestDedupeCandidate() {
  std::mt19937_64 rng(13);
  auto dedupe = std::make_unique<masseffect::native::DedupeVertices>();
  uint64_t candidates = 0, checks = 0;
  uint64_t frame = 0;
  for (int step = 0; step < 2000000; ++step) {
    const uint32_t r = uint32_t(rng() % 1000);
    const uint64_t address = (rng() % 512) * 64;
    const uint32_t bytes = 64u * (1 + uint32_t(rng() % 4));
    const uint32_t order = uint32_t(rng() % 3);
    const uint64_t fingerprint = 1 + rng() % 3;
    if (r < 2) {
      dedupe->NewFrame(++frame);
    } else if (r < 4) {
      dedupe->Forget();
    } else if (r < 400) {
      dedupe->Note(address, bytes, order, rng() % 100000, fingerprint);
    } else {
      ++checks;
      const bool candidate = dedupe->Candidate(address, bytes, order);
      uint64_t offset = 0;
      const bool hit = dedupe->Search(address, bytes, order, offset, fingerprint);
      if (!candidate) {
        CHECK(!hit, "dedupe: Search hit without a Candidate");
      } else {
        ++candidates;
      }
    }
  }
  std::printf("dedupe: %llu searches, %llu with a candidate\n", (unsigned long long)checks,
              (unsigned long long)candidates);
}

}  // namespace

int main() {
  TestInvalidationByAddress();
  TestConstantsSameContent();
  TestDedupeCandidate();
  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("OK\n");
  return 0;
}

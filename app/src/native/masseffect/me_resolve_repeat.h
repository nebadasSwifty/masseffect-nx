#pragma once
// masseffect_native_resolve_repeat (docs/vulkan-frame-time.md section 11): a resolve that would write exactly what
// its destination texture already holds is skipped.
//
// A resolved texture is written only by resolves (and by the few paths that bump its revision: plain copies, the
// lazy front buffer, image swaps, pool wakes). If the previous hooked resolve into the same texture had the same
// request (source view, rectangles, exponent bias, formats) and its source EDRAM tiles still carry the same content
// identity (owner, owner image, tile, version and the owner's write count), the texture still holds that result
// bit for bit and the GPU work is not needed. The content identity is the one the mode-4 ownership map already
// maintains for its sync cache: a version changes with every published write of a tile, and the owner's write
// count covers the publish fast path that keeps tile versions.
//
// Depth resolves (k_24_8 / k_24_8_FLOAT) read only the depth plane, so stencil-only writes (a draw with stencil
// writes and no depth write, a redirected or canonical stencil-only clear) do not change their identity:
// StencilOnlyVersions remembers, per physical tile, the version the tile had before a run of stencil-only changes.
//
// Header-only and free of Vulkan so the bookkeeping is testable on the host (tests/cpu/test_native_resolve_repeat.cpp).
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace me::native {

// The request part of a resolve: everything that, together with the source content, decides what is written.
using ResolveRepeatKey = std::array<uint64_t, 12>;

struct ResolveRepeatRecord {
  ResolveRepeatKey key{};
  std::vector<uint64_t> signature;      // source content identity (see the file comment)
  std::array<uint32_t, 4> rect{};       // destination rectangle: x, y, width, height
  uint64_t destination = 0;             // destination image handle
  uint64_t revision = 0;                // destination revision right after this record's write
  uint64_t version_floor = 0;           // tile version counter when recorded (wrap guard)
};

inline bool OverlapResolveRect(const std::array<uint32_t, 4>& a, const std::array<uint32_t, 4>& b) {
  const uint64_t ax1 = uint64_t(a[0]) + a[2], ay1 = uint64_t(a[1]) + a[3];
  const uint64_t bx1 = uint64_t(b[0]) + b[2], by1 = uint64_t(b[1]) + b[3];
  return a[2] && a[3] && b[2] && b[3] && a[0] < bx1 && b[0] < ax1 && a[1] < by1 && b[1] < ay1;
}

class ResolveRepeatCache {
 public:
  static constexpr size_t kPerTexture = 4;

  // An earlier hooked resolve with this request and this source identity wrote `destination`, and nothing has
  // written that texture since (its revision is still the one recorded after that write).
  bool Repeats(uint32_t base, const ResolveRepeatKey& key, const std::vector<uint64_t>& signature,
               uint64_t destination, uint64_t revision, uint64_t version_now) const {
    const auto it = records_.find(base);
    if (it == records_.end() || signature.empty()) return false;
    for (const ResolveRepeatRecord& r : it->second) {
      if (r.destination == destination && r.revision == revision && version_now >= r.version_floor &&
          r.key == key && r.signature == signature)
        return true;
    }
    return false;
  }

  // A hooked resolve has written `record.rect` of `record.destination`; `revision_before` is the destination's
  // revision just before that write, `record.revision` the one after. Records of the same texture stay valid only
  // if they were valid right before the write (nothing unknown wrote in between), target the same image and do
  // not overlap the new rectangle; they move to the new revision. The rest are dropped.
  void Written(uint32_t base, ResolveRepeatRecord record, uint64_t revision_before) {
    auto& list = records_[base];
    std::vector<ResolveRepeatRecord> kept;
    kept.reserve(kPerTexture);
    for (ResolveRepeatRecord& r : list) {
      if (r.revision != revision_before || r.destination != record.destination ||
          OverlapResolveRect(r.rect, record.rect) || r.key == record.key)
        continue;
      r.revision = record.revision;
      kept.push_back(std::move(r));
    }
    if (kept.size() >= kPerTexture) kept.erase(kept.begin(), kept.begin() + (kept.size() - kPerTexture + 1));
    if (!record.signature.empty()) kept.push_back(std::move(record));
    list = std::move(kept);
    if (list.empty()) records_.erase(base);
  }

  void Forget(uint32_t base) { records_.erase(base); }
  void Clear() { records_.clear(); }
  size_t Textures() const { return records_.size(); }

 private:
  std::unordered_map<uint32_t, std::vector<ResolveRepeatRecord>> records_;
};

// Per physical EDRAM tile: the content version of the depth plane. A stencil-only change of a tile (same owner, same
// tile) gets a new tile version as usual, but the depth plane keeps the version it had before the first of a run
// of stencil-only changes.
class StencilOnlyVersions {
 public:
  static constexpr uint32_t kTiles = 2048;

  // The tile's version goes from `old_version` to `new_version` through a stencil-only change.
  void Note(uint32_t physical, uint64_t old_version, uint64_t new_version) {
    physical &= kTiles - 1;
    if (stencil_only_[physical] != old_version || !valid_[physical]) depth_[physical] = old_version;
    stencil_only_[physical] = new_version;
    valid_[physical] = true;
  }

  // Version of the depth plane of a tile whose current version is `version`. Any change other than a stencil-only
  // one gives the tile a version that differs from the recorded stencil-only one, so it is returned as is.
  uint64_t Depth(uint32_t physical, uint64_t version) const {
    physical &= kTiles - 1;
    return valid_[physical] && stencil_only_[physical] == version ? depth_[physical] : version;
  }

  void Clear() {
    valid_.fill(false);
  }

 private:
  std::array<uint64_t, kTiles> stencil_only_{};
  std::array<uint64_t, kTiles> depth_{};
  std::array<bool, kTiles> valid_{};
};

}  // namespace me::native

#pragma once
// masseffect_native_bias_life_probe (docs/vulkan-frame-time.md section 12): what happens around every
// exponent-bias resolve (the HDR scene colour resolved with 2^-3 into an RGBA16F texture), to decide with numbers
// which way of removing it is exact and how many resolves each way would remove. Diagnostic only: it reads draw
// registers, publish write counts and resolve requests; nothing here changes what is recorded.
//
// Two histories are kept:
//
//  - Per source image (the RGBA16F host image a bias resolve reads), the chain of draws that wrote it since the
//    last "full start": a draw that writes the image with ONE/ZERO (no blending on the written channels, render
//    target exponent bias 0), colour mask F, no alpha test / alpha-to-mask / kill, with a proven overwrite
//    rectangle. A chain continues through draws that write the image without blending (any mask, kills allowed:
//    a second output of the same draw would see the same masks and kills) and breaks at a blended draw, at a write
//    of the same EDRAM tiles through another view (the image would import those tiles later), at a clear of those
//    tiles, or when the image's write count moved without one of its draws. At a resolve the chain gives the
//    "producer" class:
//      strict    the last write is one full start whose rectangle covers the resolve rectangle,
//      chain     a full start covering the resolve rectangle, then only non-blended draws,
//      blended   a blended draw wrote the image since the last full start,
//      other     another view, a clear, or a write the probe did not see touched the image since the chain,
//      small     a full start exists but its rectangle does not cover the resolve rectangle,
//      no start  the image was never written by a full start the probe saw.
//    "strict" and "chain" are the resolves that a second colour output of the producing draws (dual output, the
//    2^-3 applied in the output epilogue) could replace exactly; the other classes cannot be replaced that way.
//    A "round trip" is a strict resolve whose full start sampled the destination texture's current content: if
//    that draw only multiplied it by 2^3, the resolve writes back exactly what the texture holds (elidable).
//
//  - Per resolved texture address, the "life" of a bias resolve: from the resolve to the next resolve or copy into
//    the same address. Every draw whose pixel shader samples the address is a reader. The life records whether a
//    reader also writes the source image itself, whether a reader came after the source image was written again
//    (a draw into it, a clear of it, or an import into it at a resolve's sync; writes of the same tiles through
//    another view leave the source image as it is), the readers' fetch-constant filters, and the first write or
//    clear of the source tiles through any view after the resolve: if that is a full ONE/ZERO overwrite (or a
//    clear) of the whole resolve rectangle, the EDRAM content the resolve read is dead after it. At the end:
//      unread    no reader: deferring the resolve until its first read would drop it,
//      hand-off  readers exist, none writes the source image and none comes after it was written again: the
//                texture could be the source image itself (with the 2^-3 applied where it is read),
//      copy (writer)   a reader writes the source image itself: the source must be copied anyway,
//      copy (changed)  a reader samples after the source image was written again: likewise.
//
// EDRAM tile spans are physical tile indices on the 2048-tile ring: [first, first + count) modulo 2048.
//
// Header-only and free of Vulkan so the bookkeeping is testable on the host (tests/cpu/test_native_bias_life.cpp).
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace me::native {

// Rectangles are guest pixels: x0, y0, x1, y1 (exclusive).
using BiasLifeRect = std::array<int32_t, 4>;

inline bool BiasLifeContains(const BiasLifeRect& outer, const BiasLifeRect& inner) {
  return inner[2] > inner[0] && inner[3] > inner[1] && outer[0] <= inner[0] && outer[1] <= inner[1] &&
         outer[2] >= inner[2] && outer[3] >= inner[3];
}

struct BiasLifeSpan {
  uint32_t first = 0;  // physical tile, 0..2047
  uint32_t count = 0;  // 0 = empty, 2048 = all
};

inline bool BiasLifeOverlap(const BiasLifeSpan& a, const BiasLifeSpan& b) {
  if (!a.count || !b.count) return false;
  if (a.count >= 2048 || b.count >= 2048) return true;
  // Distance from a's start to b's start along the ring, and back.
  const uint32_t ab = (b.first - a.first) & 2047u;
  const uint32_t ba = (a.first - b.first) & 2047u;
  return ab < a.count || ba < b.count;
}

// A write count the caller did not capture (the check against the chain is skipped).
inline constexpr uint64_t kBiasLifeUnknown = ~uint64_t(0);

// One colour slot of a draw that writes an EDRAM view.
struct BiasLifeWrite {
  uint64_t image = 0;             // identity of the host image written
  uint32_t base = 0;              // EDRAM base of the view
  BiasLifeSpan span;              // tiles the draw touches in that view
  bool direct = false;            // ONE/ZERO/ADD on the written channels and render target exponent bias 0
  bool full_mask = false;         // colour mask F
  bool no_kill = false;           // no alpha test, alpha-to-mask or shader kill
  bool has_proven = false;        // a proven overwrite rectangle for this slot
  BiasLifeRect proven{};
  uint64_t writes_before = kBiasLifeUnknown;  // the image's write count before the draw (before its syncs)
  uint64_t writes_after = 0;      // ... and after its publish
};

// One pixel-shader sampler fetch of a 2D texture.
struct BiasLifeRead {
  uint32_t address = 0;           // guest base address of the texture (fetch constant)
  bool point = false;             // magnification and minification filters are both point
  bool exp_adjust = false;        // the fetch constant has a non-zero exponent adjust
};

struct BiasLifeDraw {
  uint32_t vs = 0;
  uint32_t ps = 0;
  std::vector<BiasLifeRead> reads;
  std::vector<BiasLifeWrite> writes;       // colour slots
  std::vector<BiasLifeSpan> depth_writes;  // depth/stencil writes (they share the EDRAM tiles)
};

enum BiasLifeProducer : uint32_t {
  kBiasProducerStrict = 0,
  kBiasProducerChain,
  kBiasProducerBlended,
  kBiasProducerOther,
  kBiasProducerSmall,
  kBiasProducerNoStart,
  kBiasProducerCount
};

enum BiasLifeEnd : uint32_t {
  kBiasEndUnread = 0,
  kBiasEndHandOff,
  kBiasEndCopyWriter,
  kBiasEndCopyChanged,
  kBiasEndCount
};

struct BiasLifeCounts {
  uint64_t resolves = 0;
  uint64_t full_cover = 0;                       // the resolve covers the whole texture
  std::array<uint64_t, kBiasProducerCount> producer{};
  std::array<uint64_t, kBiasEndCount> end{};
  uint64_t hand_off_point = 0;                   // hand-off lives whose reads are all point-filtered
  uint64_t dead = 0;                             // first later write of the source tiles fully overwrites the rect
  uint64_t not_dead = 0;                         // ... does not (blended, partial, a smaller rectangle)
  uint64_t reads = 0;                            // reader draws over all ended lives
  uint64_t reads_exp_adjust = 0;                 // lives with a reader fetch that has an exponent adjust
  uint64_t dual_and_dead = 0;                    // producer strict/chain and the source dead after the resolve
  uint64_t round_trip = 0;                       // producer strict and its start sampled the destination's content
};

class BiasLifeProbe {
 public:
  struct Life {
    bool active = false;
    uint64_t source = 0;
    BiasLifeSpan span;            // the source tiles of the resolve rectangle
    uint32_t base = 0;
    BiasLifeRect rect{};
    uint64_t source_writes = 0;   // the source's write count at the resolve
    uint32_t producer = kBiasProducerNoStart;
    uint64_t producer_site = 0;   // VS << 32 | PS of the chain start (or of the last writer)
    uint32_t readers = 0;
    bool all_point = true;
    bool exp_adjust = false;
    bool reader_writes_source = false;
    bool read_after_change = false;
    bool source_changed = false;  // the source image was written after the resolve
    bool touched = false;         // the first such write was seen
    bool dead = false;
    std::vector<uint32_t> reader_ps;
    uint64_t serial = 0;          // distinguishes successive lives of one address
  };

  struct Chain {
    bool known = false;
    bool started = false;         // a full start is in the chain
    bool broken_blend = false;    // a blended draw since the last full start
    bool broken_other = false;    // another view, a clear or an unseen write since the last draw
    uint32_t length = 0;          // draws since the full start (1 = the start alone)
    BiasLifeRect start_rect{};
    uint64_t start_site = 0;
    uint64_t last_site = 0;
    uint64_t writes_after = 0;    // the image's write count after the chain's last draw
    BiasLifeSpan span;            // tiles of the image's last draw
    std::vector<std::pair<uint32_t, uint64_t>> start_reads;  // tracked textures the full start sampled (life serial)
  };

  bool Tracks(uint32_t address) const {
    const auto it = lives_.find(address);
    return it != lives_.end() && it->second.active;
  }
  bool AnyLife() const {
    for (const auto& [address, life] : lives_)
      if (life.active) return true;
    return false;
  }
  template <typename F> void ForEachLife(F&& f) const {
    for (const auto& [address, life] : lives_)
      if (life.active) f(address, life);
  }
  const Chain* ChainOf(uint64_t image) const {
    const auto it = chains_.find(image);
    return it == chains_.end() ? nullptr : &it->second;
  }

  // A draw that was recorded (readers first: a draw samples before it writes).
  void Draw(const BiasLifeDraw& d) {
    const uint64_t site = (uint64_t(d.vs) << 32) | d.ps;
    for (auto& [address, life] : lives_) {
      if (!life.active) continue;
      bool reads = false, point = true, adjust = false;
      for (const BiasLifeRead& r : d.reads) {
        if (r.address != address) continue;
        reads = true;
        point = point && r.point;
        adjust = adjust || r.exp_adjust;
      }
      if (!reads) continue;
      ++life.readers;
      life.all_point = life.all_point && point;
      life.exp_adjust = life.exp_adjust || adjust;
      if (life.reader_ps.size() < 8 &&
          std::find(life.reader_ps.begin(), life.reader_ps.end(), d.ps) == life.reader_ps.end())
        life.reader_ps.push_back(d.ps);
      for (const BiasLifeWrite& w : d.writes)
        if (w.image == life.source) life.reader_writes_source = true;
      if (life.source_changed) life.read_after_change = true;
    }
    for (const BiasLifeWrite& w : d.writes) {
      // Other images whose tiles this write covers lose their chain (they would import these tiles).
      for (auto& [image, c] : chains_)
        if (image != w.image && c.known && BiasLifeOverlap(c.span, w.span)) c.broken_other = true;
      Chain& c = chains_[w.image];
      if (c.known && w.writes_before != kBiasLifeUnknown && c.writes_after != w.writes_before)
        c.broken_other = true;
      const bool full = w.direct && w.full_mask && w.no_kill && w.has_proven;
      if (full) {
        c.started = true;
        c.broken_blend = false;
        c.broken_other = false;
        c.length = 1;
        c.start_rect = w.proven;
        c.start_site = site;
        c.start_reads.clear();
        for (const BiasLifeRead& r : d.reads)
          if (const auto l = lives_.find(r.address); l != lives_.end() && l->second.active)
            c.start_reads.push_back({r.address, l->second.serial});
      } else if (!w.direct) {
        c.broken_blend = true;
      } else if (c.started) {
        ++c.length;
      }
      c.last_site = site;
      c.writes_after = w.writes_after;
      c.span = w.span;
      c.known = true;
      Touch(w.image, w.span, w.base, full ? &w.proven : nullptr);
    }
    for (const BiasLifeSpan& s : d.depth_writes) {
      for (auto& [image, c] : chains_)
        if (c.known && BiasLifeOverlap(c.span, s)) c.broken_other = true;
      Touch(0, s, UINT32_MAX, nullptr);
    }
  }

  // A clear of `span` through the view `image` at `base` (a resolve with clear, a redirected clear): `rect` is the
  // cleared rectangle when the clear covers it with whole tiles, nullptr if not known.
  void Clear(uint64_t image, const BiasLifeSpan& span, uint32_t base, const BiasLifeRect* rect) {
    for (auto& [chain_image, c] : chains_)
      if (c.known && BiasLifeOverlap(c.span, span)) c.broken_other = true;
    Touch(image, span, base, rect);
  }

  // Tiles were imported into `image` (a resolve's sync): it changed without a draw.
  void Imported(uint64_t image) {
    const auto it = chains_.find(image);
    if (it != chains_.end()) it->second.broken_other = true;
    for (auto& [address, life] : lives_)
      if (life.active && life.source == image) life.source_changed = true;
  }

  // A bias resolve of `rect` (tiles `span` of the view at `base`) from `source` (write count `source_writes`
  // right before the resolve, after its sync) into texture `address`. Ends the previous life of the address
  // first. Returns the producer class.
  uint32_t Resolve(uint64_t source, uint64_t source_writes, uint32_t base, const BiasLifeSpan& span,
                   const BiasLifeRect& rect, uint32_t address, bool full_cover) {
    uint32_t producer = kBiasProducerNoStart;
    uint64_t site = 0;
    const auto it = chains_.find(source);
    if (it != chains_.end() && it->second.known) {
      const Chain& c = it->second;
      site = c.started ? c.start_site : c.last_site;
      if (c.broken_other || c.writes_after != source_writes) producer = kBiasProducerOther;
      else if (c.broken_blend) producer = kBiasProducerBlended;
      else if (!c.started) producer = kBiasProducerNoStart;
      else if (!BiasLifeContains(c.start_rect, rect)) producer = kBiasProducerSmall;
      else producer = c.length == 1 ? kBiasProducerStrict : kBiasProducerChain;
      // A round trip: the start alone produced the source and sampled this very texture's current content (e.g. a
      // restore of the scene from it); if it only scaled it by the inverse bias, the resolve writes back what the
      // texture already holds.
      const auto l = lives_.find(address);
      if (producer == kBiasProducerStrict && l != lives_.end() && l->second.active &&
          std::find(c.start_reads.begin(), c.start_reads.end(), std::make_pair(address, l->second.serial)) !=
              c.start_reads.end())
        ++counts_.round_trip;
    }
    Retire(address);
    ++counts_.resolves;
    counts_.full_cover += full_cover;
    ++counts_.producer[producer];
    ++producer_sites_[site][producer];
    Life& life = lives_[address];
    life = Life{};
    life.serial = ++serial_;
    life.active = true;
    life.source = source;
    life.span = span;
    life.base = base;
    life.rect = rect;
    life.source_writes = source_writes;
    life.producer = producer;
    life.producer_site = site;
    return producer;
  }

  // Any other resolve or copy into `address` ends its life. Returns the end class, or kBiasEndCount if none.
  uint32_t Retire(uint32_t address) {
    const auto it = lives_.find(address);
    if (it == lives_.end() || !it->second.active) return kBiasEndCount;
    Life& life = it->second;
    life.active = false;
    uint32_t end = kBiasEndHandOff;
    if (!life.readers) end = kBiasEndUnread;
    else if (life.reader_writes_source) end = kBiasEndCopyWriter;
    else if (life.read_after_change) end = kBiasEndCopyChanged;
    ++counts_.end[end];
    if (end == kBiasEndHandOff && life.all_point) ++counts_.hand_off_point;
    if (life.touched) (life.dead ? ++counts_.dead : ++counts_.not_dead);
    if (life.touched && life.dead &&
        (life.producer == kBiasProducerStrict || life.producer == kBiasProducerChain))
      ++counts_.dual_and_dead;
    counts_.reads += life.readers;
    counts_.reads_exp_adjust += life.exp_adjust;
    for (uint32_t ps : life.reader_ps) ++reader_ps_[ps][end];
    return end;
  }

  // The image is gone or replaced: forget its chain.
  void ForgetImage(uint64_t image) { chains_.erase(image); }

  const BiasLifeCounts& Counts() const { return counts_; }
  const std::map<uint64_t, std::array<uint64_t, kBiasProducerCount>>& ProducerSites() const {
    return producer_sites_;
  }
  const std::map<uint32_t, std::array<uint64_t, kBiasEndCount>>& ReaderShaders() const { return reader_ps_; }
  void ClearCounts() {
    counts_ = {};
    producer_sites_.clear();
    reader_ps_.clear();
  }

 private:
  // A write or clear of `span` through the view `image`: a write of the source image itself changes it, and the
  // first write of the source tiles through any view after a resolve decides whether its content is dead.
  void Touch(uint64_t image, const BiasLifeSpan& span, uint32_t base, const BiasLifeRect* full_rect) {
    for (auto& [address, life] : lives_) {
      if (!life.active) continue;
      if (image && image == life.source) life.source_changed = true;
      if (!BiasLifeOverlap(span, life.span) || life.touched) continue;
      life.touched = true;
      life.dead = full_rect && base == life.base && BiasLifeContains(*full_rect, life.rect);
    }
  }

  std::unordered_map<uint64_t, Chain> chains_;
  std::unordered_map<uint32_t, Life> lives_;
  uint64_t serial_ = 0;
  BiasLifeCounts counts_;
  std::map<uint64_t, std::array<uint64_t, kBiasProducerCount>> producer_sites_;
  std::map<uint32_t, std::array<uint64_t, kBiasEndCount>> reader_ps_;
};

}  // namespace me::native

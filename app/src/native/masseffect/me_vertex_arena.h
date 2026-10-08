// masseffect - cross-frame vertex arena of the native renderer (docs/zero-copy-vertices.md, "Cross-frame vertex arena").
//
// The ring copies every vertex binding range [vmin..vmax] from guest memory into the per-submission upload buffer,
// byte-swapped (CopyVertices). The in-frame dedupe removes the repeats within one frame; what is still copied is
// mostly equal to an earlier frame (masseffect_native_diag_vertices_repeated: 99.0-99.6 % of the bytes are repeats).
// This header holds the CPU logic of two switches, with no Vulkan, no hashing and no logging, so the tests in
// tests/cpu can drive it alone:
//
//  - Measure (masseffect_native_vertex_arena_measure): per copied range (guest address, bytes, byte order, stride),
//    the content hash and the coherency stamp (me_texture_coherency.h) of its last copy. The next copy of the same
//    range is classified: "no coherency event since the last copy" (clean) or not (dirty) x content same / changed.
//    clean+CHANGED is a write the events did not declare: the arena must never trust the events for such a range.
//  - Arena (masseffect_native_vertex_arena): a persistent buffer holding the swapped copy of ranges seen twice with the
//    same content. A later draw (any frame) of the same range binds the arena copy instead of copying again when the
//    range's pages saw no coherency event since the copy (and, optionally, a sampled fingerprint is still equal).
//
// Arena lifetime (never overwrite what a frame on the GPU may read). The arena is split into kSegments equal segments
// filled in FIFO order. Every use of an entry (its store and every hit) records, per work slot, the newest frame that
// used the segment. A segment is recycled (all its entries dropped, by bumping its epoch) only when, for every slot,
// the last frame of that slot that used it has completed. The client calls SlotStarted(slot, frame) when a slot starts
// recording a new frame, which it does only after waiting for that slot's previous submission, so at that moment the
// previous frame of that slot is complete (same rule as draw_cache::IndexArena). An entry that is still hot when its
// segment comes round again is dropped and stored again on its next use: one copy per trip round the arena, an
// approximate LRU without moving anything the GPU may be reading.
//
// Draining. Hot entries are hit every frame, so a segment holding one would never become reusable. Once the current
// segment is half full, the next one in line stops serving hits (Live() is false there): within a few frames every
// frame that read it has completed and it can be recycled when the current one is full. Its hot ranges are copied
// again (into the current segment) on their next use.
//
// Exemptions. Pages (16 KB, the coherency table's) where a range changed with no coherency event are marked for the
// rest of the session: no range touching them is stored or served again (dynamic buffers that the game rewrites
// without declaring it: UI, particles, Bink planes if they ever reach a vertex fetch).
//
// Ring thread only. Nothing here allocates after Configure (Measure uses hash maps: measurement only).
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace me::native::vertex_arena {

inline constexpr uint64_t kPhysicalBytes = 0x20000000ull;  // 512 MB
inline constexpr uint32_t kPageShift = 14;                 // the coherency table's 16 KB pages
inline constexpr uint32_t kPages = uint32_t(kPhysicalBytes >> kPageShift);

inline uint64_t Mix(uint64_t h, uint64_t v) {
  h = (h ^ v) * 0x9E3779B97F4A7C15ull;
  return h ^ (h >> 29);
}

// The identity of a copied range. The bytes depend only on address, size and byte order; the stride is part of the
// key so that the report can tell meshes apart (the same bytes fetched with two strides are two keys).
struct RangeKey {
  uint32_t address = 0;  // physical, of the first byte used
  uint32_t bytes = 0;
  uint32_t stride = 0;
  uint32_t order = 0;  // xenos::Endian
  bool operator==(const RangeKey& o) const {
    return address == o.address && bytes == o.bytes && stride == o.stride && order == o.order;
  }
  uint64_t Tag() const { return Mix(Mix(Mix(address, bytes), stride), order); }
};
struct RangeKeyHash {
  size_t operator()(const RangeKey& k) const { return size_t(k.Tag()); }
};

// --- Measurement -----------------------------------------------------------------------------------------------------
class Measure {
 public:
  enum Result : uint32_t { kFirst = 0, kCleanSame, kCleanChanged, kDirtySame, kDirtyChanged, kResults };
  struct Counts {
    std::array<uint64_t, kResults> n{};
    std::array<uint64_t, kResults> bytes{};
    uint64_t table_resets = 0;
  };
  struct Offender {
    RangeKey key;
    uint64_t count = 0;  // clean+CHANGED copies in the window
    uint64_t bytes = 0;
  };

  explicit Measure(size_t max_ranges = size_t(1) << 17) : max_ranges_(max_ranges) {}

  // One copy of `key` whose content hash is `hash`, with `stamp` = the coherency stamp taken BEFORE the bytes were
  // read. clean(start, length, stamp) answers "no page of the range marked after stamp" (texture_coherency::Clean).
  template <class CleanFn>
  Result Note(const RangeKey& key, uint64_t hash, uint32_t stamp, CleanFn&& clean) {
    Result r = kFirst;
    auto it = last_.find(key);
    if (it != last_.end()) {
      const bool is_clean = clean(uint64_t(key.address), uint64_t(key.bytes), it->second.stamp);
      const bool same = it->second.hash == hash;
      r = is_clean ? (same ? kCleanSame : kCleanChanged) : (same ? kDirtySame : kDirtyChanged);
      it->second = {hash, stamp};
    } else {
      if (last_.size() >= max_ranges_) {
        last_.clear();  // bounded memory: start over (the next copies of every range count as first seen)
        ++counts_.table_resets;
      }
      last_.emplace(key, Last{hash, stamp});
    }
    ++counts_.n[r];
    counts_.bytes[r] += key.bytes;
    if (r == kCleanChanged) {
      Offender& o = window_[key];
      o.key = key;
      ++o.count;
      o.bytes += key.bytes;
    }
    return r;
  }

  const Counts& counts() const { return counts_; }
  size_t tracked() const { return last_.size(); }

  // The `n` ranges with the most clean+CHANGED copies since the last call (most first; ties: more bytes first), and
  // the number of distinct such ranges. Clears the window.
  std::vector<Offender> TakeTop(size_t n, size_t& distinct) {
    std::vector<Offender> all;
    all.reserve(window_.size());
    for (const auto& [k, o] : window_) all.push_back(o);
    distinct = all.size();
    window_.clear();
    const size_t m = std::min(n, all.size());
    std::partial_sort(all.begin(), all.begin() + m, all.end(), [](const Offender& a, const Offender& b) {
      if (a.count != b.count) return a.count > b.count;
      if (a.bytes != b.bytes) return a.bytes > b.bytes;
      return a.key.address < b.key.address;
    });
    all.resize(m);
    return all;
  }

 private:
  struct Last {
    uint64_t hash = 0;
    uint32_t stamp = 0;
  };
  size_t max_ranges_;
  std::unordered_map<RangeKey, Last, RangeKeyHash> last_;
  std::unordered_map<RangeKey, Offender, RangeKeyHash> window_;
  Counts counts_;
};

// --- Exempt pages ----------------------------------------------------------------------------------------------------
class ExemptPages {
 public:
  ExemptPages() : bits_(kPages / 64, 0) {}
  // Marks every page [start, start + length) touches. Returns how many pages were newly marked.
  uint32_t Mark(uint64_t start, uint64_t length) {
    if (!length) return 0;
    uint32_t added = 0;
    ForPages(start, length, [&](uint32_t p) {
      uint64_t& w = bits_[p >> 6];
      const uint64_t bit = uint64_t(1) << (p & 63);
      if (!(w & bit)) {
        w |= bit;
        ++added;
      }
      return true;
    });
    marked_ += added;
    return added;
  }
  bool Any(uint64_t start, uint64_t length) const {
    if (!marked_ || !length) return false;
    bool hit = false;
    ForPages(start, length, [&](uint32_t p) {
      hit = (bits_[p >> 6] >> (p & 63)) & 1;
      return !hit;
    });
    return hit;
  }
  uint64_t marked() const { return marked_; }

 private:
  template <class Fn>
  static void ForPages(uint64_t start, uint64_t length, Fn&& fn) {
    start &= kPhysicalBytes - 1;
    uint64_t end = start + length;
    if (end > kPhysicalBytes) end = kPhysicalBytes;
    for (uint64_t p = start >> kPageShift; p <= (end - 1) >> kPageShift; ++p) {
      if (!fn(uint32_t(p))) return;
    }
  }
  std::vector<uint64_t> bits_;
  uint64_t marked_ = 0;
};

// --- Arena bookkeeping -----------------------------------------------------------------------------------------------
class Arena {
 public:
  static constexpr uint32_t kMaxSlots = 8;
  static constexpr uint32_t kSegments = 8;
  enum State : uint8_t { kEmpty = 0, kCandidate = 1, kStored = 2 };
  struct Entry {
    RangeKey key;
    uint8_t state = kEmpty;
    uint64_t fingerprint = 0;  // candidate: the content seen last; stored: the content when it was stored
    uint64_t full = 0;         // stored: full hash of the guest bytes right before the copy (0 = not taken)
    uint32_t stamp = 0;        // stored: coherency stamp taken before the copy read the guest bytes
    uint32_t offset = 0;       // stored: byte offset in the arena buffer
    uint32_t segment = 0;
    uint32_t epoch = 0;        // the segment's epoch when stored
    uint64_t hits = 0;         // since stored
  };

  // capacity in bytes (split in kSegments), table_slots a power of two.
  void Configure(uint64_t capacity, size_t table_slots) {
    table_.assign(table_slots, Entry{});
    mask_ = table_slots ? table_slots - 1 : 0;
    segment_bytes_ = (capacity / kSegments) & ~uint64_t(255);
    usable_ = segment_bytes_ > 0 && table_slots && (table_slots & mask_) == 0;
    current_ = 0;
    used_ = 0;
    draining_ = kSegments;  // none
    epoch_.fill(1);
    use_.fill({});
    last_started_.fill(0);
    completed_.fill(0);
  }
  bool usable() const { return usable_; }
  uint64_t capacity() const { return segment_bytes_ * kSegments; }
  uint64_t segment_bytes() const { return segment_bytes_; }
  // Ranges larger than a segment are never stored.
  uint64_t max_bytes() const { return segment_bytes_; }

  // The table slot of `key` (direct mapped). It may hold another key (check Same) or be empty.
  Entry& Slot(const RangeKey& key) { return table_[size_t(key.Tag() >> 7) & mask_]; }
  static bool Same(const Entry& e, const RangeKey& key) { return e.state != kEmpty && e.key == key; }
  // A stored entry whose segment has not been recycled since.
  // Not in the segment being drained for reuse.
  bool Live(const Entry& e) const {
    return e.state == kStored && epoch_[e.segment] == e.epoch && e.segment != draining_;
  }

  // Space for `bytes` (aligned) in the current segment, moving to the next one when it is full. False when the next
  // segment may still be read by a frame on the GPU (the caller copies into the upload buffer as before) or when the
  // range is larger than a segment.
  bool Allocate(uint32_t bytes, uint32_t alignment, uint32_t& offset, uint32_t& segment, uint32_t& epoch) {
    if (!usable_ || !bytes || bytes > segment_bytes_) return false;
    alignment = std::max<uint32_t>(alignment, 4);
    uint64_t at = (used_ + alignment - 1) / alignment * alignment;
    if (at + bytes > segment_bytes_) {
      const uint32_t next = (current_ + 1) % kSegments;
      if (!Reusable(next)) {
        ++waits_;
        return false;
      }
      Recycle(next);
      current_ = next;
      used_ = 0;
      at = 0;
      draining_ = kSegments;
    }
    const uint64_t absolute = uint64_t(current_) * segment_bytes_ + at;
    offset = uint32_t(absolute);
    segment = current_;
    epoch = epoch_[current_];
    used_ = at + bytes;
    Use(current_);
    if (used_ * 2 >= segment_bytes_) draining_ = (current_ + 1) % kSegments;
    dirty_lo_ = std::min(dirty_lo_, absolute);
    dirty_hi_ = std::max(dirty_hi_, absolute + bytes);
    return true;
  }

  // Records that the current frame reads (or wrote) segment `s`.
  void Use(uint32_t s) { use_[s][slot_] = std::max(use_[s][slot_], frame_ + 1); }

  // A work slot starts recording `frame`, after its previous submission completed.
  void SlotStarted(uint32_t slot, uint64_t frame) {
    slot %= kMaxSlots;
    completed_[slot] = last_started_[slot];
    last_started_[slot] = frame + 1;  // +1: 0 = no frame
    slot_ = slot;
    frame_ = frame;
  }

  // Drops every entry (the space is reused segment by segment under the same GPU rule).
  void DropAll() {
    for (uint32_t s = 0; s < kSegments; ++s) BumpEpoch(s);
  }

  // The range written since the last call (for a flush of non-coherent memory). False if nothing was written.
  bool TakeDirty(uint64_t& lo, uint64_t& hi) {
    if (dirty_hi_ <= dirty_lo_) return false;
    lo = dirty_lo_;
    hi = dirty_hi_;
    dirty_lo_ = UINT64_MAX;
    dirty_hi_ = 0;
    return true;
  }

  // Bytes holding copies (for the report): the current segment's fill plus the other segments already filled once.
  uint64_t used() const { return used_ + uint64_t(filled_) * segment_bytes_; }
  uint64_t recycles() const { return recycles_; }
  uint64_t waits() const { return waits_; }

  // For tests: true if segment s may be overwritten now.
  bool Reusable(uint32_t s) const {
    for (uint32_t k = 0; k < kMaxSlots; ++k) {
      if (completed_[k] < use_[s][k]) return false;
    }
    return true;
  }

 private:
  void BumpEpoch(uint32_t s) {
    ++epoch_[s];
    if (!epoch_[s]) epoch_[s] = 1;
  }
  void Recycle(uint32_t s) {
    BumpEpoch(s);
    use_[s].fill(0);
    ++recycles_;
    filled_ = std::min<uint32_t>(filled_ + 1, kSegments - 1);
  }

  std::vector<Entry> table_;
  size_t mask_ = 0;
  uint64_t segment_bytes_ = 0;
  bool usable_ = false;
  uint32_t current_ = 0;
  uint32_t draining_ = kSegments;  // the segment that serves no hits until it is recycled (kSegments = none)
  uint64_t used_ = 0;  // in the current segment
  uint32_t filled_ = 0;
  std::array<uint32_t, kSegments> epoch_{};
  std::array<std::array<uint64_t, kMaxSlots>, kSegments> use_{};  // per segment and slot: newest frame + 1 using it
  std::array<uint64_t, kMaxSlots> last_started_{};
  std::array<uint64_t, kMaxSlots> completed_{};
  uint32_t slot_ = 0;
  uint64_t frame_ = 0;
  uint64_t dirty_lo_ = UINT64_MAX, dirty_hi_ = 0;
  uint64_t recycles_ = 0, waits_ = 0;
};

}  // namespace me::native::vertex_arena

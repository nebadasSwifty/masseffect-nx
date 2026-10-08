// masseffect - per-component draw cache of the native renderer (docs/frame-coherence.md, "Per-component cache").
//
// The frame coherence measurement found no draw equal to a draw of the previous frame as a whole (the shader constants
// change every frame), but the components repeat: shaders 99.7 %, render state 98.9 %, texture fetch words 100 %,
// geometry content 98.5 %. This header holds the logic of the caches that reuse those components, with no Vulkan and
// no logging, so the tests in tests/cpu can drive it alone:
//
//  - TextureSets: the texture bindings of a draw's pixel shader, keyed by the shader's sampler registers and the six
//    fetch words of each. The value is the client's per-binding record (slot, heap and sampler indices, host size, and
//    the invalidation fields of the cache it came from); the client checks every binding with the rule of its own
//    sampler caches, so the set never outlives the bindings it was made of.
//  - PipelineMemo: raw pipeline key (plus the lookup context) -> pipeline, skipping the lookup key derivation
//    (canonical form, dynamic-state masking) and the hash map / direct-mapped cache of the pipelines.
//  - IndexArena: 16-bit index ranges converted once and kept in a persistent buffer across frames, keyed by
//    (address, byte order, count, full fingerprint of the guest bytes). The space is only reused after every frame
//    that may read it has completed on the GPU (per work slot completion).
//  - Verifier: the self-check schedule (the first N hits, then 1 in K) and the switch-off on the first difference.
//
// Ring thread only. Nothing here allocates after Configure.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace me::native::draw_cache {

inline uint64_t Mix(uint64_t h, uint64_t v) {
  h = (h ^ v) * 0x9E3779B97F4A7C15ull;
  return h ^ (h >> 29);
}

// Self-check of one component: the first `first` hits and then 1 in `every` (0: none after the first ones) also run
// the slow path, and the caller compares. A difference switches the component off for the rest of the session.
class Verifier {
 public:
  void Configure(bool on, uint64_t first, uint64_t every) {
    on_ = on;
    first_ = first;
    every_ = every;
  }
  bool on() const { return on_; }
  // Called on every hit the cache would take: true = check this one.
  bool Due() {
    ++hits_;
    return hits_ <= first_ || (every_ && hits_ % every_ == 0);
  }
  // The comparison of a checked hit. Returns `equal`.
  bool Result(bool equal) {
    ++checked_;
    if (!equal) {
      ++differences_;
      on_ = false;
    }
    return equal;
  }
  // True once, when the first `first` checks are done (for the one-time log line).
  bool FirstChecksDone() {
    if (first_reported_ || !first_ || checked_ < first_) return false;
    first_reported_ = true;
    return true;
  }
  uint64_t hits() const { return hits_; }
  uint64_t checked() const { return checked_; }
  uint64_t differences() const { return differences_; }
  uint64_t first() const { return first_; }

 private:
  bool on_ = false;
  bool first_reported_ = false;
  uint64_t first_ = 0, every_ = 0;
  uint64_t hits_ = 0, checked_ = 0, differences_ = 0;
};

// Texture bindings of one pixel shader's sampler list. Binding must have a `std::array<uint32_t, 6> fetch` member
// holding the fetch words it was computed for.
template <class Binding, size_t kMaxBindings, size_t kSlots>
class TextureSets {
  static_assert(kSlots && (kSlots & (kSlots - 1)) == 0, "kSlots must be a power of two");
  static_assert(kMaxBindings <= 255);

 public:
  struct Key {
    uint32_t shader = 0;  // client identity of the sampler list (non-zero)
    uint32_t n = 0;
    uint64_t tag = 0;
    std::array<uint8_t, kMaxBindings> registers{};
    std::array<const uint32_t*, kMaxBindings> fetch{};
  };
  struct Entry {
    uint64_t tag = 0;
    uint32_t shader = 0;  // 0 = empty
    uint32_t n = 0;
    std::array<uint8_t, kMaxBindings> registers{};
    std::array<Binding, kMaxBindings> bindings{};
  };

  // The key of a list of `n` registers whose fetch constants start at fetch_base (6 words per register). False if
  // the list is too long for an entry (the caller takes the slow path).
  static bool MakeKey(uint32_t shader, const uint8_t* registers, uint32_t n, const uint32_t* fetch_base, Key& key) {
    if (!shader || n > kMaxBindings) return false;
    key.shader = shader;
    key.n = n;
    uint64_t h = Mix(shader, n);
    for (uint32_t i = 0; i < n; ++i) {
      key.registers[i] = registers[i];
      const uint32_t* f = fetch_base + size_t(registers[i]) * 6;
      key.fetch[i] = f;
      h = Mix(h, registers[i]);
      h = Mix(h, (uint64_t(f[0]) << 32) | f[1]);
      h = Mix(h, (uint64_t(f[2]) << 32) | f[3]);
      h = Mix(h, (uint64_t(f[4]) << 32) | f[5]);
    }
    key.tag = h | 1;
    return true;
  }

  // The entry whose shader, registers and fetch words are exactly the key's, or nullptr. `clash` = the slot holds
  // another key (for the report).
  Entry* Find(const Key& key, bool& clash) {
    Entry& e = table_[key.tag & (kSlots - 1)];
    clash = false;
    if (!e.shader) return nullptr;
    if (e.tag != key.tag || e.shader != key.shader || e.n != key.n) {
      clash = true;
      return nullptr;
    }
    for (uint32_t i = 0; i < key.n; ++i) {
      if (e.registers[i] != key.registers[i] ||
          std::memcmp(e.bindings[i].fetch.data(), key.fetch[i], sizeof(uint32_t) * 6) != 0) {
        clash = true;
        return nullptr;
      }
    }
    return &e;
  }

  // Replaces the key's slot. bindings[i] must hold the values the slow path used for register i of the key, with
  // their fetch words equal to the key's (checked: a binding computed for other words is not stored).
  bool Store(const Key& key, const Binding* bindings) {
    for (uint32_t i = 0; i < key.n; ++i) {
      if (std::memcmp(bindings[i].fetch.data(), key.fetch[i], sizeof(uint32_t) * 6) != 0) return false;
    }
    Entry& e = table_[key.tag & (kSlots - 1)];
    e.shader = 0;
    e.tag = key.tag;
    e.n = key.n;
    for (uint32_t i = 0; i < key.n; ++i) {
      e.registers[i] = key.registers[i];
      e.bindings[i] = bindings[i];
    }
    e.shader = key.shader;
    return true;
  }

  void Clear() {
    for (Entry& e : table_) e.shader = 0;
  }

 private:
  std::array<Entry, kSlots> table_{};
};

// Raw pipeline key + lookup context -> pipeline. Key must have a unique object representation (it is compared and
// hashed as bytes). A one-entry shortcut for consecutive draws with the same key, then a direct-mapped table.
template <class Key, class Value, size_t kSlots>
class PipelineMemo {
  static_assert(std::has_unique_object_representations_v<Key>, "the key is compared byte by byte");
  static_assert(sizeof(Key) % 4 == 0);
  static_assert(kSlots && (kSlots & (kSlots - 1)) == 0, "kSlots must be a power of two");

 public:
  static uint64_t Tag(const Key& key, uint32_t context) {
    uint32_t words[sizeof(Key) / 4];
    std::memcpy(words, &key, sizeof(Key));
    uint64_t h = Mix(0x51ED270B27u, context);
    for (size_t i = 0; i + 1 < sizeof(Key) / 4; i += 2) h = Mix(h, (uint64_t(words[i]) << 32) | words[i + 1]);
    if ((sizeof(Key) / 4) & 1) h = Mix(h, words[sizeof(Key) / 4 - 1]);
    return h;
  }
  // nullptr on a miss. `clash`: the slot holds another key.
  const Value* Find(const Key& key, uint32_t context, bool& clash) {
    clash = false;
    if (last_valid_ && last_context_ == context && std::memcmp(&last_key_, &key, sizeof(Key)) == 0) {
      ++last_hits_;
      return &last_value_;
    }
    const Entry& e = table_[Tag(key, context) & (kSlots - 1)];
    if (!e.valid) return nullptr;
    if (e.context != context || std::memcmp(&e.key, &key, sizeof(Key)) != 0) {
      clash = true;
      return nullptr;
    }
    last_key_ = key;
    last_context_ = context;
    last_value_ = e.value;
    last_valid_ = true;
    return &e.value;
  }
  void Store(const Key& key, uint32_t context, const Value& value) {
    Entry& e = table_[Tag(key, context) & (kSlots - 1)];
    e.key = key;
    e.context = context;
    e.value = value;
    e.valid = 1;
    last_key_ = key;
    last_context_ = context;
    last_value_ = value;
    last_valid_ = true;
  }
  void Clear() {
    for (Entry& e : table_) e.valid = 0;
    last_valid_ = false;
  }
  uint64_t last_hits() const { return last_hits_; }

 private:
  struct Entry {
    Key key{};
    uint32_t context = 0;
    uint32_t valid = 0;
    Value value{};
  };
  std::array<Entry, kSlots> table_{};
  Key last_key_{};
  uint32_t last_context_ = 0;
  bool last_valid_ = false;
  Value last_value_{};
  uint64_t last_hits_ = 0;
};

// Persistent arena of converted 16-bit index ranges (CPU bookkeeping only; the client owns the buffer).
//
// Lifetime. The GPU may read a range in every frame that drew with it. The arena is never overwritten in place: it
// fills up front to back, and when a range does not fit, Reset() drops every entry (no hit from then on) and the
// space is reused only after every frame that may have read it has completed. Frames run in work slots; the client
// calls SlotStarted(slot, frame) when a slot starts recording a new frame, which it does only after waiting for that
// slot's previous submission. So at that moment the previous frame of that slot is complete; the arena is free again
// once, for every slot, the last frame started before the reset is complete.
class IndexArena {
 public:
  static constexpr uint32_t kMaxSlots = 8;
  struct Entry {
    uint64_t key = 0;
    uint64_t fingerprint = 0;
    uint32_t count = 0;
    uint32_t vmin = 0;
    uint32_t vmax = 0;
    uint32_t offset = 0;
    uint32_t epoch = 0;  // 0 = empty
  };

  // capacity in bytes; table_slots a power of two.
  void Configure(uint64_t capacity, size_t table_slots) {
    capacity_ = capacity;
    table_.assign(table_slots, Entry{});
    mask_ = table_slots - 1;
    epoch_ = 1;
    used_ = 0;
    usable_ = capacity > 0 && table_slots && (table_slots & mask_) == 0;
    seen_.assign(table_slots * 2, 0);
  }
  bool usable() const { return usable_; }
  uint64_t capacity() const { return capacity_; }
  uint64_t used() const { return used_; }

  static uint64_t SlotOf(uint64_t key, uint32_t count) { return Mix(key, count); }

  // A live entry of exactly this range, or nullptr. fingerprint 0 never matches (the caller has no full fingerprint).
  const Entry* Find(uint64_t key, uint32_t count, uint64_t fingerprint) const {
    if (!usable_ || !fingerprint) return nullptr;
    const Entry& e = table_[SlotOf(key, count) & mask_];
    if (e.epoch != epoch_ || e.key != key || e.count != count || e.fingerprint != fingerprint) return nullptr;
    return &e;
  }

  // Promotion: true the second time the same content (key, count, fingerprint) is seen, so ranges seen once (dynamic
  // buffers rewritten every frame) do not fill the arena. Only a filter: a false "seen" (tag collision) just stores a
  // range earlier.
  bool SeenBefore(uint64_t key, uint32_t count, uint64_t fingerprint) {
    if (seen_.empty()) return true;
    const uint64_t tag = Mix(Mix(key, count), fingerprint) | 1;
    uint64_t& cell = seen_[(tag >> 7) & (seen_.size() - 1)];
    if (cell == tag) return true;
    cell = tag;
    return false;
  }

  // Space for `bytes` (aligned). False while the arena waits for the GPU, or when it is full (then it starts a reset:
  // every entry is dropped and the space waits for the frames that may read it).
  bool Allocate(uint32_t bytes, uint32_t alignment, uint32_t& offset) {
    if (!usable_ || !bytes) return false;
    const uint64_t at = (used_ + alignment - 1) / alignment * alignment;
    if (at + bytes > capacity_) {
      Reset();
      return false;
    }
    offset = uint32_t(at);
    used_ = at + bytes;
    dirty_lo_ = std::min(dirty_lo_, at);
    dirty_hi_ = std::max(dirty_hi_, used_);
    return true;
  }

  void Insert(uint64_t key, uint32_t count, uint64_t fingerprint, uint32_t vmin, uint32_t vmax, uint32_t offset) {
    if (!usable_ || !fingerprint) return;
    Entry& e = table_[SlotOf(key, count) & mask_];
    e = {key, fingerprint, count, vmin, vmax, offset, epoch_};
    ++inserted_;
  }

  // Drops every entry; the space is reused once every frame started so far has completed on the GPU.
  void Reset() {
    ++epoch_;
    if (!epoch_) epoch_ = 1;
    usable_ = false;
    wait_ = last_started_;
    ++resets_;
    TryRelease();
  }

  // A work slot starts recording `frame`, after its previous submission completed.
  void SlotStarted(uint32_t slot, uint64_t frame) {
    slot %= kMaxSlots;
    completed_[slot] = last_started_[slot];
    last_started_[slot] = frame + 1;  // +1: 0 = no frame
    if (!usable_ && capacity_) TryRelease();
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

  uint64_t resets() const { return resets_; }
  uint64_t releases() const { return releases_; }
  uint64_t inserted() const { return inserted_; }

 private:
  void TryRelease() {
    for (uint32_t k = 0; k < kMaxSlots; ++k) {
      if (completed_[k] < wait_[k]) return;
    }
    used_ = 0;
    usable_ = capacity_ > 0;
    ++releases_;
  }

  uint64_t capacity_ = 0;
  std::vector<Entry> table_;
  std::vector<uint64_t> seen_;
  size_t mask_ = 0;
  uint32_t epoch_ = 1;
  uint64_t used_ = 0;
  bool usable_ = false;
  std::array<uint64_t, kMaxSlots> last_started_{};
  std::array<uint64_t, kMaxSlots> completed_{};
  std::array<uint64_t, kMaxSlots> wait_{};
  uint64_t dirty_lo_ = UINT64_MAX, dirty_hi_ = 0;
  uint64_t resets_ = 0, releases_ = 0, inserted_ = 0;
};

// Time and counts of one component (us per timed draw on the hit and the slow path, for the report).
struct Timing {
  uint64_t ns_hit = 0, n_hit = 0, ns_slow = 0, n_slow = 0;
  void Add(bool hit, uint64_t ns) {
    if (hit) {
      ns_hit += ns;
      ++n_hit;
    } else {
      ns_slow += ns;
      ++n_slow;
    }
  }
};

}  // namespace me::native::draw_cache

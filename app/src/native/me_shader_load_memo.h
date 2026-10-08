// Native renderer, ring thread: shader IM_LOAD memo by guest address (masseffect_native_load_memo).
//
// Why. In the Normandy walk windows the ring sees ~860 shader IM_LOADs per frame (about 1.15 per draw). With
// masseffect_native_raw_microcode the reload of the stage's current program is one memcmp, but ~40 % of the loads
// switch back to a program loaded a few draws earlier (identity memo hits, 6-8k per second). Each of those
// byte-swaps the whole microcode (up to 966 words), hashes it with XXH3 and looks the hash up in the identity memo,
// only to find the entry it found last time.
//
// What. A small direct-mapped table keyed by (stage, guest address, word count). A slot keeps the raw guest words
// of the last load seen there, their byte-swapped copy and the XXH3 of the swapped words (the identity memo hash).
// A new load from the same address with the same size whose raw words compare equal (one memcmp) has, word for
// word, the same swapped microcode (the swap is a bijection) and therefore the same XXH3: the caller can skip the
// swap loop and the hash and go straight to the identity-memo lookup with the stored hash.
//
// Exactness. Find() only answers when the bytes are equal, so everything derived from the bytes is equal. The
// caller still looks the hash up in the identity memo (which may have been cleared or may not hold it) and falls
// back to the full path if it is not there. Single thread (the ring thread).
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace me::native {

class ShaderLoadMemo {
 public:
  static constexpr uint32_t kSlots = 1024;  // power of two; ~450 shader objects in Mass Effect
  static constexpr uint32_t kMaxWords = 4096;

  struct Slot {
    uint32_t address = 0;
    uint32_t words = 0;
    uint32_t type = 0;
    bool valid = false;
    uint64_t memo_hash = 0;
    uint64_t generation = 0;        // the stage's identity generation right after this load (caller-defined)
    std::vector<uint32_t> raw;      // guest words as read (big-endian)
    std::vector<uint32_t> swapped;  // host-order microcode, as IdentifyShader saw it
  };

  // The slot whose last load has the same stage, address, size and raw words as `src`, or nullptr.
  const Slot* Find(uint32_t type, uint32_t address, const void* src, uint32_t words) {
    ++lookups_;
    if (!address || !words || words > kMaxWords) return nullptr;
    const Slot& s = slots_[Index(type, address, words)];
    if (!s.valid || s.address != address || s.words != words || s.type != type) return nullptr;
    if (std::memcmp(s.raw.data(), src, size_t(words) * sizeof(uint32_t)) != 0) {
      ++changed_;
      return nullptr;
    }
    ++hits_;
    return &s;
  }

  // Records a load: the swapped microcode IdentifyShader used and its XXH3. The raw words are derived from the
  // swapped ones (not read from guest memory again), so raw and swapped always describe the same program even if
  // the guest rewrote the memory in between.
  void Store(uint32_t type, uint32_t address, const std::vector<uint32_t>& swapped, uint64_t memo_hash,
             uint64_t generation = 0) {
    const uint32_t words = uint32_t(swapped.size());
    if (!address || !words || words > kMaxWords) return;
    Slot& s = slots_[Index(type, address, words)];
    s.address = address;
    s.words = words;
    s.type = type;
    s.memo_hash = memo_hash;
    s.generation = generation;
    s.raw.resize(words);
    for (uint32_t i = 0; i < words; ++i) s.raw[i] = __builtin_bswap32(swapped[i]);
    s.swapped = swapped;  // reuses the slot's capacity after the first fill
    s.valid = true;
  }

  void Clear() {
    for (auto& s : slots_) s.valid = false;
  }

  uint64_t lookups() const { return lookups_; }
  uint64_t hits() const { return hits_; }
  uint64_t changed() const { return changed_; }

 private:
  static uint32_t Index(uint32_t type, uint32_t address, uint32_t words) {
    uint64_t x = (uint64_t(address) << 2 | type) ^ (uint64_t(words) * 0x9E3779B97F4A7C15ull);
    x ^= x >> 31;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 29;
    return uint32_t(x) & (kSlots - 1);
  }

  Slot slots_[kSlots];
  uint64_t lookups_ = 0, hits_ = 0, changed_ = 0;
};

}  // namespace me::native

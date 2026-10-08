// Mass Effect native renderer: shader constants tracked per float4 vector (masseffect_native_constants_dirty,
// docs/batched-constants.md).
//
// The ring sink (NativeGraphicsSystem) sets one bit per constant vector whose value really changed (VS c0-c255 are
// vectors 0-255, PS c0-c255 are vectors 256-511: vector = (register - 0x4000) / 4). The draws side takes the bits at
// every draw and keeps, per bank, a CPU-cached shadow of what its last upload holds. A draw then needs no full compare
// of the bank with that shadow (masseffect_native_constants_same_content, ~2 % of a core on the ring thread) and no
// full shadow copy after an upload: only the vectors whose bit is set are compared and copied. Exact by construction:
// a vector without its bit set has not changed value since the last upload. Both sides run on the ring thread.
//
// Host test: tests/cpu/test_native_constants_dirty.cpp.
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>

namespace me::native {

constexpr uint32_t kConstantRegisterFirst = 0x4000;  // VS c0.x
constexpr uint32_t kConstantRegisterEnd = 0x4800;    // one past PS c255.w
constexpr uint32_t kConstantBankWords = 0x400;       // 256 vectors x 4 words per bank

// Written by the ring sink, taken by the draws side.
struct ConstantDirtyBits {
  uint64_t bits[8] = {};   // [0..3] VS vectors 0-255, [4..7] PS vectors 0-255
  uint64_t bumps[2] = {};  // generation bumps (VS, PS) made by writes that also set their bits
  // `reg` must lie in [kConstantRegisterFirst, kConstantRegisterEnd).
  void Mark(uint32_t reg) {
    const uint32_t v = (reg - kConstantRegisterFirst) >> 2;
    bits[v >> 6] |= uint64_t(1) << (v & 63);
  }
  // Every vector of the registers [first, end) (clamped to the constant range): the conservative mark.
  void MarkRange(uint32_t first, uint32_t end) {
    first = std::max(first, kConstantRegisterFirst);
    end = std::min(end, kConstantRegisterEnd);
    for (uint32_t r = first; r < end; r = (r | 3) + 1) Mark(r);
  }
};

// One bank (VS or PS) on the draws side. The shadow buffer is the caller's (kConstantBankWords words) so that the
// older content path (ConstantsSame) keeps reading the same copy.
class ConstantBankTracker {
 public:
  // stage 0 = VS, 1 = PS. `generation` is the draw's generation of this bank (SubmissionDraw).
  void Take(ConstantDirtyBits& d, uint32_t stage, uint64_t generation) {
    uint64_t* bits = d.bits + stage * 4;
    for (uint32_t k = 0; k < 4; ++k) {
      pending_[k] |= bits[k];
      bits[k] = 0;
    }
    // A generation bump that did not come with bits (a write path that does not mark) breaks the tracking.
    if (seen_valid_ && generation - seen_generation_ != d.bumps[stage] - seen_bumps_) {
      synced_ = false;
      ++lost_sync_;
    }
    seen_generation_ = generation;
    seen_bumps_ = d.bumps[stage];
    seen_valid_ = true;
  }

  bool synced() const { return synced_; }
  uint32_t shadow_bytes() const { return shadow_bytes_; }
  uint64_t lost_sync() const { return lost_sync_; }
  bool AnyPendingIn(uint32_t bytes) const { return Pending(bytes, nullptr); }

  // The last upload is valid (same upload buffer, it holds at least `bytes`, synced): true if its first `bytes` equal
  // `current`. Only the vectors changed since the upload are compared; those found equal are cleared.
  bool Same(const uint32_t* shadow, const uint32_t* current, uint32_t bytes, uint32_t& compared) {
    uint64_t mask[4];
    if (!Pending(bytes, mask)) return true;
    for (uint32_t k = 0; k < 4; ++k) {
      while (mask[k]) {
        const uint32_t v = k * 64 + uint32_t(std::countr_zero(mask[k]));
        mask[k] &= mask[k] - 1;
        const uint32_t length = std::min<uint32_t>(16, bytes - v * 16);
        ++compared;
        if (std::memcmp(shadow + v * 4, current + v * 4, length) != 0) return false;
        pending_[k] &= ~(uint64_t(1) << (v & 63));
      }
    }
    return true;
  }

  // The upload was reused for `bytes`: from now on only these bytes count.
  void Reused(uint32_t bytes) { shadow_bytes_ = bytes; }

  // A new upload of `current` (`bytes`) while synced: brings the shadow up to date by copying only the changed
  // vectors of the old valid prefix and the part beyond it. Returns the bytes copied.
  uint32_t Uploaded(uint32_t* shadow, const uint32_t* current, uint32_t bytes) {
    const uint32_t old = std::min(shadow_bytes_, bytes);
    uint32_t copied = 0;
    for (uint32_t k = 0; k < 4; ++k) {
      uint64_t m = pending_[k];
      while (m) {
        const uint32_t v = k * 64 + uint32_t(std::countr_zero(m));
        m &= m - 1;
        if (v * 16 >= old) break;  // ascending: the rest is beyond the old prefix (copied below)
        std::memcpy(shadow + v * 4, current + v * 4, 16);
        copied += 16;
      }
      pending_[k] = 0;
    }
    if (bytes > old) {
      std::memcpy(reinterpret_cast<uint8_t*>(shadow) + old, reinterpret_cast<const uint8_t*>(current) + old,
                  bytes - old);
      copied += bytes - old;
    }
    shadow_bytes_ = bytes;
    return copied;
  }

  // An upload made by the older path from the registers (`bytes`): full copy, synced from now on.
  void Resync(uint32_t* shadow, const uint32_t* current, uint32_t bytes) {
    std::memcpy(shadow, current, bytes);
    for (uint64_t& p : pending_) p = 0;
    shadow_bytes_ = bytes;
    synced_ = true;
  }

  // An upload that is not a copy of the registers (patched PS constants) or a failed check.
  void Desync() { synced_ = false; }

 private:
  // The pending bits of the vectors that hold the first `bytes`; false if there is none.
  bool Pending(uint32_t bytes, uint64_t* out) const {
    const uint32_t vectors = std::min<uint32_t>((bytes + 15) / 16, 256);
    uint64_t any = 0;
    for (uint32_t k = 0; k < 4; ++k) {
      const uint32_t from = k * 64;
      uint64_t prefix = 0;
      if (vectors >= from + 64) prefix = ~uint64_t(0);
      else if (vectors > from) prefix = (uint64_t(1) << (vectors - from)) - 1;
      const uint64_t m = pending_[k] & prefix;
      if (out) out[k] = m;
      any |= m;
    }
    return any != 0;
  }

  uint64_t pending_[4] = {};
  uint32_t shadow_bytes_ = 0;
  bool synced_ = false;
  bool seen_valid_ = false;
  uint64_t seen_generation_ = 0, seen_bumps_ = 0;
  uint64_t lost_sync_ = 0;
};

}  // namespace me::native

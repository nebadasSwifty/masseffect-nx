// Ring thread front/back split, steps 0 and 1 of docs/multithread-translation.md (host testable, no SDK headers).
//
// Step 0 (masseffect_native_effects_stats, measurement only): EffectStats counts the guest-visible effects the ring
// produces (fence and memory writes, scratch write-back, interrupts, occlusion writes, read-pointer write-back) and the
// WAIT_REG_MEM sync points, and how many ring draws lie between consecutive ones. An effect with back work (draw, copy,
// present) since the previous one is a barrier the threaded step 2 would have to pay.
//
// Step 1 (masseffect_native_split_lockstep): the front (PM4 parse, registers, shader loads, pairing, draw-front
// proofs) appends every register store and every change of a stage's microcode to a Journal; the back (everything
// behind TargetsNative) owns a Mirror that applies the journal before each back operation and reads registers and
// microcode only from the mirror. In step 1 both run on the ring thread, one after the other (no concurrency):
// the mirror must equal the live register file bit for bit at every back operation, which the verification checks.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace me::native::ring_split {

// ------------------------------------------------------------------------------------------------------------------
// Step 0: guest-visible effects
// ------------------------------------------------------------------------------------------------------------------
enum Effect : uint32_t {
  kFence = 0,        // EVENT_WRITE_SHD: fence value written to memory (+ NotifyRingProgress)
  kMemWrite,         // MEM_WRITE
  kRegToMem,         // REG_TO_MEM
  kCondWriteMemory,  // COND_WRITE whose condition held and that wrote memory
  kEventWriteExt,    // EVENT_WRITE_EXT: screen extent written to memory
  kScratch,          // SCRATCH_REGn write-back to memory (SCRATCH_UMSK)
  kInterrupt,        // INTERRUPT packet (guest callback on the ring thread)
  kQueryBegin,       // EVENT_WRITE_ZPD begin: the begin counts are zeroed in guest memory
  kQueryEnd,         // EVENT_WRITE_ZPD end: counts written (query modes 0/1) or queued for the GPU result (mode 2)
  kReadPointer,      // read pointer write-back at the end of a ring segment
  kWaitMemory,       // WAIT_REG_MEM on memory (sync point: the front waits on guest memory)
  kWaitRegister,     // WAIT_REG_MEM on a register (sync point)
  kEffectCount
};

inline constexpr const char* kEffectNames[kEffectCount] = {
    "fence",   "MEM_WRITE", "REG_TO_MEM",   "COND_WRITE mem", "EVENT_WRITE_EXT", "scratch",
    "INTERRUPT", "ZPD begin", "ZPD end", "read pointer", "WAIT_REG_MEM mem", "WAIT_REG_MEM reg"};

inline constexpr uint32_t kGapBuckets = 6;
inline constexpr const char* kGapNames[kGapBuckets] = {"0", "1", "2-7", "8-31", "32-127", "128+"};

inline uint32_t GapBucket(uint64_t draws) {
  if (draws == 0) return 0;
  if (draws == 1) return 1;
  if (draws < 8) return 2;
  if (draws < 32) return 3;
  if (draws < 128) return 4;
  return 5;
}

struct EffectStats {
  std::array<uint64_t, kEffectCount> effects{};
  std::array<uint64_t, kGapBuckets> gaps{};  // ring draws between consecutive effects
  uint64_t barriers = 0;  // effects preceded by back work since the previous effect (a step-2 barrier that waits)
  uint64_t max_gap = 0;   // most ring draws between two consecutive effects
  uint64_t draws_since = 0, back_since = 0;  // carried across intervals

  void Draw() {
    ++draws_since;
    ++back_since;
  }
  void BackWork() { ++back_since; }  // copy, present
  void Note(Effect e) {
    ++effects[e];
    ++gaps[GapBucket(draws_since)];
    if (draws_since > max_gap) max_gap = draws_since;
    if (back_since) ++barriers;
    draws_since = back_since = 0;
  }
  uint64_t Total() const {
    uint64_t t = 0;
    for (uint64_t v : effects) t += v;
    return t;
  }
  void ResetInterval() {
    effects.fill(0);
    gaps.fill(0);
    barriers = max_gap = 0;
  }
};

// ------------------------------------------------------------------------------------------------------------------
// Step 1: register / microcode journal and the back's mirror
// ------------------------------------------------------------------------------------------------------------------
// Records, host-order 32-bit words:
//   kRecordRegs:      [kRecordRegs << 28 | count] [first register index] [count values]
//   kRecordMicrocode: [kRecordMicrocode << 28 | count] [stage (0 = VS, 1 = PS)] [count words]
// Consecutive register runs that continue each other (index == previous index + count) are merged into one record.
enum Kind : uint32_t { kRecordRegs = 1, kRecordMicrocode = 2 };
inline constexpr uint32_t kCountMask = 0x0FFFFFFFu;

class Journal {
 public:
  Journal() { Grow(1u << 16); }

  void Regs(uint32_t index, const uint32_t* values, uint32_t count) {
    if (!count) return;
    if (open_ != kNone && open_end_ == index && (words_[open_] & kCountMask) + count <= kCountMask) {
      uint32_t* w = Append(count);
      std::memcpy(w, values, size_t(count) * 4);
      words_[open_] += count;
      open_end_ += count;
    } else {
      const size_t at = size_;
      uint32_t* w = Append(2 + size_t(count));
      w[0] = (kRecordRegs << 28) | count;
      w[1] = index;
      std::memcpy(w + 2, values, size_t(count) * 4);
      open_ = at;
      open_end_ = index + count;
      ++records_;
    }
    register_words_ += count;
  }
  void Reg(uint32_t index, uint32_t value) {
    if (open_ != kNone && open_end_ == index && (words_[open_] & kCountMask) < kCountMask) {
      *Append(1) = value;
      ++words_[open_];
      ++open_end_;
      ++register_words_;
      return;
    }
    Regs(index, &value, 1);
  }
  void Microcode(uint32_t stage, const uint32_t* words, uint32_t count) {
    uint32_t* w = Append(2 + size_t(count));
    w[0] = (kRecordMicrocode << 28) | (count & kCountMask);
    w[1] = stage;
    if (count) std::memcpy(w + 2, words, size_t(count) * 4);
    open_ = kNone;
    ++records_;
    microcode_words_ += count;
  }

  bool empty() const { return size_ == 0; }
  size_t size() const { return size_; }
  const uint32_t* data() const { return words_.get(); }
  void Clear() {
    size_ = 0;
    open_ = kNone;
  }
  // Cumulative volume (for the overhead report).
  uint64_t records() const { return records_; }
  uint64_t register_words() const { return register_words_; }
  uint64_t microcode_words() const { return microcode_words_; }

 private:
  static constexpr size_t kNone = ~size_t(0);
  // n more words at the end (uninitialized: every caller writes them); the buffer only grows.
  uint32_t* Append(size_t n) {
    if (size_ + n > capacity_) Grow(std::max(capacity_ * 2, size_ + n));
    uint32_t* w = words_.get() + size_;
    size_ += n;
    return w;
  }
  void Grow(size_t capacity) {
    std::unique_ptr<uint32_t[]> bigger(new uint32_t[capacity]);
    if (size_) std::memcpy(bigger.get(), words_.get(), size_ * 4);
    words_ = std::move(bigger);
    capacity_ = capacity;
  }
  std::unique_ptr<uint32_t[]> words_;
  size_t size_ = 0, capacity_ = 0;
  size_t open_ = kNone;  // header of the last kRecordRegs record (kNone: the last record is not a register run)
  uint32_t open_end_ = 0;
  uint64_t records_ = 0, register_words_ = 0, microcode_words_ = 0;
};

// The back's private copy of the register file and of the two stages' microcode.
class Mirror {
 public:
  void Reset(const uint32_t* registers, size_t count) {
    registers_.assign(registers, registers + count);
    microcode_[0].clear();
    microcode_[1].clear();
  }
  // Start-up / resync: the full live register file and both stages' current microcode.
  void Seed(const uint32_t* registers, size_t count, const std::vector<uint32_t>& vs, const std::vector<uint32_t>& ps) {
    registers_.assign(registers, registers + count);
    microcode_[0] = vs;
    microcode_[1] = ps;
  }
  // Applies every record of the journal in order. False (and nothing more applied) on a malformed record; the
  // caller treats that like a verification difference.
  bool Apply(const uint32_t* w, size_t n) {
    size_t i = 0;
    while (i < n) {
      if (n - i < 2) return false;
      const uint32_t kind = w[i] >> 28, count = w[i] & kCountMask, arg = w[i + 1];
      if (n - i - 2 < count) return false;
      const uint32_t* values = w + i + 2;
      if (kind == kRecordRegs) {
        if (uint64_t(arg) + count > registers_.size()) return false;
        std::memcpy(registers_.data() + arg, values, size_t(count) * 4);
      } else if (kind == kRecordMicrocode && arg < 2) {
        microcode_[arg].assign(values, values + count);
      } else {
        return false;
      }
      i += 2 + size_t(count);
    }
    return true;
  }
  bool Apply(const Journal& journal) { return Apply(journal.data(), journal.size()); }

  const uint32_t* registers() const { return registers_.data(); }
  uint32_t* registers_mutable() { return registers_.data(); }
  size_t register_count() const { return registers_.size(); }
  const std::vector<uint32_t>& microcode(uint32_t stage) const { return microcode_[stage & 1]; }
  uint32_t Register(uint32_t index) const { return index < registers_.size() ? registers_[index] : 0; }

 private:
  std::vector<uint32_t> registers_;
  std::array<std::vector<uint32_t>, 2> microcode_;
};

// A register written outside the journal on purpose (CP_RB_WPTR: the game's MMIO kick, too frequent to flag) is
// journaled at a sync only when its live value differs from the mirror.
inline void JournalIfChanged(Journal& journal, const Mirror& mirror, const uint32_t* live, uint32_t index) {
  if (index < mirror.register_count() && mirror.registers()[index] != live[index]) journal.Reg(index, live[index]);
}

// Index of the first word where a and b differ, or -1 when the n words are equal.
inline int64_t FirstDifference(const uint32_t* a, const uint32_t* b, size_t n) {
  constexpr size_t kChunk = 256;
  for (size_t i = 0; i < n; i += kChunk) {
    const size_t m = n - i < kChunk ? n - i : kChunk;
    if (std::memcmp(a + i, b + i, m * 4) == 0) continue;
    for (size_t k = i; k < i + m; ++k)
      if (a[k] != b[k]) return int64_t(k);
  }
  return -1;
}

// Counts differing words (stops counting at limit).
inline uint32_t CountDifferences(const uint32_t* a, const uint32_t* b, size_t n, uint32_t limit = 0xFFFFFFFFu) {
  uint32_t c = 0;
  for (size_t i = 0; i < n && c < limit; ++i) c += a[i] != b[i];
  return c;
}

// Verification schedule: every back operation for the first `first` ones, then one in `every` (0 = never).
struct VerifySchedule {
  uint64_t first = 0, every = 0, seen = 0;
  bool Next() {
    const uint64_t k = seen++;
    if (k < first) return true;
    return every && (k - first) % every == 0;
  }
};

}  // namespace me::native::ring_split

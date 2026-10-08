/**
 * @file        core/log_ring.h
 * @brief       Lock-free multi-producer, single-consumer byte ring for the non-blocking log
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * Used by log_nonblocking.cpp (log_nonblocking). Kept free of spdlog and platform headers so it can
 * be unit-tested on the host on its own.
 *
 * Layout: a power-of-two byte array holding records. Every record starts at an 8-byte aligned
 * offset with an 8-byte header {state, size} followed by `size` bytes of text, padded to 8 bytes.
 * A record that does not fit before the end of the array is preceded by a padding record that
 * covers the rest of the array, so a record's text is always contiguous.
 *
 *   producers  Reserve with one CAS on head_ (the capacity check uses tail_), copy the text, then
 *              publish with a release store of the header state. No locks, no waiting: when the
 *              ring is full Push() returns false and the caller counts the drop.
 *   consumer   Walks from tail_ while headers are published, copies the text out, zeroes the
 *              consumed bytes (so a future header there reads as "not published") and advances
 *              tail_ with a release store. It stops at the first record that is reserved but not
 *              yet published: order is kept, and the record is picked up on the next pass.
 *
 * Only one consumer may run at a time; the caller serializes consumers (log_nonblocking.cpp does it
 * with its file lock).
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace rex::log_nb {

class MpscByteRing {
 public:
  static constexpr uint32_t kHeaderBytes = 8;

  // capacity is rounded up to a power of two (minimum 4 KB).
  explicit MpscByteRing(size_t capacity) {
    size_t cap = 4096;
    while (cap < capacity) cap <<= 1;
    capacity_ = cap;
    mask_ = cap - 1;
    data_.reset(new uint8_t[cap]);
    std::memset(data_.get(), 0, cap);
  }

  size_t capacity() const { return capacity_; }

  // Largest text a single record may hold. Callers truncate longer lines.
  size_t max_record_text() const { return capacity_ / 8 - kHeaderBytes; }

  // Bytes reserved and not yet consumed (approximate when read concurrently).
  size_t used() const {
    return static_cast<size_t>(head_.load(std::memory_order_relaxed) -
                               tail_.load(std::memory_order_relaxed));
  }

  // Never blocks. false = the ring is full (or n is too large); nothing was written.
  bool Push(const void* text, size_t n) {
    if (n > max_record_text()) return false;
    const uint64_t rec = Align8(kHeaderBytes + n);
    uint64_t h = head_.load(std::memory_order_relaxed);
    uint64_t pad = 0;
    for (;;) {
      const uint64_t pos = h & mask_;
      const uint64_t room_to_end = capacity_ - pos;
      pad = room_to_end < rec ? room_to_end : 0;
      const uint64_t need = pad + rec;
      if (h + need - tail_.load(std::memory_order_acquire) > capacity_) return false;
      if (head_.compare_exchange_weak(h, h + need, std::memory_order_relaxed,
                                      std::memory_order_relaxed)) {
        break;
      }
    }
    if (pad) {
      uint8_t* p = At(h);
      SizeOf(p) = static_cast<uint32_t>(pad);
      StateOf(p).store(kPadding, std::memory_order_release);
      h += pad;
    }
    uint8_t* p = At(h);
    SizeOf(p) = static_cast<uint32_t>(n);
    std::memcpy(p + kHeaderBytes, text, n);
    StateOf(p).store(kReady, std::memory_order_release);
    return true;
  }

  // Single consumer. Appends whole records to out (up to out_cap bytes; a record that does not fit
  // stays for the next call) and returns the number of bytes appended. *stalled is set when the walk
  // stopped at a record that is reserved but not yet published.
  size_t Pop(char* out, size_t out_cap, bool* stalled = nullptr) {
    if (stalled) *stalled = false;
    uint64_t t = tail_.load(std::memory_order_relaxed);
    const uint64_t h = head_.load(std::memory_order_acquire);
    size_t written = 0;
    while (t < h) {
      uint8_t* p = At(t);
      const uint32_t state = StateOf(p).load(std::memory_order_acquire);
      if (state == kEmpty) {
        if (stalled) *stalled = true;
        break;
      }
      const uint32_t size = SizeOf(p);
      uint64_t rec;
      if (state == kPadding) {
        rec = size;
      } else {
        if (written + size > out_cap) break;
        std::memcpy(out + written, p + kHeaderBytes, size);
        written += size;
        rec = Align8(kHeaderBytes + size);
      }
      // Zero the record so that whatever header lands here next starts unpublished. The release
      // store of tail_ below orders this before any producer can reserve these bytes again.
      StateOf(p).store(kEmpty, std::memory_order_relaxed);
      std::memset(p + 4, 0, static_cast<size_t>(rec - 4));
      t += rec;
    }
    tail_.store(t, std::memory_order_release);
    return written;
  }

 private:
  static constexpr uint32_t kEmpty = 0;
  static constexpr uint32_t kReady = 1;
  static constexpr uint32_t kPadding = 2;

  static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t), "header layout");
  static_assert(std::atomic<uint32_t>::is_always_lock_free, "header state must be lock-free");

  static uint64_t Align8(uint64_t v) { return (v + 7) & ~uint64_t(7); }
  uint8_t* At(uint64_t cursor) const { return data_.get() + (cursor & mask_); }
  // The header lives inside the byte array; its state word is accessed as an atomic in place
  // (std::atomic<uint32_t> has the size and alignment of uint32_t on every supported compiler).
  static std::atomic<uint32_t>& StateOf(uint8_t* p) {
    return *reinterpret_cast<std::atomic<uint32_t>*>(p);
  }
  static uint32_t& SizeOf(uint8_t* p) { return *reinterpret_cast<uint32_t*>(p + 4); }

  std::unique_ptr<uint8_t[]> data_;
  size_t capacity_ = 0;
  uint64_t mask_ = 0;
  alignas(64) std::atomic<uint64_t> head_{0};  // reservation cursor (producers)
  alignas(64) std::atomic<uint64_t> tail_{0};  // consumption cursor (the consumer)
};

}  // namespace rex::log_nb

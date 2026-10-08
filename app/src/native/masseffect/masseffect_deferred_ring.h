// Packed record layout of the deferred recording queue (masseffect_deferred_native_compact).
//
// The default queue gives every queued vkCmd* call a fixed 128-byte slot. Most calls carry 24-48 bytes of
// arguments (vkCmdBindPipeline 32, vkCmdDrawIndexed 40), so a draw's ~8.6 calls wrote ~1.1 KB and touched 2-3
// cache lines per call that the worker core had just read (the slot array was only 16-byte aligned). The compact
// layout packs each call as a 16-byte header plus its arguments rounded up to 16 bytes, in a 64-byte aligned byte
// ring of the same total size. A record never wraps: when it does not fit before the end of the ring, the producer
// writes a skip record (run == nullptr) that covers the tail and the call goes to offset 0.
//
// Positions are byte counts that only grow (written, read); the ring offset is position & (capacity - 1).
// Shared by masseffect_deferred_recording.cpp and tests/cpu/test_native_deferred_queue_compact.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

namespace masseffect::native::deferred::ring {

struct alignas(16) Header {
  void (*run)(void*);  // nullptr: skip record (end of the ring)
  uint32_t bytes;      // whole record, header included, multiple of 16
  uint32_t reserved;
};
static_assert(sizeof(Header) == 16, "record header must stay 16 bytes");

// Record size for a payload of `payload` bytes.
constexpr uint32_t RecordBytes(size_t payload) {
  return uint32_t(sizeof(Header) + ((payload + 15) & ~size_t(15)));
}

// Bytes the producer must have free to queue a record of `bytes` at position `written`: the record itself plus
// the skip record of the ring's tail when it does not fit before the end. Writes the tail size to `skip`.
inline uint64_t Needed(uint64_t written, size_t capacity, uint32_t bytes, uint32_t& skip) {
  const size_t offset = size_t(written & (capacity - 1));
  skip = offset + bytes > capacity ? uint32_t(capacity - offset) : 0;
  return uint64_t(skip) + bytes;
}

// The record space is free when the worker has read past everything that used it one lap earlier.
inline bool Fits(uint64_t written, uint64_t read, size_t capacity, uint64_t needed) {
  return written + needed - read <= capacity;
}

// Producer: writes the skip record (if any) and returns where the record goes; advances `written` past the skip.
inline uint8_t* Place(uint8_t* base, size_t capacity, uint64_t& written, uint32_t skip) {
  if (skip) {
    Header* h = reinterpret_cast<Header*>(base + (written & (capacity - 1)));
    h->run = nullptr;
    h->bytes = skip;
    written += skip;
  }
  return base + (written & (capacity - 1));
}

// Worker: runs the record at `read` (a skip record runs nothing) and returns the position after it.
inline uint64_t RunOne(uint8_t* base, size_t capacity, uint64_t read) {
  Header* h = reinterpret_cast<Header*>(base + (read & (capacity - 1)));
  const uint32_t bytes = h->bytes;  // read before run() destroys the payload
  if (h->run) h->run(reinterpret_cast<uint8_t*>(h) + sizeof(Header));
  return read + bytes;
}

}  // namespace masseffect::native::deferred::ring

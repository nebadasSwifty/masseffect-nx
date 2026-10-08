// Guest-memory change stamps from the GPU coherency events (masseffect_native_texture_coherency).
//
// Why. A stable texture is rechecked by hashing (XXH3) all its guest bytes every few frames
// (masseffect_native_texture_interval_max), only to find, almost always, that nothing changed. In the Feros firefight
// profile of 2026-10-08 that hash was ~25 % of the ring thread's busy samples (docs/ring-cpu-per-draw.md).
//
// The idea. On the Xbox 360 the CPU and the GPU share memory, but the GPU keeps texture and vertex caches. Whenever
// the CPU (or a DMA from disk) changes memory the GPU may already have read, Direct3D has to tell the GPU to drop
// that range from its caches; it does so in the command stream: type-0 writes of COHER_SIZE_HOST (0x0A2F) and
// COHER_BASE_HOST (0x0A30), COHER_STATUS_HOST (0x0A31) with the TC/VC action bits, and a WAIT_REG_MEM on the
// status (sub_82227210 and its siblings in the recompiled D3D: base rounded down to 4 KB, size in bytes rounded up
// to 4 KB). So the ring sees, in stream order, every range the game declares changed. This table keeps, for every
// 16 KB page of the 512 MB of physical memory, the stamp of the last such event that touched it. A texture records
// the current stamp when it hashes its bytes; if later no page of its range has a newer stamp, its bytes have not
// been declared changed and the hash can be skipped.
//
// It is a hypothesis about the game, not a proof: a write the game never declares (no cache invalidation because,
// say, the GPU had not read that memory since its last full invalidation) would be missed. Hence the guards in
// PrepareTexture: the first masseffect_native_verify_n skips and then 1 in
// masseffect_native_texture_coherency_full_every skips of each texture still hash, and a single changed hash on a
// texture the table calls clean logs DIFFERENCE and turns the skip off for the session. Mode 2 measures only.
//
// Other writers of guest memory that the ring knows about (resolved texels read back into guest memory) stamp
// their range too. Small writes (fences, query results, scratch registers) never hit texture memory.
//
// Threads. Marks come from the ring thread (register writes, read-backs) and, in theory, from MMIO writes on game
// threads; checks come from the ring thread. Stamps are handed out by one atomic counter and a page only ever moves
// to a newer stamp (CAS max), so a racing older mark cannot hide a newer one. A reader captures Current() before it
// reads the texture bytes: an event that got a stamp <= that value was issued before the read, so its data is in
// the hash (the issuing thread wrote the data before the event, release/acquire through `seq`).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace me::native::texture_coherency {

inline constexpr uint64_t kPhysicalBytes = 0x20000000ull;  // 512 MB
inline constexpr uint32_t kPageShift = 14;                 // 16 KB pages: 32768 stamps, 128 KB
inline constexpr uint32_t kPages = uint32_t(kPhysicalBytes >> kPageShift);
// The 0xE0000000 view of the 360 maps physical memory with a 4 KB offset; if an event ever carries a virtual
// address instead of a physical one, widening every mark by this much on both sides still covers the right bytes.
inline constexpr uint64_t kSlack = 0x1000;

struct Table {
  std::atomic<uint32_t> consumers{0};  // kConsumer* bits; marks are only recorded while a consumer is on
  std::atomic<uint32_t> seq{0};    // last stamp handed out; 0 = no event yet
  std::array<std::atomic<uint32_t>, kPages> page{};
  // Measurement (cumulative; the report takes differences).
  std::atomic<uint64_t> events{0};         // marks with a non-empty range
  std::atomic<uint64_t> events_empty{0};   // size 0: nothing to mark
  std::atomic<uint64_t> bytes{0};          // bytes declared (before rounding to pages)
  std::atomic<uint64_t> pages_marked{0};
  std::atomic<uint64_t> by_source[4]{};    // kSource*
};

enum Source : uint32_t { kSourceBaseWrite = 0, kSourceWait, kSourceMmio, kSourceReadback };

inline Table g;

// Consumers of the stamps. Each one switches only its own bit, so the texture recheck turning itself off (a
// DIFFERENCE) does not stop the marks the vertex arena relies on, and the other way round. A consumer must switch
// its bit on before it takes the first stamp it will trust, and must never trust a stamp again after its bit was off
// (marks are lost while no consumer is on).
inline constexpr uint32_t kConsumerTextures = 1;  // masseffect_native_texture_coherency
inline constexpr uint32_t kConsumerVertices = 2;  // masseffect_native_vertex_arena / _measure

inline void EnableConsumer(uint32_t bit, bool on) {
  if (on) {
    g.consumers.fetch_or(bit, std::memory_order_acq_rel);
  } else {
    g.consumers.fetch_and(~bit, std::memory_order_acq_rel);
  }
}
// The texture recheck's switch (its historical name).
inline void Enable(bool on) { EnableConsumer(kConsumerTextures, on); }
inline bool Enabled() { return g.consumers.load(std::memory_order_relaxed) != 0; }

// The newest stamp handed out so far. Take it BEFORE reading the bytes it will vouch for.
inline uint32_t Current() { return g.seq.load(std::memory_order_acquire); }

// Declares [address, address + size) of physical memory changed. Addresses are masked to 512 MB; a range that
// reaches past the end is clamped (and wraps to the start if it does, as the GPU's 29-bit addresses would).
// Returns the stamp (0 if nothing was marked).
inline uint32_t Mark(uint32_t address, uint32_t size, Source source) {
  if (!Enabled()) return 0;
  if (!size) {
    g.events_empty.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  const uint32_t stamp = g.seq.fetch_add(1, std::memory_order_acq_rel) + 1;
  g.events.fetch_add(1, std::memory_order_relaxed);
  g.bytes.fetch_add(size, std::memory_order_relaxed);
  g.by_source[source].fetch_add(1, std::memory_order_relaxed);
  const uint64_t start = uint64_t(address & 0x1FFFFFFFu);
  const uint64_t first = start >= kSlack ? start - kSlack : 0;
  uint64_t end = start + uint64_t(size) + kSlack;  // exclusive
  uint64_t wrapped = 0;                            // bytes past 512 MB, marked from 0
  if (uint64_t(size) >= kPhysicalBytes) {
    end = kPhysicalBytes;
    wrapped = 0;
  } else if (end > kPhysicalBytes) {
    wrapped = end - kPhysicalBytes;
    end = kPhysicalBytes;
  }
  auto mark_pages = [stamp](uint64_t from, uint64_t to) {
    uint64_t n = 0;
    for (uint64_t p = from >> kPageShift; p <= (to - 1) >> kPageShift; ++p, ++n) {
      std::atomic<uint32_t>& cell = g.page[size_t(p)];
      uint32_t old = cell.load(std::memory_order_relaxed);
      while (old < stamp && !cell.compare_exchange_weak(old, stamp, std::memory_order_release,
                                                        std::memory_order_relaxed)) {
      }
    }
    g.pages_marked.fetch_add(n, std::memory_order_relaxed);
  };
  if (end > first) mark_pages(first, end);
  if (wrapped) mark_pages(0, wrapped);
  return stamp;
}

// The newest stamp of the pages [start, start + length) touches (physical, masked). 0 if never marked.
inline uint32_t Newest(uint64_t start, uint64_t length) {
  if (!length) return 0;
  start &= 0x1FFFFFFFull;
  uint64_t end = start + length;
  if (end > kPhysicalBytes) end = kPhysicalBytes;  // callers never pass ranges past memory
  uint32_t newest = 0;
  for (uint64_t p = start >> kPageShift; p <= (end - 1) >> kPageShift; ++p) {
    const uint32_t v = g.page[size_t(p)].load(std::memory_order_acquire);
    newest = v > newest ? v : newest;
  }
  return newest;
}

// True if no page of [start, start + length) was marked after `stamp`.
inline bool Clean(uint64_t start, uint64_t length, uint32_t stamp) { return Newest(start, length) <= stamp; }

}  // namespace me::native::texture_coherency

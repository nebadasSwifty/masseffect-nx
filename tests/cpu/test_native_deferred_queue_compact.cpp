// Host stress test of the compact deferred recording queue (masseffect_deferred_native_compact): records of
// different sizes packed in a byte ring with masseffect_deferred_ring.h (the same header the app uses), skip
// records at the end of the ring, and the producer/worker protocol of masseffect_deferred_recording.cpp
// (ReserveCompact / PublishCompact / PublishAlready / the worker's compact branch / DrainInternal) in both the
// classic and the batched ("fast") publication modes. The ring is 1 KB here so that it wraps and fills
// constantly. Checks: every record runs exactly once, in order, with its own payload intact (sizes 16-112 bytes,
// as the real Package types), nothing runs from a skip record, a drain returns only after everything queued
// before it ran, and the producer never deadlocks on a full ring of unpublished records.
//   clang++ -std=c++20 -O2 -pthread -Iapp/src/native/masseffect tests/cpu/test_native_deferred_queue_compact.cpp
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <random>
#include <thread>
#include <vector>

#include "masseffect_deferred_ring.h"

namespace ring = masseffect::native::deferred::ring;

namespace {

constexpr size_t kRingBytes = 1024;
alignas(64) uint8_t g_ring[kRingBytes];
alignas(64) std::atomic<uint64_t> g_written{0};
alignas(64) std::atomic<uint64_t> g_read{0};
uint64_t g_written_producer = 0, g_published = 0, g_read_view = 0;
uint32_t g_unpublished = 0, g_batch = 16;
bool g_fast = true;
std::atomic<bool> g_sleeping{false}, g_stop{false};
std::mutex g_mutex;
std::condition_variable g_cv;
std::vector<uint64_t> g_executed;  // worker only; read by the producer after a drain
uint64_t g_corrupt = 0;            // worker only
uint64_t g_queued = 0, g_skips = 0;
uint32_t g_naps = 8;

void Wake() {
  if (g_sleeping.load(std::memory_order_seq_cst)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cv.notify_one();
  }
}

void PublishAlready() {
  g_published = g_written_producer;
  g_unpublished = 0;
  g_written.store(g_written_producer, std::memory_order_seq_cst);
  Wake();
}

uint8_t* ReserveCompact(uint32_t bytes) {
  uint32_t skip = 0;
  const uint64_t needed = ring::Needed(g_written_producer, kRingBytes, bytes, skip);
  if (g_fast) {
    if (!ring::Fits(g_written_producer, g_read_view, kRingBytes, needed)) {
      if (g_published != g_written_producer) PublishAlready();
      for (;;) {
        g_read_view = g_read.load(std::memory_order_acquire);
        if (ring::Fits(g_written_producer, g_read_view, kRingBytes, needed)) break;
        Wake();
        std::this_thread::sleep_for(std::chrono::microseconds(20));
      }
    }
  } else {
    while (!ring::Fits(g_written_producer, g_read.load(std::memory_order_acquire), kRingBytes, needed)) {
      Wake();
      std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
  }
  if (skip) ++g_skips;
  return ring::Place(g_ring, kRingBytes, g_written_producer, skip);
}

void PublishCompact(uint32_t bytes) {
  g_written_producer += bytes;
  ++g_queued;
  if (g_fast) {
    if (++g_unpublished >= g_batch) PublishAlready();
    return;
  }
  g_written.store(g_written_producer, std::memory_order_seq_cst);
  Wake();
}

// A payload like the app's Package: a sequence number and a filler derived from it (checked by the worker).
template <size_t N>
struct Payload {
  uint64_t sequence;
  uint8_t fill[N];
};

template <size_t N>
void Enqueue(uint64_t sequence) {
  using P = Payload<N>;
  constexpr uint32_t kBytes = ring::RecordBytes(sizeof(P));
  static_assert(kBytes <= 128, "the app's largest record is 16 + 112 bytes");
  uint8_t* record = ReserveCompact(kBytes);
  auto* header = reinterpret_cast<ring::Header*>(record);
  P* p = new (record + sizeof(ring::Header)) P{};
  p->sequence = sequence;
  std::memset(p->fill, int(sequence & 0xFF), N);
  header->run = [](void* c) {
    P* q = static_cast<P*>(c);
    for (size_t i = 0; i < N; ++i)
      if (q->fill[i] != uint8_t(q->sequence & 0xFF)) {
        ++g_corrupt;
        break;
      }
    g_executed.push_back(q->sequence);
    q->~P();
  };
  header->bytes = kBytes;
  PublishCompact(kBytes);
}

void Flush() {
  if (g_fast && g_published != g_written_producer) PublishAlready();
}

void Worker() {
  uint64_t read = 0;
  uint32_t empty = 0;
  const bool fast = g_fast;
  while (!g_stop.load(std::memory_order_relaxed)) {
    const uint64_t written = g_written.load(std::memory_order_acquire);
    if (read < written) {
      uint32_t records = 0;
      do {
        read = ring::RunOne(g_ring, kRingBytes, read);
        if (!fast || (++records & 15) == 0) g_read.store(read, std::memory_order_release);
      } while (read < written);
      g_read.store(read, std::memory_order_release);
      empty = 0;
      continue;
    }
    if (++empty < g_naps) {
      std::this_thread::sleep_for(std::chrono::microseconds(5));
      continue;
    }
    std::unique_lock<std::mutex> lock(g_mutex);
    g_sleeping.store(true, std::memory_order_seq_cst);
    if (g_written.load(std::memory_order_seq_cst) == read && !g_stop.load()) {
      g_cv.wait_for(lock, std::chrono::milliseconds(2));
    }
    g_sleeping.store(false, std::memory_order_seq_cst);
    empty = 0;
  }
}

void Drain(int* failures) {
  if (g_fast && g_published != g_written_producer) PublishAlready();
  if (g_read.load(std::memory_order_acquire) != g_written_producer) {
    Wake();
    for (uint32_t laps = 0; g_read.load(std::memory_order_acquire) != g_written_producer; ++laps) {
      if (laps < 32) std::this_thread::yield();
      else std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
  }
  g_read_view = g_written_producer;
  if (g_executed.size() != g_queued) {
    std::printf("FAIL drain: %zu executed, %llu queued\n", g_executed.size(), (unsigned long long)g_queued);
    ++*failures;
  }
}

}  // namespace

int main() {
  int failures = 0;
  // Layout checks.
  if (ring::RecordBytes(1) != 32 || ring::RecordBytes(16) != 32 || ring::RecordBytes(17) != 48 ||
      ring::RecordBytes(112) != 128) {
    std::printf("FAIL RecordBytes\n");
    ++failures;
  }
  {
    uint32_t skip = 0;
    if (ring::Needed(1000, 1024, 32, skip) != 56 || skip != 24) { std::printf("FAIL Needed wrap\n"); ++failures; }
    if (ring::Needed(992, 1024, 32, skip) != 32 || skip != 0) { std::printf("FAIL Needed exact end\n"); ++failures; }
    if (!ring::Fits(2048, 2048, 1024, 1024) || !ring::Fits(2048, 1026, 1024, 2) || ring::Fits(2048, 1025, 1024, 2)) { std::printf("FAIL Fits\n"); ++failures; }
  }
  for (int mode = 0; mode < 2; ++mode) {
    for (uint32_t batch : {1u, 3u, 16u, 100u}) {
      g_fast = mode == 1;
      g_batch = batch;
      if (!g_fast && batch != 16) continue;
      g_written = 0;
      g_read = 0;
      g_written_producer = g_published = g_read_view = 0;
      g_unpublished = 0;
      g_queued = g_skips = 0;
      g_executed.clear();
      g_corrupt = 0;
      g_stop = false;
      std::thread worker(Worker);
      std::mt19937 rng(4321 + batch);
      uint64_t sequence = 0, drains = 0;
      for (int burst = 0; burst < 20000; ++burst) {
        const uint32_t n = 1 + uint32_t(rng() % 120);  // often more than the ring holds
        for (uint32_t i = 0; i < n; ++i) {
          switch (rng() % 5) {
            case 0: Enqueue<8>(sequence++); break;    // 16 B payload: 32 B record
            case 1: Enqueue<24>(sequence++); break;   // 32: 48
            case 2: Enqueue<40>(sequence++); break;   // 48: 64
            case 3: Enqueue<72>(sequence++); break;   // 80: 96
            default: Enqueue<104>(sequence++); break; // 112: 128
          }
        }
        const uint32_t action = uint32_t(rng() % 8);
        if (action == 0) {
          Drain(&failures);
          ++drains;
        } else if (action == 1) {
          Flush();
          std::this_thread::sleep_for(std::chrono::microseconds(rng() % 300));
        }
        if (failures) break;
      }
      Drain(&failures);
      g_stop = true;
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_cv.notify_all();
      }
      worker.join();
      for (size_t i = 0; i < g_executed.size(); ++i)
        if (g_executed[i] != i) {
          std::printf("FAIL order: position %zu executed record %llu\n", i, (unsigned long long)g_executed[i]);
          ++failures;
          break;
        }
      if (g_corrupt) {
        std::printf("FAIL payload: %llu records corrupted\n", (unsigned long long)g_corrupt);
        ++failures;
      }
      std::printf("mode %s batch %u: %llu records, %llu skip records, %llu drains, %zu executed, %d failures\n",
                  g_fast ? "fast" : "classic", batch, (unsigned long long)sequence, (unsigned long long)g_skips,
                  (unsigned long long)drains, g_executed.size(), failures);
    }
  }
  return failures ? 1 : 0;
}

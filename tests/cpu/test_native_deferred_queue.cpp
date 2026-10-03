// Host stress test of the deferred recording queue protocol in app/src/native/masseffect/masseffect_deferred_recording.cpp, "fast" mode
// (masseffect_deferred_native_fast): batched publication by the producer, the producer's own copy of the worker's read
// index, the worker storing its read index every 16 commands and at the end of each burst, drain = publish + wait for the
// read index. The functions below are a line-for-line model of Reserve / Publish / PublishAlready / Wake / Worker /
// DrainInternal with the Vulkan payload replaced by a sequence number (the real queue holds 1024 slots; here 64 so that it
// wraps and fills constantly). Checks: every command executes exactly once, in order, nothing is lost or duplicated, a
// drain returns only after everything queued before it ran, the producer never deadlocks when the queue is full of
// unpublished commands, and a final flush (idle flush) is enough for the worker to finish.
//   clang++ -std=c++20 -O2 -pthread tests/cpu/test_native_deferred_queue.cpp -o /tmp/test_native_deferred_queue
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace {

constexpr size_t kSlots = 64;
struct Slot {
  uint64_t sequence = 0;
};
Slot g_queue[kSlots];
alignas(64) std::atomic<uint64_t> g_written{0};
alignas(64) std::atomic<uint64_t> g_read{0};
uint64_t g_written_producer = 0, g_published = 0, g_read_view = 0;
uint32_t g_batch = 16;
bool g_fast = true;
std::atomic<bool> g_sleeping{false}, g_stop{false};
std::mutex g_mutex;
std::condition_variable g_cv;
std::vector<uint64_t> g_executed;  // written by the worker only; read by the producer after a drain (acquire on g_read)
uint32_t g_naps = 8;

void Wake() {
  if (g_sleeping.load(std::memory_order_seq_cst)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cv.notify_one();
  }
}

void PublishAlready() {
  g_published = g_written_producer;
  g_written.store(g_written_producer, std::memory_order_seq_cst);
  Wake();
}

Slot& Reserve() {
  if (g_fast) {
    if (g_written_producer - g_read_view >= kSlots) {
      if (g_published != g_written_producer) PublishAlready();
      for (;;) {
        g_read_view = g_read.load(std::memory_order_acquire);
        if (g_written_producer - g_read_view < kSlots) break;
        Wake();
        std::this_thread::sleep_for(std::chrono::microseconds(20));
      }
    }
    return g_queue[g_written_producer & (kSlots - 1)];
  }
  while (g_written_producer - g_read.load(std::memory_order_acquire) >= kSlots) {
    Wake();
    std::this_thread::sleep_for(std::chrono::microseconds(20));
  }
  return g_queue[g_written_producer & (kSlots - 1)];
}

void Publish() {
  ++g_written_producer;
  if (g_fast) {
    if (g_written_producer - g_published >= g_batch) PublishAlready();
    return;
  }
  g_written.store(g_written_producer, std::memory_order_seq_cst);
  Wake();
}

void Enqueue(uint64_t sequence) {
  Slot& r = Reserve();
  r.sequence = sequence;
  Publish();
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
      if (fast) {
        do {
          Slot& r = g_queue[read & (kSlots - 1)];
          g_executed.push_back(r.sequence);
          ++read;
          if ((read & 15) == 0) g_read.store(read, std::memory_order_release);
        } while (read < written);
        g_read.store(read, std::memory_order_release);
      } else {
        do {
          Slot& r = g_queue[read & (kSlots - 1)];
          g_executed.push_back(r.sequence);
          ++read;
          g_read.store(read, std::memory_order_release);
        } while (read < written);
      }
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
  // everything queued before the drain has run, in order
  if (g_executed.size() != g_written_producer) {
    std::printf("FAIL drain: %zu executed, %llu queued\n", g_executed.size(), (unsigned long long)g_written_producer);
    ++*failures;
  }
}

}  // namespace

int main(int argc, char** argv) {
  int failures = 0;
  for (int mode = 0; mode < 2; ++mode) {
    for (uint32_t batch : {1u, 3u, 16u, 100u}) {
      g_fast = mode == 1;
      g_batch = batch;
      if (!g_fast && batch != 16) continue;
      g_written = 0;
      g_read = 0;
      g_written_producer = g_published = g_read_view = 0;
      g_executed.clear();
      g_stop = false;
      std::thread worker(Worker);
      std::mt19937 rng(1234 + batch);
      uint64_t sequence = 0;
      uint64_t drains = 0;
      for (int burst = 0; burst < 20000; ++burst) {
        const uint32_t n = 1 + uint32_t(rng() % 200);  // longer than the queue sometimes
        for (uint32_t i = 0; i < n; ++i) Enqueue(sequence++);
        const uint32_t action = uint32_t(rng() % 8);
        if (action == 0) {
          Drain(&failures);
          ++drains;
        } else if (action == 1) {
          Flush();  // the ring thread idles
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
          std::printf("FAIL order: position %zu executed command %llu\n", i, (unsigned long long)g_executed[i]);
          ++failures;
          break;
        }
      std::printf("mode %s batch %u: %llu commands, %llu drains, %zu executed, %d failures\n", g_fast ? "fast" : "classic",
                  batch, (unsigned long long)sequence, (unsigned long long)drains, g_executed.size(), failures);
    }
  }
  (void)argc;
  (void)argv;
  return failures ? 1 : 0;
}

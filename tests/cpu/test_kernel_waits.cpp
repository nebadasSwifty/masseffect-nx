// Host test of the SDK's POSIX wait objects (sdk/src/core/threading_posix.cpp, docs/kernel-waits.md):
//   - a state change notifies the condition variable only when a waiter is counted: no lost wake-up when the signal
//     races the start of a wait (Event, Semaphore, Mutant), with blocking and with timed waits;
//   - WaitMultiple without per-call vectors: index of the signalled handle, wait-all, more than 64 handles (heap
//     fallback), a single handle (the fast path), timeouts;
//   - the static cast from WaitHandle to its condition (every handle type records it).
// Built by tests/run_all.sh together with threading_posix.cpp and the stubs below (no logger, no timer queue: the
// test never arms a timer). A lost wake-up shows up as a wait that hits its 5 s guard timeout.

#include <rex/thread.h>
#include <rex/cvar.h>
#include <rex/chrono/clock.h>
#include <rex/logging/api.h>
#include <rex/thread/timer_queue.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

// ---- stubs of the SDK pieces threading_posix.cpp links against --------------------------------------------------
namespace rex {
LogCategoryId RegisterLogCategory(const char*) { return LogCategoryId{0}; }
spdlog::logger* GetLoggerRaw(LogCategoryId) { return nullptr; }
uint64_t chrono::Clock::QueryHostSystemTime() { return 0; }
namespace cvar {
std::optional<size_t> RegisterFlag(FlagEntry) { return std::nullopt; }
void UnregisterFlag(std::string_view) {}
}  // namespace cvar
namespace thread {
void TimerQueueWaitItem::Disarm() {}
std::weak_ptr<TimerQueueWaitItem> QueueTimerOnce(std::function<void(void*)>, void*, TimerQueueWaitItem::clock::time_point) {
  std::abort();
}
std::weak_ptr<TimerQueueWaitItem> QueueTimerRecurring(std::function<void(void*)>, void*,
                                                      TimerQueueWaitItem::clock::time_point,
                                                      TimerQueueWaitItem::clock::duration) {
  std::abort();
}
uint32_t logical_processor_count() { return std::max(1u, std::thread::hardware_concurrency()); }
}  // namespace thread
}  // namespace rex

REXCVAR_DECLARE(bool, thread_wait_blocking);

using namespace std::chrono_literals;
using rex::thread::WaitResult;

namespace {

std::atomic<int> g_failures{0};
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

constexpr auto kGuard = std::chrono::milliseconds(5000);

void TestHandlesRecordTheirCondition() {
  auto e = rex::thread::Event::CreateAutoResetEvent(false);
  auto m = rex::thread::Event::CreateManualResetEvent(false);
  auto s = rex::thread::Semaphore::Create(0, 4);
  auto x = rex::thread::Mutant::Create(false);
  auto t = rex::thread::Timer::CreateManualResetTimer();
  CHECK(e->platform_wait_object() != nullptr);
  CHECK(m->platform_wait_object() != nullptr);
  CHECK(s->platform_wait_object() != nullptr);
  CHECK(x->platform_wait_object() != nullptr);
  CHECK(t->platform_wait_object() != nullptr);
  CHECK(e->platform_wait_object() != m->platform_wait_object());
  CHECK(rex::thread::Wait(nullptr, false, 0ms) == WaitResult::kFailed);
}

// The signaller races the waiter's start: alternate who goes first, many times. Every wait must finish well before
// its guard timeout (a skipped notify with a parked waiter would hit it).
template <typename SignalFn>
void RaceSignalAgainstWait(rex::thread::WaitHandle* handle, SignalFn signal, bool timed, int rounds) {
  for (int i = 0; i < rounds; ++i) {
    std::atomic<bool> go{false};
    std::thread signaller([&] {
      while (!go.load(std::memory_order_acquire)) {
      }
      if (i % 3 == 1) std::this_thread::yield();
      if (i % 3 == 2) std::this_thread::sleep_for(50us);
      signal();
    });
    go.store(true, std::memory_order_release);
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = rex::thread::Wait(handle, false, timed ? kGuard : std::chrono::milliseconds::max());
    const auto dt = std::chrono::steady_clock::now() - t0;
    signaller.join();
    CHECK(r == WaitResult::kSuccess);
    CHECK(dt < kGuard / 2);
    if (r != WaitResult::kSuccess || dt >= kGuard / 2) return;
  }
}

void TestEventNoLostWakeup() {
  auto e = rex::thread::Event::CreateAutoResetEvent(false);
  RaceSignalAgainstWait(e.get(), [&] { e->Set(); }, false, 3000);
  RaceSignalAgainstWait(e.get(), [&] { e->Set(); }, true, 3000);
  // Auto-reset: consumed by the waits above.
  CHECK(rex::thread::Wait(e.get(), false, 0ms) == WaitResult::kTimeout);
  auto m = rex::thread::Event::CreateManualResetEvent(false);
  RaceSignalAgainstWait(m.get(), [&] { m->Set(); }, true, 1);
  CHECK(rex::thread::Wait(m.get(), false, 0ms) == WaitResult::kSuccess);  // stays signalled
  m->Reset();
  CHECK(rex::thread::Wait(m.get(), false, 0ms) == WaitResult::kTimeout);
}

void TestSemaphoreNoLostWakeup() {
  auto s = rex::thread::Semaphore::Create(0, 1000);
  RaceSignalAgainstWait(s.get(), [&] { s->Release(1, nullptr); }, false, 3000);
  RaceSignalAgainstWait(s.get(), [&] { s->Release(1, nullptr); }, true, 3000);
  CHECK(rex::thread::Wait(s.get(), false, 0ms) == WaitResult::kTimeout);
  int previous = -1;
  CHECK(s->Release(3, &previous) && previous == 0);
  CHECK(!s->Release(998, nullptr));  // over the maximum
  for (int i = 0; i < 3; ++i) CHECK(rex::thread::Wait(s.get(), false, 0ms) == WaitResult::kSuccess);
  CHECK(rex::thread::Wait(s.get(), false, 0ms) == WaitResult::kTimeout);

  // Several waiters, released one at a time: each release wakes one of them.
  constexpr int kWaiters = 4, kEach = 500;
  std::atomic<int> got{0};
  std::vector<std::thread> waiters;
  for (int w = 0; w < kWaiters; ++w) {
    waiters.emplace_back([&] {
      for (int i = 0; i < kEach; ++i) {
        if (rex::thread::Wait(s.get(), false, kGuard) == WaitResult::kSuccess) got.fetch_add(1);
      }
    });
  }
  for (int i = 0; i < kWaiters * kEach; ++i) {
    while (!s->Release(1, nullptr)) std::this_thread::yield();
    if (i % 64 == 0) std::this_thread::yield();
  }
  for (auto& t : waiters) t.join();
  CHECK(got.load() == kWaiters * kEach);
}

// The spinning guest timer pattern: acquire a mutant through a one-handle WaitAny with a 1 ms timeout, work, release;
// several threads at once. Checks mutual exclusion, recursion and that a release with a parked waiter wakes it.
void TestMutantExclusionAndHandoff() {
  auto x = rex::thread::Mutant::Create(false);
  rex::thread::WaitHandle* handles[1] = {x.get()};
  constexpr int kThreads = 4, kEach = 4000;
  std::atomic<int> inside{0};
  std::atomic<int> done{0};
  std::atomic<bool> overlap{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      int mine = 0;
      while (mine < kEach) {
        auto r = rex::thread::WaitAny(handles, 1, false, 1ms);
        if (r.first == WaitResult::kTimeout) continue;
        CHECK(r.first == WaitResult::kSuccess && r.second == 0);
        if (inside.fetch_add(1) != 0) overlap = true;
        // Recursive acquisition by the owner succeeds at once.
        CHECK(rex::thread::Wait(x.get(), false, 0ms) == WaitResult::kSuccess);
        CHECK(x->Release());
        inside.fetch_sub(1);
        CHECK(x->Release());
        ++mine;
      }
      done.fetch_add(mine);
    });
  }
  for (auto& t : threads) t.join();
  CHECK(!overlap.load());
  CHECK(done.load() == kThreads * kEach);
  CHECK(!x->Release());  // not owned

  // Hand-off to a thread blocked without timeout: the release must wake it.
  auto y = rex::thread::Mutant::Create(true);
  std::atomic<bool> acquired{false};
  std::thread waiter([&] {
    CHECK(rex::thread::Wait(y.get(), false, kGuard) == WaitResult::kSuccess);
    acquired = true;
    CHECK(y->Release());
  });
  std::this_thread::sleep_for(20ms);
  CHECK(!acquired.load());
  CHECK(y->Release());
  waiter.join();
  CHECK(acquired.load());
}

void TestWaitMultiple() {
  // Any: the index of the handle that was signalled, from another thread while parked.
  for (int round = 0; round < 200; ++round) {
    auto a = rex::thread::Event::CreateAutoResetEvent(false);
    auto b = rex::thread::Event::CreateAutoResetEvent(false);
    auto c = rex::thread::Event::CreateAutoResetEvent(false);
    rex::thread::WaitHandle* hs[3] = {a.get(), b.get(), c.get()};
    std::thread s([&] {
      if (round & 1) std::this_thread::sleep_for(100us);
      c->Set();
    });
    auto r = rex::thread::WaitAny(hs, 3, false, kGuard);
    s.join();
    CHECK(r.first == WaitResult::kSuccess && r.second == 2);
    CHECK(rex::thread::WaitAny(hs, 3, false, 0ms).first == WaitResult::kTimeout);  // auto-reset consumed
  }
  // All: only when every handle is signalled; consumes all of them.
  {
    auto a = rex::thread::Event::CreateAutoResetEvent(true);
    auto b = rex::thread::Event::CreateAutoResetEvent(false);
    auto s = rex::thread::Semaphore::Create(1, 1);
    rex::thread::WaitHandle* hs[3] = {a.get(), b.get(), s.get()};
    CHECK(rex::thread::WaitAll(hs, 3, false, 2ms) == WaitResult::kTimeout);
    std::thread t([&] {
      std::this_thread::sleep_for(2ms);
      b->Set();
    });
    CHECK(rex::thread::WaitAll(hs, 3, false, kGuard) == WaitResult::kSuccess);
    t.join();
    CHECK(rex::thread::Wait(a.get(), false, 0ms) == WaitResult::kTimeout);
    CHECK(rex::thread::Wait(s.get(), false, 0ms) == WaitResult::kTimeout);
  }
  // More than 64 handles (heap fallback): the last one.
  {
    std::vector<std::unique_ptr<rex::thread::Event>> events;
    std::vector<rex::thread::WaitHandle*> hs;
    for (int i = 0; i < 70; ++i) {
      events.push_back(rex::thread::Event::CreateManualResetEvent(false));
      hs.push_back(events.back().get());
    }
    CHECK(rex::thread::WaitAny(hs.data(), hs.size(), false, 1ms).first == WaitResult::kTimeout);
    events[69]->Set();
    auto r = rex::thread::WaitAny(hs.data(), hs.size(), false, kGuard);
    CHECK(r.first == WaitResult::kSuccess && r.second == 69);
    for (auto& e : events) e->Set();
    CHECK(rex::thread::WaitAll(hs.data(), hs.size(), false, kGuard) == WaitResult::kSuccess);
    // The vector overloads still work.
    CHECK(rex::thread::WaitAny(hs, false, 0ms).second == 0);
  }
  // One handle: timeout and index 0; a null handle fails.
  {
    auto e = rex::thread::Event::CreateAutoResetEvent(false);
    rex::thread::WaitHandle* hs[1] = {e.get()};
    const auto t0 = std::chrono::steady_clock::now();
    auto r = rex::thread::WaitAny(hs, 1, false, 3ms);
    CHECK(r.first == WaitResult::kTimeout && r.second == 0);
    CHECK(std::chrono::steady_clock::now() - t0 >= 3ms);
    e->Set();
    r = rex::thread::WaitAny(hs, 1, false, 0ms);
    CHECK(r.first == WaitResult::kSuccess && r.second == 0);
    rex::thread::WaitHandle* bad[2] = {e.get(), nullptr};
    CHECK(rex::thread::WaitAny(bad, 2, false, 0ms).first == WaitResult::kFailed);
  }
  // Alertable, one and several handles, signalled from another thread and timing out.
  {
    auto a = rex::thread::Event::CreateAutoResetEvent(false);
    auto b = rex::thread::Event::CreateAutoResetEvent(false);
    rex::thread::WaitHandle* one[1] = {a.get()};
    rex::thread::WaitHandle* two[2] = {a.get(), b.get()};
    CHECK(rex::thread::WaitAny(one, 1, true, 2ms).first == WaitResult::kTimeout);
    CHECK(rex::thread::WaitAny(two, 2, true, 2ms).first == WaitResult::kTimeout);
    std::thread t([&] {
      std::this_thread::sleep_for(2ms);
      b->Set();
    });
    auto r = rex::thread::WaitAny(two, 2, true, kGuard);
    t.join();
    CHECK(r.first == WaitResult::kSuccess && r.second == 1);
    std::thread u([&] {
      std::this_thread::sleep_for(2ms);
      a->Set();
    });
    r = rex::thread::WaitAny(one, 1, true, kGuard);
    u.join();
    CHECK(r.first == WaitResult::kSuccess && r.second == 0);
  }
}

void TestSignalAndWait() {
  auto s = rex::thread::Event::CreateAutoResetEvent(false);
  auto w = rex::thread::Event::CreateAutoResetEvent(false);
  std::thread peer([&] {
    CHECK(rex::thread::Wait(s.get(), false, kGuard) == WaitResult::kSuccess);
    w->Set();
  });
  CHECK(rex::thread::SignalAndWait(s.get(), w.get(), false, kGuard) == WaitResult::kSuccess);
  peer.join();
}

}  // namespace

int main() {
  // A lost wake-up in a wait without timeout would hang: fail the test instead.
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(120));
    std::fprintf(stderr, "test_kernel_waits: still running after 120 s (lost wake-up?)\n");
    std::_Exit(2);
  }).detach();
  for (const bool blocking : {true, false}) {
    REXCVAR_SET(thread_wait_blocking, blocking);
    TestHandlesRecordTheirCondition();
    TestEventNoLostWakeup();
    TestSemaphoreNoLostWakeup();
    TestMutantExclusionAndHandoff();
    TestWaitMultiple();
    TestSignalAndWait();
  }
  if (g_failures) {
    std::fprintf(stderr, "test_kernel_waits: %d failure(s)\n", g_failures.load());
    return 1;
  }
  std::printf("test_kernel_waits: ok\n");
  return 0;
}

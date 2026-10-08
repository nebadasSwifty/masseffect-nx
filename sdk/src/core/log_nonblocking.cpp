/**
 * @file        core/log_nonblocking.cpp
 * @brief       Non-blocking rotating file sink (log_nonblocking)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * Why: with the synchronous rotating_file_sink_mt, the thread that logs formats the line, takes the
 * sink mutex and writes to the SD card itself (fwrite, and fflush on every warn line because of
 * flush_on(warn)). Stack sampling during a Feros firefight found the "GPU ring native" thread, the
 * CPU bottleneck, spending ~14 % of its time in frames over 45 ms inside fsdev_write/fsdev_seek,
 * at only ~27 lines/s. log_async (spdlog's queue) is no answer on Horizon: its overflow policy blocks
 * and its pool thread inherits a priority at which it starves (see logging.cpp, log_async).
 *
 * Design (log_nonblocking = true):
 *   - The logging thread formats the line itself, with a per-thread clone of the sink's formatter
 *     (no lock), and pushes the text into a lock-free multi-producer byte ring (log_ring.h). That is
 *     all it does: no mutex, no stdio, no SD. If the ring is full the line is dropped and counted.
 *   - flush() (called on every warn line through flush_on, and by spdlog's flush_every thread) only
 *     raises a flag that makes the writer run its next batch early.
 *   - A dedicated writer thread (libnx threadCreate on Switch, never std::thread there) wakes every
 *     20 ms, and every log_nonblocking_interval_ms (or earlier on a flush request or when the ring is
 *     half full) moves everything queued into a 256 KB batch, writes it with one fwrite per batch and
 *     one fflush, and rotates the file like rotating_file_sink (base.log -> base.1.log ...). Its
 *     priority is explicit (log_nonblocking_priority, 0x2C by default: below the audio output and XMA
 *     threads 0x2B so it never delays them, level with presentation/ring 0x2C, above the guest threads
 *     0x3B; it is mostly blocked in the fs IPC, and if starved it only drops lines) and its core mask
 *     keeps it off core 2, the core masseffect_exclusive_core gives to the game's main thread.
 *   - Drops are reported in the file by the writer ("[log] N lines dropped ...").
 *   - Orderly exit: FlushLogging() drains synchronously from the calling thread. Crash paths
 *     (RexSwitchCrashLog: abort, exit, std::terminate, fatal faults) call RexLogEmergencyDrain(),
 *     which waits at most ~300 ms for the writer to finish its batch, then writes what is queued
 *     without rotating (no fopen/rename, so no malloc, in a context where the heap lock may be held).
 *
 * Known limits:
 *   - A thread suspended between reserving and publishing its record holds back the lines behind it
 *     until it resumes (they are not lost; the ring may fill and drop meanwhile).
 *   - Each thread that logs leaks one small formatter clone when it exits (no thread_local
 *     destructors on purpose: they are not something to rely on for guest threads on Horizon).
 *   - Lines longer than 16 KB are truncated.
 */

#include "log_nonblocking.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include <spdlog/details/os.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include "log_ring.h"

#ifdef __SWITCH__
#include <switch.h>
// switch_perf.cpp (in the executable): the name shown for the thread in logs/rex/rex_profile.log.
extern "C" void RexSwitchPerfSetThreadName(u32 handle, const char* name);
#endif

namespace rex::log_nb {

namespace {

constexpr size_t kMaxLineBytes = 16 * 1024;
constexpr size_t kBatchBytes = 256 * 1024;
constexpr size_t kFileBufferBytes = 64 * 1024;
constexpr uint32_t kStepMs = 20;

// true on the writer thread (trivial thread_local: no destructor involved).
thread_local bool t_is_writer = false;

void SleepMs(uint32_t ms) {
#ifdef __SWITCH__
  svcSleepThread(static_cast<int64_t>(ms) * 1000000LL);
#else
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
}

class NonblockingFileSink;
std::atomic<NonblockingFileSink*> g_sink{nullptr};

class NonblockingFileSink final : public spdlog::sinks::sink {
 public:
  explicit NonblockingFileSink(const NonblockingFileSinkOptions& opts)
      : opts_(opts),
        ring_(std::max<size_t>(opts.ring_bytes, kBatchBytes)),
        formatter_(std::make_unique<spdlog::pattern_formatter>()) {
    batch_.reset(new char[kBatchBytes]);
    file_buffer_.reset(new char[kFileBufferBytes]);
  }

  ~NonblockingFileSink() override {
    Stop();
    if (file_) std::fclose(file_);
  }

  bool Open() {
    file_ = std::fopen(opts_.path.c_str(), "ab");
    if (!file_) return false;
    std::setvbuf(file_, file_buffer_.get(), _IOFBF, kFileBufferBytes);
    std::fseek(file_, 0, SEEK_END);
    const long pos = std::ftell(file_);
    file_size_ = pos > 0 ? static_cast<size_t>(pos) : 0;
    return true;
  }

  bool Start() {
    stop_.store(false, std::memory_order_relaxed);
#ifdef __SWITCH__
    if (R_FAILED(threadCreate(&thread_, &NonblockingFileSink::ThreadEntry, this, nullptr, 64 * 1024,
                              opts_.writer_priority, -2))) {
      return false;
    }
    RexSwitchPerfSetThreadName(thread_.handle, "log writer");
    const u32 mask = static_cast<u32>(opts_.writer_core_mask & 0x7);
    if (mask) {
      // The ideal core has to be inside the mask: take the lowest core of the mask.
      s32 ideal = 0;
      while (!(mask & (1u << ideal))) ++ideal;
      svcSetThreadCoreMask(thread_.handle, ideal, mask);
    }
    if (R_FAILED(threadStart(&thread_))) {
      threadClose(&thread_);
      return false;
    }
    thread_started_ = true;
#else
    thread_ = std::thread([this] { Run(); });
    thread_started_ = true;
#endif
    return true;
  }

  void Stop() {
    if (!thread_started_) return;
    stop_.store(true, std::memory_order_release);
#ifdef __SWITCH__
    threadWaitForExit(&thread_);
    threadClose(&thread_);
#else
    if (thread_.joinable()) thread_.join();
#endif
    thread_started_ = false;
  }

  // --- spdlog::sinks::sink -----------------------------------------------------------------------

  void log(const spdlog::details::log_msg& msg) override {
    spdlog::memory_buf_t text;
    FormatterForThisThread()->format(msg, text);
    size_t n = text.size();
    if (n > kMaxLineBytes) {
      n = kMaxLineBytes;
      text[n - 1] = '\n';
    }
    if (!ring_.Push(text.data(), n)) {
      dropped_lines_.fetch_add(1, std::memory_order_relaxed);
      dropped_bytes_.fetch_add(n, std::memory_order_relaxed);
      urgent_.store(true, std::memory_order_relaxed);
      return;
    }
    if (ring_.used() > ring_.capacity() / 2) urgent_.store(true, std::memory_order_relaxed);
  }

  // Never touches the file: the writer picks the request up within one step (20 ms).
  void flush() override { urgent_.store(true, std::memory_order_relaxed); }

  void set_pattern(const std::string& pattern) override {
    set_formatter(std::make_unique<spdlog::pattern_formatter>(pattern));
  }

  void set_formatter(std::unique_ptr<spdlog::formatter> f) override {
    std::lock_guard lock(formatter_mutex_);
    formatter_ = std::move(f);
    formatter_generation_.fetch_add(1, std::memory_order_release);
  }

  // --- draining ----------------------------------------------------------------------------------

  // Synchronous drain from any thread. crash: bounded wait, no rotation, never from the writer.
  void DrainSync(bool crash) {
    if (crash && t_is_writer) return;  // the writer faulted, maybe holding the file: leave it
    const uint32_t max_wait_ms = crash ? 300 : 5000;
    if (!LockFile(max_wait_ms)) return;
    if (crash) {
      static const char kNote[] = "[log] crash: emergency drain of the non-blocking log follows\n";
      WriteRaw(kNote, sizeof(kNote) - 1, /*allow_rotate=*/false);
    }
    WriteQueued(/*allow_rotate=*/!crash);
    if (file_) std::fflush(file_);
    UnlockFile();
  }

  // Developer USB file channel (app/src/me_usb_files.cpp): Horizon's FS refuses a second open of a file that is
  // open for writing, so the current log cannot be read by path while the game runs. With the file lock held,
  // write what is queued, close the file, read [offset, offset + len) through a read-only handle, and reopen it
  // for appending (as Rotate does). -1: path is not the current log; -2: lock busy; -3: the file cannot be read.
  // *size_out (if given) is the file size after the queued lines were written.
  int64_t ReadCurrentFile(const char* path, uint64_t offset, void* buf, size_t len, uint64_t* size_out) {
    if (!SamePath(path, opts_.path.c_str())) return -1;
    if (!LockFile(2000)) return -2;
    WriteQueued(/*allow_rotate=*/false);
    if (file_) {
      std::fclose(file_);
      file_ = nullptr;
    }
    int64_t got = -3;
    if (std::FILE* r = std::fopen(opts_.path.c_str(), "rb")) {
      std::fseek(r, 0, SEEK_END);
      const long end = std::ftell(r);
      const uint64_t size = end > 0 ? static_cast<uint64_t>(end) : 0;
      if (size_out) *size_out = size;
      got = 0;
      if (buf && len && offset < size && std::fseek(r, static_cast<long>(offset), SEEK_SET) == 0) {
        got = static_cast<int64_t>(std::fread(buf, 1, std::min<uint64_t>(len, size - offset), r));
      }
      std::fclose(r);
    }
    file_ = std::fopen(opts_.path.c_str(), "ab");
    if (file_) std::setvbuf(file_, file_buffer_.get(), _IOFBF, kFileBufferBytes);
    UnlockFile();
    return got;
  }

 private:
  // Same file: equal after dropping a leading "sdmc:" and collapsing repeated '/'.
  static bool SamePath(const char* a, const char* b) {
    auto skip_device = [](const char* p) { return std::strncmp(p, "sdmc:", 5) == 0 ? p + 5 : p; };
    if (!a || !b) return false;
    a = skip_device(a);
    b = skip_device(b);
    for (;;) {
      while (a[0] == '/' && a[1] == '/') ++a;
      while (b[0] == '/' && b[1] == '/') ++b;
      if (*a != *b) return false;
      if (!*a) return true;
      ++a;
      ++b;
    }
  }

#ifdef __SWITCH__
  static void ThreadEntry(void* arg) { static_cast<NonblockingFileSink*>(arg)->Run(); }
#endif

  void Run() {
    t_is_writer = true;
    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();
    const auto interval = std::chrono::milliseconds(opts_.interval_ms);
    for (;;) {
      const bool stopping = stop_.load(std::memory_order_acquire);
      const auto now = Clock::now();
      if (stopping || urgent_.load(std::memory_order_relaxed) || now - last >= interval) {
        urgent_.store(false, std::memory_order_relaxed);
        last = now;
        if (LockFile(stopping ? 5000 : 0)) {
          WriteQueued(/*allow_rotate=*/true);
          if (file_) std::fflush(file_);
          UnlockFile();
        }
      }
      if (stopping) break;
      SleepMs(kStepMs);
    }
  }

  // The file lock serializes the consumers of the ring (the writer and the synchronous drains) and
  // the use of file_. It is never taken by a thread that only logs. Waiting sleeps, never spins:
  // a spinning high-priority thread would starve the holder on Horizon.
  bool LockFile(uint32_t max_wait_ms) {
    for (uint32_t waited = 0;; ++waited) {
      bool expected = false;
      if (file_busy_.compare_exchange_strong(expected, true, std::memory_order_acquire)) return true;
      if (waited >= max_wait_ms) return false;
      SleepMs(1);
    }
  }
  void UnlockFile() { file_busy_.store(false, std::memory_order_release); }

  // With the file lock held.
  void WriteQueued(bool allow_rotate) {
    for (;;) {
      const size_t n = ring_.Pop(batch_.get(), kBatchBytes);
      if (n == 0) break;
      WriteRaw(batch_.get(), n, allow_rotate);
    }
    const uint64_t lines = dropped_lines_.exchange(0, std::memory_order_relaxed);
    if (lines) {
      const uint64_t bytes = dropped_bytes_.exchange(0, std::memory_order_relaxed);
      char note[192];
      const int len = std::snprintf(note, sizeof(note),
                                    "[log] %" PRIu64 " lines (%" PRIu64
                                    " bytes) dropped: the non-blocking log ring (%zu KB) was full\n",
                                    lines, bytes, ring_.capacity() / 1024);
      if (len > 0) WriteRaw(note, std::min(sizeof(note) - 1, static_cast<size_t>(len)), allow_rotate);
    }
  }

  // With the file lock held. One fwrite per batch; rotation as in rotating_file_sink.
  void WriteRaw(const char* data, size_t n, bool allow_rotate) {
    if (allow_rotate && file_size_ > 0 && file_size_ + n > opts_.max_file_bytes) Rotate();
    if (!file_) return;
    std::fwrite(data, 1, n, file_);
    file_size_ += n;
  }

  // base.log -> base.1.log -> ... -> base.<max_files>.log (the oldest is removed), then a new base.log.
  void Rotate() {
    using spdlog::sinks::rotating_file_sink_st;
    if (file_) {
      std::fclose(file_);
      file_ = nullptr;
    }
    for (size_t i = opts_.max_files; i > 0; --i) {
      const spdlog::filename_t src = rotating_file_sink_st::calc_filename(opts_.path, i - 1);
      if (!spdlog::details::os::path_exists(src)) continue;
      const spdlog::filename_t target = rotating_file_sink_st::calc_filename(opts_.path, i);
      (void)spdlog::details::os::remove(target);
      (void)spdlog::details::os::rename(src, target);
    }
    file_ = std::fopen(opts_.path.c_str(), "wb");
    if (file_) std::setvbuf(file_, file_buffer_.get(), _IOFBF, kFileBufferBytes);
    file_size_ = 0;
  }

  // Per-thread clone of the formatter: pattern_formatter caches the time of the last line and is not
  // thread-safe. The lock is taken only the first time a thread logs or after a pattern change.
  struct ThreadFormatter {
    const NonblockingFileSink* owner = nullptr;
    uint64_t generation = ~uint64_t(0);
    std::unique_ptr<spdlog::formatter> formatter;
  };
  spdlog::formatter* FormatterForThisThread() {
    static thread_local ThreadFormatter* tls = nullptr;  // trivial: see "Known limits" above
    const uint64_t gen = formatter_generation_.load(std::memory_order_acquire);
    if (!tls) tls = new ThreadFormatter();
    if (tls->owner != this || tls->generation != gen || !tls->formatter) {
      std::lock_guard lock(formatter_mutex_);
      tls->formatter = formatter_->clone();
      tls->generation = formatter_generation_.load(std::memory_order_relaxed);
      tls->owner = this;
    }
    return tls->formatter.get();
  }

  NonblockingFileSinkOptions opts_;
  MpscByteRing ring_;

  std::mutex formatter_mutex_;
  std::unique_ptr<spdlog::formatter> formatter_;
  std::atomic<uint64_t> formatter_generation_{0};

  std::atomic<uint64_t> dropped_lines_{0};
  std::atomic<uint64_t> dropped_bytes_{0};
  std::atomic<bool> urgent_{false};
  std::atomic<bool> stop_{false};
  std::atomic<bool> file_busy_{false};

  std::FILE* file_ = nullptr;
  size_t file_size_ = 0;
  std::unique_ptr<char[]> batch_;
  std::unique_ptr<char[]> file_buffer_;

  bool thread_started_ = false;
#ifdef __SWITCH__
  Thread thread_{};
#else
  std::thread thread_;
#endif
};

// Unregisters the global before the sink is destroyed.
struct SinkDeleter {
  void operator()(NonblockingFileSink* s) const {
    NonblockingFileSink* expected = s;
    g_sink.compare_exchange_strong(expected, nullptr);
    delete s;
  }
};

}  // namespace

std::shared_ptr<spdlog::sinks::sink> CreateNonblockingFileSink(const NonblockingFileSinkOptions& opts) {
  if (g_sink.load(std::memory_order_acquire)) return nullptr;  // only one at a time
  std::shared_ptr<NonblockingFileSink> sink(new NonblockingFileSink(opts), SinkDeleter{});
  if (!sink->Open() || !sink->Start()) return nullptr;
  g_sink.store(sink.get(), std::memory_order_release);
  return sink;
}

void DrainNonblockingFileSink() {
  if (NonblockingFileSink* s = g_sink.load(std::memory_order_acquire)) s->DrainSync(false);
}

}  // namespace rex::log_nb

extern "C" long long RexLogReadCurrentFile(const char* path, unsigned long long offset, void* buf, size_t len,
                                          unsigned long long* size_out) {
  auto* s = rex::log_nb::g_sink.load(std::memory_order_acquire);
  if (!s) return -1;
  uint64_t size = 0;
  const int64_t got = s->ReadCurrentFile(path, offset, buf, len, &size);
  if (size_out && got >= 0) *size_out = size;
  return got;
}

extern "C" void RexLogEmergencyDrain(void) {
  if (auto* s = rex::log_nb::g_sink.load(std::memory_order_acquire)) s->DrainSync(true);
}

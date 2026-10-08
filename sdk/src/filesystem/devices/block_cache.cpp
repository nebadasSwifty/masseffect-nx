/**
 * @file        filesystem/devices/block_cache.cpp
 * @brief       RAM block cache + coalesced read-ahead for read-only game files. docs/streaming-io.md.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * What the console logs show (Mass Effect, manual runs 2026-10-07/08, details in docs/streaming-io.md):
 *  - every package read during gameplay comes from ONE guest thread, the UE3 async IO thread
 *    (sub_8239E260 -> sub_8231C7B0 -> sub_826DC260 -> NtReadFile). Its requests are split into
 *    128 KB pieces (1,184 of 1,314 logged slow reads are exactly 131,072 bytes; most of the rest are the
 *    tails of a request) and EVERY offset is a multiple of 32 KB;
 *  - after the first minute 33-69 % of the bytes read per session (81-84 % in single 15 s windows)
 *    are exact rereads of ranges read earlier (texture mips streamed out and back in), and reads also overlap earlier ones at a different
 *    32 KB-aligned start, which an exact-range cache cannot see;
 *  - a 128 KB pread costs 3-6 ms on average and 15-78 ms in the tail on the Switch SD
 *    (fixed ~2.3 ms per call + ~70 MB/s), so per-call overhead is about half of the time.
 *
 * Why the earlier block cache (masseffect_io_cache_mb, host_path_file.cpp) failed and why this one
 * is different:
 *  - block size: it used 256 KB blocks against reads that were 1 % aligned to 256 KB, so it read
 *    x3.3 more than asked. Here the block is 32 KB by default, the game's own alignment: a demand
 *    miss reads at most the request rounded up to 32 KB (only the tail of a request can round up);
 *  - lock: it held one mutex across the pread. Here the lock covers only the table and memcpy of
 *    blocks; every SD read happens with the lock released;
 *  - read-ahead is separate, off by default and audited: blocks brought in by read-ahead are
 *    flagged, a hit clears the flag (used), and the amount of read-ahead adapts to the measured
 *    used/issued ratio, so a pattern that does not continue sequentially turns it off by itself.
 *
 * Exactness: the cache only answers reads that lie completely inside the file size recorded by the
 * VFS (the same size the guest sees); anything else goes down the usual path. Blocks hold the bytes
 * the SD returned; a block shorter than the block size exists only where the SD returned end of
 * file, and then the answer is cut there, exactly like the direct pread loop would. The guest's
 * status block, event and APC are still written by NtReadFile after ReadSync returns, so completion
 * semantics are unchanged.
 *
 * Threads: the guest reads with its own handle on its own thread. The optional async read-ahead
 * (masseffect_io_bcache_async, Switch only) runs on one libnx thread (threadCreate, never
 * std::thread) that opens its OWN handles: newlib's pread on Horizon is lseek+read+lseek, so a
 * handle shared by two threads would read the wrong bytes.
 */

#include <rex/filesystem/block_cache.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>

#if REX_PLATFORM_SWITCH
#include <switch.h>
// switch_perf.cpp (in the executable): the name shown for the thread in rex_profile.log.
extern "C" void RexSwitchPerfSetThreadName(u32 handle, const char* name);
// guest_memory_switch.cpp: the always-mapped alias of a guest window address (NULL if uncommitted).
extern "C" void* RexGmShadowFor(uint64_t window_address);
#endif

REXCVAR_DEFINE_INT32(masseffect_io_bcache_mb, 0, "Filesystem",
                     "RAM block cache for read-only game files, cap in MB (0 = off). Read once at the "
                     "first file open. See docs/streaming-io.md.");
REXCVAR_DEFINE_INT32(masseffect_io_bcache_block_kb, 32, "Filesystem",
                     "Block size of masseffect_io_bcache_mb in KB (power of two, 4-1024). 32 = the "
                     "alignment of every game read; larger blocks over-read.");
REXCVAR_DEFINE_INT32(masseffect_io_bcache_max_request_kb, 1024, "Filesystem",
                     "Reads larger than this many KB bypass the block cache (the game's streaming "
                     "reads are 128 KB).");
REXCVAR_DEFINE_INT32(masseffect_io_bcache_readahead_kb, 0, "Filesystem",
                     "Read-ahead after a request that ends on a block boundary, in KB (0 = off). "
                     "Synchronous: appended to the SD read of a miss. Adaptive (see _adaptive).");
REXCVAR_DEFINE_INT32(masseffect_io_bcache_readahead_min_kb, 64, "Filesystem",
                     "Only requests of at least this many KB trigger read-ahead (a 128 KB piece of a "
                     "longer request does; the short tail of a request does not).");
REXCVAR_DEFINE_BOOL(masseffect_io_bcache_adaptive, true, "Filesystem",
                    "Halve the read-ahead when fewer than 30 % of read-ahead blocks get used, grow it "
                    "back above 60 %.");
REXCVAR_DEFINE_BOOL(masseffect_io_bcache_async, false, "Filesystem",
                    "Switch only: do the read-ahead on a dedicated prefetch thread after the guest's "
                    "read returns, instead of inside it.");
REXCVAR_DEFINE_INT32(masseffect_io_bcache_async_priority, 44, "Filesystem",
                     "Horizon priority of the prefetch thread (it is mostly blocked in the FS IPC).");
REXCVAR_DEFINE_INT32(masseffect_io_bcache_wait_ms, 100, "Filesystem",
                     "Longest a guest read waits for a block another thread is reading; then it "
                     "reads the block itself.");
REXCVAR_DEFINE_STRING(masseffect_io_bcache_skip, ".bik", "Filesystem",
                      "Comma separated, case-insensitive substrings of guest paths that never go "
                      "through the block cache (movies are streamed once).");
REXCVAR_DEFINE_INT32(masseffect_io_bcache_verify_n, 512, "Filesystem",
                     "The first N reads the block cache answers are also read from the SD and compared, "
                     "both the bytes the cache assembled and the bytes that ended up in the guest's "
                     "buffer. On any difference: '[io] block cache: DIFFERENCE' + the cache turns itself "
                     "off for the session (the read is redone from the SD). 0 = never, -1 = every read.");
REXCVAR_DEFINE_BOOL(masseffect_io_bcache_checksum, true, "Filesystem",
                    "Hash every block when it is stored and check it on every hit: a block whose RAM "
                    "changed after it was stored is reported as a DIFFERENCE and the cache turns off.");
REXCVAR_DEFINE_INT32(masseffect_io_bcache_guest_write, 1, "Filesystem",
                     "How the answer is written into the guest's buffer. 1 = Switch: through the guest "
                     "memory shadow alias, page by page (never faults, like the SD/kernel write of the "
                     "direct path; ReadInternal still triggers the physical write callbacks). 0 = plain "
                     "memcpy to the guest pointer (may fault on pages not yet mapped in that view).");
REXCVAR_DEFINE_BOOL(masseffect_io_bcache_arena, true, "Filesystem",
                    "Take the whole cap in ONE allocation at the first file open (before the game frees "
                    "any guest memory) instead of 4 MB slabs on demand: cache blocks can then never sit "
                    "in heap memory that was guest backing earlier. Falls back to slabs if it fails.");
REXCVAR_DEFINE_BOOL(masseffect_io_bcache_ghost, false, "Filesystem",
                    "Metadata-only LRU simulation of block caches of 32/64/128/256/512 MB: logs the "
                    "hit rate each size would get, with no data stored. Works with the cache off.");

namespace rex::filesystem::block_cache {
namespace {

constexpr size_t kSlabBytes = 4u << 20;
#if REX_PLATFORM_SWITCH
constexpr size_t kMaxJobs = 8;
constexpr size_t kWorkerHandles = 8;
#endif
constexpr uint64_t kAdaptWindow = 512;       // read-ahead blocks issued per adaptation step
constexpr uint64_t kReprobeMissBlocks = 2048;  // demand-missed blocks before read-ahead is retried
constexpr uint64_t kGhostMb[Stats::kGhostSizes] = {32, 64, 128, 256, 512};

inline uint64_t NowUs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

struct FileRec {
  std::string guest;
  std::filesystem::path host;
  uint64_t size = 0;
};

enum : uint8_t { kFree = 0, kValid = 1, kPending = 2 };

struct Slot {
  uint64_t key = 0;
  uint8_t* data = nullptr;
  uint32_t len = 0;       // valid bytes; < block only where the SD returned end of file
  uint32_t job = 0;       // pending: async job that owns it (0 = a guest thread is reading it)
  int32_t prev = -1;      // LRU links, valid slots only
  int32_t next = -1;
  uint8_t state = kFree;
  bool ahead = false;     // brought by read-ahead and not hit yet
  uint64_t sum = 0;       // BlockSum of the bytes when stored (masseffect_io_bcache_checksum)
};

struct Job {
  uint32_t seq = 0;
  uint32_t id = 0;
  uint64_t first = 0;
  uint32_t count = 0;
  bool cancelled = false;
};

struct Counters {
  std::atomic<uint64_t> reads{0}, full_hit{0}, partial{0}, miss{0}, bypass{0};
  std::atomic<uint64_t> bytes_req{0}, bytes_ram{0};
  std::atomic<uint64_t> sd_calls{0}, sd_demand{0}, sd_ahead{0}, sd_us{0};
  std::atomic<uint64_t> ahead_used{0}, ahead_wasted{0}, evictions{0};
  std::atomic<uint64_t> waits{0}, wait_us{0}, wait_timeouts{0};
  std::atomic<uint64_t> jobs_queued{0}, jobs_dropped{0}, jobs_cancelled{0};
  std::atomic<uint64_t> worker_opens{0}, worker_open_us{0};
  std::atomic<uint64_t> verified{0}, verify_budget_used{0}, sums_checked{0};
};
Counters c;

struct GhostLru {
  size_t cap = 0;  // blocks
  std::list<uint64_t> order;  // front = most recent
  std::unordered_map<uint64_t, std::list<uint64_t>::iterator> where;
  uint64_t hits = 0;
};

struct State {
  std::mutex mu;
  std::condition_variable cv;       // a pending block became valid or was released
  std::condition_variable cv_jobs;  // work for the prefetch thread

  bool inited = false;
  uint32_t block = 0;
  uint32_t shift = 0;
  size_t max_blocks = 0;
  bool ghost = false;
  bool async = false;
  bool checksum = false;

  std::vector<FileRec> files;  // id - 1
  std::unordered_map<std::string, uint32_t> ids;

  std::vector<Slot> slots;  // reserved to max_blocks up front: never reallocates
  std::vector<int32_t> free_slots;
  std::vector<std::unique_ptr<uint8_t[]>> slabs;
  std::unique_ptr<uint8_t[]> arena;  // masseffect_io_bcache_arena: all block storage, taken at init
  size_t arena_blocks_used = 0;
  std::unordered_map<uint64_t, int32_t> index;
  int32_t head = -1;  // most recently used
  int32_t tail = -1;  // least recently used
  uint64_t live = 0;
  uint64_t slab_bytes = 0;
  bool slab_failed = false;

  // Adaptive read-ahead, in blocks. Issued/used are counted per window of kAdaptWindow issued.
  int32_t ahead_blocks = 0;
  int32_t ahead_max = 0;
  uint64_t win_issued = 0;
  uint64_t win_used = 0;
  uint64_t miss_blocks_while_off = 0;

  std::deque<Job> jobs;
  uint32_t job_seq = 0;
  uint32_t running_job = 0;
  bool worker_started = false;
  bool worker_failed = false;
};
State g;

// Set once by a DIFFERENCE: from then on every read takes the usual path.
std::atomic<bool> g_disabled{false};

std::mutex g_ghost_mu;
GhostLru g_ghost[Stats::kGhostSizes];
std::atomic<uint64_t> g_ghost_accesses{0};

inline uint64_t Key(uint32_t id, uint64_t block) { return (uint64_t(id) << 40) | block; }

// ---- LRU (g.mu held) ---------------------------------------------------------------------------

void LruRemove(int32_t i) {
  Slot& s = g.slots[i];
  if (s.prev >= 0) g.slots[s.prev].next = s.next; else g.head = s.next;
  if (s.next >= 0) g.slots[s.next].prev = s.prev; else g.tail = s.prev;
  s.prev = s.next = -1;
}

void LruPushFront(int32_t i) {
  Slot& s = g.slots[i];
  s.prev = -1;
  s.next = g.head;
  if (g.head >= 0) g.slots[g.head].prev = i;
  g.head = i;
  if (g.tail < 0) g.tail = i;
}

void AdaptAfterIssue(uint64_t issued) {
  g.win_issued += issued;
  if (!REXCVAR_GET(masseffect_io_bcache_adaptive) || g.win_issued < kAdaptWindow) {
    return;
  }
  const double ratio = double(g.win_used) / double(g.win_issued);
  const int32_t before = g.ahead_blocks;
  if (ratio < 0.30) {
    g.ahead_blocks /= 2;
  } else if (ratio > 0.60 && g.ahead_blocks < g.ahead_max) {
    g.ahead_blocks = std::min(g.ahead_max, std::max(1, g.ahead_blocks * 2));
  }
  if (g.ahead_blocks != before) {
    REXFS_INFO("[io] block cache: read-ahead {} -> {} KB ({:.0f} % of the last {} read-ahead blocks "
               "were used)",
               uint64_t(before) * g.block / 1024, uint64_t(g.ahead_blocks) * g.block / 1024,
               100.0 * ratio, g.win_issued);
  }
  g.win_issued = 0;
  g.win_used = 0;
  g.miss_blocks_while_off = 0;
}

void NoteHit(int32_t i) {
  Slot& s = g.slots[i];
  LruRemove(i);
  LruPushFront(i);
  if (s.ahead) {
    s.ahead = false;
    ++g.win_used;
    c.ahead_used.fetch_add(1, std::memory_order_relaxed);
  }
}

void ReleaseSlot(int32_t i) {
  Slot& s = g.slots[i];
  g.index.erase(s.key);
  s.state = kFree;
  s.job = 0;
  s.ahead = false;
  s.len = 0;
  g.free_slots.push_back(i);
}

// Fast 64-bit hash (not cryptographic): catches RAM that changed under a stored block.
uint64_t BlockSum(const uint8_t* p, size_t n) {
  uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t v;
    std::memcpy(&v, p + i, 8);
    h = (h ^ v) * 0xFF51AFD7ED558CCDull;
    h ^= h >> 29;
  }
  for (; i < n; ++i) {
    h = (h ^ p[i]) * 0x100000001B3ull;
  }
  return h;
}

void Publish(int32_t i, const uint8_t* src, uint32_t len) {
  Slot& s = g.slots[i];
  std::memcpy(s.data, src, len);
  s.sum = g.checksum ? BlockSum(s.data, len) : 0;
  s.len = len;
  s.state = kValid;
  s.job = 0;
  g.live += len;
  LruPushFront(i);
}

bool GrowSlab() {
  if (g.slab_failed || g.slots.size() >= g.max_blocks) {
    return false;
  }
  const size_t per_slab = std::max<size_t>(1, kSlabBytes / g.block);
  const size_t n = std::min(per_slab, g.max_blocks - g.slots.size());
  if (g.arena) {
    for (size_t k = 0; k < n; ++k) {
      Slot s;
      s.data = g.arena.get() + (g.arena_blocks_used + k) * size_t(g.block);
      g.slots.push_back(s);
      g.free_slots.push_back(int32_t(g.slots.size() - 1));
    }
    g.arena_blocks_used += n;
    g.slab_bytes += uint64_t(n) * g.block;
    return true;
  }
  std::unique_ptr<uint8_t[]> slab(new (std::nothrow) uint8_t[n * g.block]);
  if (!slab) {
    g.slab_failed = true;
    REXFS_WARN("[io] block cache: out of memory at {} MB, it stops growing there (cap {} MB). "
               "Reads keep working.",
               g.slab_bytes >> 20, (uint64_t(g.max_blocks) * g.block) >> 20);
    return false;
  }
  for (size_t k = 0; k < n; ++k) {
    Slot s;
    s.data = slab.get() + k * g.block;
    g.slots.push_back(s);
    g.free_slots.push_back(int32_t(g.slots.size() - 1));
  }
  g.slab_bytes += uint64_t(n) * g.block;
  g.slabs.push_back(std::move(slab));
  return true;
}

// A free slot (state kFree, not indexed), evicting the least recently used valid block if needed.
// -1 when every block is pending.
int32_t AcquireSlot() {
  if (g.free_slots.empty()) {
    GrowSlab();
  }
  if (!g.free_slots.empty()) {
    const int32_t i = g.free_slots.back();
    g.free_slots.pop_back();
    return i;
  }
  const int32_t i = g.tail;
  if (i < 0) {
    return -1;
  }
  Slot& s = g.slots[i];
  LruRemove(i);
  g.index.erase(s.key);
  g.live -= s.len;
  if (s.ahead) {
    c.ahead_wasted.fetch_add(1, std::memory_order_relaxed);
  }
  c.evictions.fetch_add(1, std::memory_order_relaxed);
  s.state = kFree;
  s.ahead = false;
  s.len = 0;
  return i;
}

int32_t ReservePending(uint64_t key, uint32_t job, bool ahead) {
  const int32_t i = AcquireSlot();
  if (i < 0) {
    return -1;
  }
  Slot& s = g.slots[i];
  s.key = key;
  s.state = kPending;
  s.job = job;
  s.ahead = ahead;
  g.index[key] = i;
  return i;
}

// Cancels a queued (not started) async job and releases its pending blocks.
void CancelJob(uint32_t seq) {
  for (auto& j : g.jobs) {
    if (j.seq != seq || j.cancelled) {
      continue;
    }
    j.cancelled = true;
    for (uint32_t k = 0; k < j.count; ++k) {
      auto it = g.index.find(Key(j.id, j.first + k));
      if (it != g.index.end() && g.slots[it->second].state == kPending &&
          g.slots[it->second].job == seq) {
        ReleaseSlot(it->second);
      }
    }
    c.jobs_cancelled.fetch_add(1, std::memory_order_relaxed);
    return;
  }
}

// ---- SD ------------------------------------------------------------------------------------------

// pread loop: a short read is not end of file (the FS can truncate); only 0 bytes is. Returns the
// bytes read, or -1 on an error before the first byte.
int64_t ReadSpan(FileHandle* fh, uint64_t offset, uint8_t* dst, size_t span) {
  size_t got = 0;
  const uint64_t t0 = NowUs();
  while (got < span) {
    size_t n = 0;
    if (!fh->Read(offset + got, dst + got, span - got, &n)) {
      if (got == 0) {
        c.sd_calls.fetch_add(1, std::memory_order_relaxed);
        c.sd_us.fetch_add(NowUs() - t0, std::memory_order_relaxed);
        return -1;
      }
      break;
    }
    c.sd_calls.fetch_add(1, std::memory_order_relaxed);
    if (n == 0) {
      break;
    }
    got += n;
  }
  c.sd_us.fetch_add(NowUs() - t0, std::memory_order_relaxed);
  return int64_t(got);
}

// ---- ghost ---------------------------------------------------------------------------------------

void GhostAccess(uint32_t id, uint64_t first, uint64_t count) {
  std::lock_guard lock(g_ghost_mu);
  g_ghost_accesses.fetch_add(count, std::memory_order_relaxed);
  for (auto& gl : g_ghost) {
    for (uint64_t b = first; b < first + count; ++b) {
      const uint64_t k = Key(id, b);
      auto it = gl.where.find(k);
      if (it != gl.where.end()) {
        ++gl.hits;
        gl.order.splice(gl.order.begin(), gl.order, it->second);
        continue;
      }
      gl.order.push_front(k);
      gl.where[k] = gl.order.begin();
      if (gl.order.size() > gl.cap) {
        gl.where.erase(gl.order.back());
        gl.order.pop_back();
      }
    }
  }
}

// ---- prefetch thread -----------------------------------------------------------------------------

#if REX_PLATFORM_SWITCH
Thread g_worker;

void WorkerMain(void*) {
  struct WorkerHandle {
    std::unique_ptr<FileHandle> fh;
    uint64_t used = 0;
  };
  std::unordered_map<uint32_t, WorkerHandle> handles;
  uint64_t tick = 0;
  std::vector<uint8_t> buffer;

  for (;;) {
    Job job;
    FileRec rec;
    {
      std::unique_lock lk(g.mu);
      g.cv_jobs.wait(lk, [] { return !g.jobs.empty(); });
      job = g.jobs.front();
      g.jobs.pop_front();
      if (job.cancelled) {
        continue;
      }
      g.running_job = job.seq;
      rec = g.files[job.id - 1];
    }

    WorkerHandle& h = handles[job.id];
    if (!h.fh) {
      if (handles.size() > kWorkerHandles) {
        auto oldest = handles.end();
        for (auto it = handles.begin(); it != handles.end(); ++it) {
          if (it->first != job.id && (oldest == handles.end() || it->second.used < oldest->second.used)) {
            oldest = it;
          }
        }
        if (oldest != handles.end()) {
          handles.erase(oldest);
        }
      }
      const uint64_t t0 = NowUs();
      handles[job.id].fh = FileHandle::OpenExisting(rec.host, FileAccess::kGenericRead);
      c.worker_opens.fetch_add(1, std::memory_order_relaxed);
      c.worker_open_us.fetch_add(NowUs() - t0, std::memory_order_relaxed);
    }
    WorkerHandle& hh = handles[job.id];
    hh.used = ++tick;

    const uint64_t start = job.first << g.shift;
    const size_t span = size_t(std::min<uint64_t>(uint64_t(job.count) << g.shift, rec.size - start));
    int64_t got = -1;
    if (hh.fh) {
      try {
        buffer.resize(span);
        got = ReadSpan(hh.fh.get(), start, buffer.data(), span);
      } catch (const std::bad_alloc&) {
        got = -1;
      }
    }
    if (got > 0) {
      c.sd_ahead.fetch_add(uint64_t(got), std::memory_order_relaxed);
    }

    {
      std::lock_guard lk(g.mu);
      uint64_t issued = 0;
      for (uint32_t k = 0; k < job.count; ++k) {
        auto it = g.index.find(Key(job.id, job.first + k));
        if (it == g.index.end()) continue;
        const int32_t i = it->second;
        if (g.slots[i].state != kPending || g.slots[i].job != job.seq) continue;
        const int64_t off = int64_t(k) << g.shift;
        const int64_t len = got > off ? std::min<int64_t>(got - off, g.block) : 0;
        if (len <= 0) {
          ReleaseSlot(i);
          continue;
        }
        Publish(i, buffer.data() + off, uint32_t(len));
        ++issued;
      }
      g.running_job = 0;
      AdaptAfterIssue(issued);
    }
    g.cv.notify_all();
  }
}

bool StartWorker() {
  // threadCreate, never std::thread: this is called from a guest thread.
  const int prio = std::clamp(REXCVAR_GET(masseffect_io_bcache_async_priority), 0x1C, 0x3F);
  if (R_FAILED(threadCreate(&g_worker, WorkerMain, nullptr, nullptr, 128 * 1024, prio, -2))) {
    return false;
  }
  RexSwitchPerfSetThreadName(g_worker.handle, "io prefetch");
  // Keep it off core 2 (the game's main thread owns it with masseffect_exclusive_core).
  svcSetThreadCoreMask(g_worker.handle, 0, 0x3);
  if (R_FAILED(threadStart(&g_worker))) {
    threadClose(&g_worker);
    return false;
  }
  return true;
}
#endif

// Queues blocks [first, first+count) of a file for the prefetch thread (g.mu held). Only the run of
// blocks that are not already present is queued.
void QueueAhead(uint32_t id, uint64_t first, uint32_t count, uint64_t size) {
#if REX_PLATFORM_SWITCH
  if (g.worker_failed) {
    return;
  }
  if (g.jobs.size() >= kMaxJobs) {
    c.jobs_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t seq = ++g.job_seq == 0 ? ++g.job_seq : g.job_seq;
  uint32_t n = 0;
  for (; n < count; ++n) {
    const uint64_t b = first + n;
    if ((b << g.shift) >= size || g.index.count(Key(id, b))) break;
    if (ReservePending(Key(id, b), seq, true) < 0) break;
  }
  if (!n) {
    return;
  }
  if (!g.worker_started) {
    g.worker_started = true;
    if (!StartWorker()) {
      g.worker_failed = true;
      REXFS_WARN("[io] block cache: could not create the prefetch thread; read-ahead stays synchronous");
      for (uint32_t k = 0; k < n; ++k) {
        ReleaseSlot(g.index[Key(id, first + k)]);
      }
      return;
    }
    REXFS_INFO("[io] block cache: prefetch thread started");
  }
  Job j;
  j.seq = seq;
  j.id = id;
  j.first = first;
  j.count = n;
  g.jobs.push_back(j);
  c.jobs_queued.fetch_add(1, std::memory_order_relaxed);
  g.cv_jobs.notify_one();
#else
  (void)id;
  (void)first;
  (void)count;
  (void)size;
#endif
}

bool PathSkipped(const std::string& guest_path) {
  std::string lower = guest_path;
  for (auto& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  const std::string& list = REXCVAR_GET(masseffect_io_bcache_skip);
  size_t pos = 0;
  while (pos <= list.size()) {
    size_t end = list.find(',', pos);
    if (end == std::string::npos) end = list.size();
    std::string item = list.substr(pos, end - pos);
    item.erase(0, item.find_first_not_of(' '));
    item.erase(item.find_last_not_of(' ') + 1);
    for (auto& ch : item) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (!item.empty() && lower.find(item) != std::string::npos) {
      return true;
    }
    pos = end + 1;
  }
  return false;
}

void InitLocked() {
  if (g.inited) {
    return;
  }
  g.inited = true;
  uint32_t kb = uint32_t(std::clamp(REXCVAR_GET(masseffect_io_bcache_block_kb), 4, 1024));
  uint32_t shift = 0;
  while ((1u << (shift + 1)) <= kb * 1024u) ++shift;
  g.shift = shift;
  g.block = 1u << shift;
  const int32_t mb = std::max(0, REXCVAR_GET(masseffect_io_bcache_mb));
  g.max_blocks = size_t(mb) * (1024u * 1024u) / g.block;
  g.slots.reserve(g.max_blocks);
  if (g.max_blocks && REXCVAR_GET(masseffect_io_bcache_arena)) {
    g.arena.reset(new (std::nothrow) uint8_t[g.max_blocks * size_t(g.block)]);
    if (g.arena) {
      REXFS_INFO("[io] block cache: arena of {} MB at {}", (uint64_t(g.max_blocks) * g.block) >> 20,
                 static_cast<const void*>(g.arena.get()));
    } else {
      REXFS_WARN("[io] block cache: could not take the {} MB arena; using 4 MB slabs on demand",
                 (uint64_t(g.max_blocks) * g.block) >> 20);
    }
  }
  g.ghost = REXCVAR_GET(masseffect_io_bcache_ghost);
  g.checksum = REXCVAR_GET(masseffect_io_bcache_checksum);
  for (int k = 0; k < Stats::kGhostSizes; ++k) {
    g_ghost[k].cap = size_t(kGhostMb[k] << 20) / g.block;
  }
  const int32_t ra_kb = std::max(0, REXCVAR_GET(masseffect_io_bcache_readahead_kb));
  g.ahead_max = int32_t((uint64_t(ra_kb) * 1024u + g.block - 1) / g.block);
  g.ahead_blocks = g.ahead_max;
#if REX_PLATFORM_SWITCH
  g.async = REXCVAR_GET(masseffect_io_bcache_async) && g.ahead_max > 0;
#endif
  REXFS_INFO("[io] block cache: cap {} MB, block {} KB, read-ahead {} KB ({}), max request {} KB, "
             "skip '{}', ghost {}",
             mb, g.block / 1024, uint64_t(g.ahead_max) * g.block / 1024,
             g.async ? "async thread" : "synchronous",
             REXCVAR_GET(masseffect_io_bcache_max_request_kb),
             REXCVAR_GET(masseffect_io_bcache_skip), g.ghost ? "on" : "off");
}

}  // namespace

uint32_t Register(const std::string& guest_path, const std::filesystem::path& host_path,
                  uint64_t size) {
  if (REXCVAR_GET(masseffect_io_bcache_mb) <= 0 && !REXCVAR_GET(masseffect_io_bcache_ghost)) {
    return 0;
  }
  if (size == 0 || PathSkipped(guest_path)) {
    return 0;
  }
  std::lock_guard lock(g.mu);
  InitLocked();
  // Keyed by the HOST path: two guest paths that resolve to one host file share blocks, and two
  // host files can never share an id.
  auto [it, inserted] = g.ids.try_emplace(host_path.string(), uint32_t(g.files.size() + 1));
  if (inserted) {
    FileRec r;
    r.guest = guest_path;
    r.host = host_path;
    r.size = size;
    g.files.push_back(std::move(r));
  } else if (g.files[it->second - 1].size != size) {
    // Read-only device: should never happen. Never mix bytes of two versions of a file.
    REXFS_WARN("[io] block cache: '{}' changed size ({} -> {}); it is no longer cached", guest_path,
               g.files[it->second - 1].size, size);
    return 0;
  }
  return it->second;
}

namespace {

// Writes the answer into the guest's buffer. See masseffect_io_bcache_guest_write.
void WriteGuest(uint8_t* dst, const uint8_t* src, size_t n) {
#if REX_PLATFORM_SWITCH
  if (REXCVAR_GET(masseffect_io_bcache_guest_write) == 1) {
    size_t done = 0;
    while (done < n) {
      const uint64_t addr = reinterpret_cast<uint64_t>(dst + done);
      const size_t piece = std::min<size_t>(n - done, 0x1000 - (addr & 0xFFF));
      void* alias = RexGmShadowFor(addr);
      std::memcpy(alias ? alias : dst + done, src + done, piece);
      done += piece;
    }
    return;
  }
#endif
  std::memcpy(dst, src, n);
}

// Reads the guest's buffer back the same way it was written (for the verification).
void ReadGuest(uint8_t* dst, const uint8_t* src, size_t n) {
#if REX_PLATFORM_SWITCH
  if (REXCVAR_GET(masseffect_io_bcache_guest_write) == 1) {
    size_t done = 0;
    while (done < n) {
      const uint64_t addr = reinterpret_cast<uint64_t>(src + done);
      const size_t piece = std::min<size_t>(n - done, 0x1000 - (addr & 0xFFF));
      const void* alias = RexGmShadowFor(addr);
      std::memcpy(dst + done, alias ? alias : src + done, piece);
      done += piece;
    }
    return;
  }
#endif
  std::memcpy(dst, src, n);
}

// The first differing byte and how many differ, for the DIFFERENCE line.
void DiffInfo(const uint8_t* a, const uint8_t* b, size_t n, size_t* first, size_t* count) {
  *first = SIZE_MAX;
  *count = 0;
  for (size_t i = 0; i < n; ++i) {
    if (a[i] != b[i]) {
      if (*first == SIZE_MAX) *first = i;
      ++*count;
    }
  }
}

void Disable(const char* what, const std::string& file, uint64_t offset, size_t req,
             const std::string& detail) {
  if (g_disabled.exchange(true)) {
    return;
  }
  REXFS_WARN("[io] block cache: DIFFERENCE ({}) in '{}' at {} ({} bytes): {}. The block cache is OFF "
             "for the rest of the session; this read is redone from the SD.",
             what, file, offset, req, detail);
}

std::string FileName(uint32_t id) {
  std::lock_guard lock(g.mu);
  return id && id <= g.files.size() ? g.files[id - 1].guest : std::string("?");
}

}  // namespace

bool Read(uint32_t id, FileHandle* fh, std::span<uint8_t> buffer, uint64_t offset,
          size_t* out_bytes_read) {
  const size_t req = buffer.size();
  if (!id || !req || !fh || g_disabled.load(std::memory_order_relaxed)) {
    return false;
  }
  uint64_t size = 0;
  uint32_t shift = 0;
  uint32_t B = 0;
  bool caching = false;
  {
    std::lock_guard lock(g.mu);
    if (id > g.files.size()) {
      return false;
    }
    size = g.files[id - 1].size;
    shift = g.shift;
    B = g.block;
    caching = g.max_blocks > 0;
  }
  const uint64_t max_req = uint64_t(std::max(0, REXCVAR_GET(masseffect_io_bcache_max_request_kb))) * 1024;
  if (offset >= size || req > size - offset || req > max_req) {
    if (caching) c.bypass.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint64_t first = offset >> shift;
  const uint64_t last = (offset + req - 1) >> shift;
  const size_t nblocks = size_t(last - first + 1);
  if (g.ghost) {
    GhostAccess(id, first, nblocks);
  }
  if (!caching) {
    return false;  // ghost only
  }

  // The answer is assembled in a private buffer and written to the guest once, at the end.
  std::unique_ptr<uint8_t[]> staging(new (std::nothrow) uint8_t[req]);
  if (!staging) {
    c.bypass.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  // Per demand block: what to do with it.
  enum : uint8_t { kServed = 0, kReadInsert = 1, kReadOnly = 2 };
  std::vector<uint8_t> todo(nblocks, kServed);
  std::vector<int32_t> slot_of(nblocks, -1);
  std::vector<uint32_t> avail(nblocks, 0);  // bytes the block has (B, or less at end of file)
  std::vector<int32_t> ahead_slots;         // synchronous read-ahead blocks after `last`
  size_t from_ram = 0;
  size_t demand_missing = 0;
  bool corrupt = false;
  std::string corrupt_detail;

  auto copy_part = [&](uint64_t b, const uint8_t* src, uint32_t len) {
    // Copies the part of block b (len valid bytes at src) that falls inside the request.
    const uint64_t bstart = b << shift;
    const uint64_t lo = std::max<uint64_t>(offset, bstart);
    const uint64_t hi = std::min<uint64_t>(offset + req, bstart + len);
    if (hi > lo) {
      std::memcpy(staging.get() + (lo - offset), src + (lo - bstart), size_t(hi - lo));
      return size_t(hi - lo);
    }
    return size_t(0);
  };

  {
    std::unique_lock lk(g.mu);
    // 1. Blocks another thread is reading: cancel queued jobs, wait for running reads.
    const int32_t wait_ms = std::max(0, REXCVAR_GET(masseffect_io_bcache_wait_ms));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    uint64_t wait_t0 = 0;
    for (;;) {
      bool must_wait = false;
      for (size_t k = 0; k < nblocks; ++k) {
        auto it = g.index.find(Key(id, first + k));
        if (it == g.index.end() || g.slots[it->second].state != kPending) continue;
        const uint32_t job = g.slots[it->second].job;
        if (job != 0 && job != g.running_job) {
          CancelJob(job);  // not started: the guest reads these blocks itself, now
        } else {
          must_wait = true;
        }
      }
      if (!must_wait) break;
      if (!wait_t0) {
        wait_t0 = NowUs();
        c.waits.fetch_add(1, std::memory_order_relaxed);
      }
      if (g.cv.wait_until(lk, deadline) == std::cv_status::timeout) {
        c.wait_timeouts.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
    if (wait_t0) {
      c.wait_us.fetch_add(NowUs() - wait_t0, std::memory_order_relaxed);
    }

    // 2. Serve what is in RAM, reserve what is missing.
    for (size_t k = 0; k < nblocks && !corrupt; ++k) {
      const uint64_t b = first + k;
      auto it = g.index.find(Key(id, b));
      if (it != g.index.end() && g.slots[it->second].state == kValid) {
        const int32_t i = it->second;
        if (g.checksum) {
          c.sums_checked.fetch_add(1, std::memory_order_relaxed);
          if (BlockSum(g.slots[i].data, g.slots[i].len) != g.slots[i].sum) {
            corrupt = true;
            corrupt_detail = fmt::format("block {} (file offset {}, {} bytes) changed in RAM after it was "
                                         "stored (slot {}, host address {})",
                                         b, b << shift, g.slots[i].len, i,
                                         static_cast<const void*>(g.slots[i].data));
            break;
          }
        }
        avail[k] = g.slots[i].len;
        from_ram += copy_part(b, g.slots[i].data, g.slots[i].len);
        NoteHit(i);
        continue;
      }
      ++demand_missing;
      if (it != g.index.end()) {
        todo[k] = kReadOnly;  // still pending after the wait: read it, leave the slot to its owner
        continue;
      }
      const int32_t i = ReservePending(Key(id, b), 0, false);
      todo[k] = i >= 0 ? kReadInsert : kReadOnly;
      slot_of[k] = i;
    }
    if (corrupt) {
      for (size_t k = 0; k < nblocks; ++k) {
        if (slot_of[k] >= 0) ReleaseSlot(slot_of[k]);
      }
    }

    // 3. Read-ahead decision. Only after a request that ends on a block boundary and is large
    //    enough to be a piece of a longer one.
    const uint64_t ra_min = uint64_t(std::max(0, REXCVAR_GET(masseffect_io_bcache_readahead_min_kb))) * 1024;
    const bool continues = !corrupt && ((offset + req) & (uint64_t(B) - 1)) == 0 && req >= ra_min &&
                           ((last + 1) << shift) < size;
    if (g.ahead_max > 0 && g.ahead_blocks == 0 && demand_missing) {
      g.miss_blocks_while_off += demand_missing;
      if (g.miss_blocks_while_off >= kReprobeMissBlocks) {
        g.ahead_blocks = std::max(1, g.ahead_max / 4);  // probe again: the pattern may have changed
        g.miss_blocks_while_off = 0;
      }
    }
    if (continues && g.ahead_blocks > 0) {
      if (g.async) {
        QueueAhead(id, last + 1, uint32_t(g.ahead_blocks), size);
      } else if (todo[nblocks - 1] != kServed) {
        // Synchronous: extend the SD read of the last missing run.
        for (int32_t k = 1; k <= g.ahead_blocks; ++k) {
          const uint64_t b = last + uint64_t(k);
          if ((b << shift) >= size || g.index.count(Key(id, b))) break;
          const int32_t i = ReservePending(Key(id, b), 0, true);
          if (i < 0) break;
          ahead_slots.push_back(i);
        }
      }
    }
  }
  if (corrupt) {
    g.cv.notify_all();
    Disable("cache RAM", FileName(id), offset, req, corrupt_detail);
    c.bypass.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  // 4. SD reads, lock released. One pread loop per contiguous run of missing blocks; the
  //    read-ahead blocks extend the last run.
  bool failed = false;
  size_t k = 0;
  while (k < nblocks) {
    if (todo[k] == kServed) {
      ++k;
      continue;
    }
    size_t end = k;
    while (end + 1 < nblocks && todo[end + 1] != kServed) ++end;
    const bool with_ahead = end == nblocks - 1 && !ahead_slots.empty();
    const uint64_t run_first = first + k;
    const uint64_t run_blocks = (end - k + 1) + (with_ahead ? ahead_slots.size() : 0);
    const uint64_t start = run_first << shift;
    const size_t span = size_t(std::min<uint64_t>(run_blocks << shift, size - start));

    std::unique_ptr<uint8_t[]> tmp(new (std::nothrow) uint8_t[span]);
    const int64_t got = tmp ? ReadSpan(fh, start, tmp.get(), span) : -1;
    if (got < 0) {
      failed = true;
    } else {
      const uint64_t demand_bytes = std::min<uint64_t>(uint64_t(got), uint64_t(end - k + 1) << shift);
      c.sd_demand.fetch_add(demand_bytes, std::memory_order_relaxed);
      c.sd_ahead.fetch_add(uint64_t(got) - demand_bytes, std::memory_order_relaxed);
    }

    {
      std::lock_guard lk(g.mu);
      for (size_t r = 0; r < run_blocks; ++r) {
        const bool is_ahead = r > end - k;
        const int32_t i = is_ahead ? ahead_slots[r - (end - k + 1)] : slot_of[k + r];
        const int64_t off = int64_t(r) << shift;
        const int64_t len = got > off ? std::min<int64_t>(got - off, B) : 0;
        if (!is_ahead) {
          avail[k + r] = uint32_t(std::max<int64_t>(len, 0));
        }
        if (i < 0) continue;
        if (len <= 0) {
          ReleaseSlot(i);
          continue;
        }
        Publish(i, tmp.get() + off, uint32_t(len));
        g.slots[i].ahead = is_ahead;
      }
      if (with_ahead) {
        AdaptAfterIssue(ahead_slots.size());
      }
    }
    g.cv.notify_all();

    if (got > 0) {
      for (size_t r = k; r <= end; ++r) {
        const int64_t off = int64_t(r - k) << shift;
        if (got > off) {
          copy_part(first + r, tmp.get() + off, uint32_t(std::min<int64_t>(got - off, B)));
        }
      }
    }
    if (with_ahead) ahead_slots.clear();
    k = end + 1;
  }

  // 5. How much of the request is really there: the answer stops where the file ended.
  size_t delivered = req;
  for (size_t r = 0; r < nblocks; ++r) {
    const uint64_t bstart = (first + r) << shift;
    const uint64_t want_end = std::min<uint64_t>(offset + req, bstart + B);
    const uint64_t have_end = bstart + avail[r];
    if (have_end < want_end) {
      delivered = have_end > offset ? size_t(have_end - offset) : 0;
      break;
    }
  }
  if (failed || delivered == 0) {
    // Let the usual path answer (and report the error exactly as before). Blocks already published
    // are real file bytes and stay.
    c.bypass.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  // 6. Verification (masseffect_io_bcache_verify_n): the same range straight from the SD.
  const int32_t verify_n = REXCVAR_GET(masseffect_io_bcache_verify_n);
  const bool verify =
      verify_n < 0 ||
      (verify_n > 0 && c.verify_budget_used.fetch_add(1, std::memory_order_relaxed) < uint64_t(verify_n));
  std::unique_ptr<uint8_t[]> check;
  size_t check_n = 0;
  if (verify) {
    check.reset(new (std::nothrow) uint8_t[req]);
    if (check) {
      while (check_n < req) {
        size_t n = 0;
        if (!fh->Read(offset + check_n, check.get() + check_n, req - check_n, &n) || n == 0) break;
        check_n += n;
      }
      size_t at = 0, count = 0;
      if (check_n != delivered) {
        Disable("cache vs SD", FileName(id), offset, req,
                fmt::format("the cache had {} bytes, the SD returned {}", delivered, check_n));
        return false;
      }
      DiffInfo(staging.get(), check.get(), delivered, &at, &count);
      if (count) {
        Disable("cache vs SD", FileName(id), offset, req,
                fmt::format("{} bytes differ, first at +{} (block {}, {} from RAM in this read), "
                            "cache {:02X} vs SD {:02X}",
                            count, at, (offset + at) >> shift, from_ram ? "some" : "none",
                            staging[at], check[at]));
        return false;
      }
    }
  }

  WriteGuest(buffer.data(), staging.get(), delivered);

  if (verify && check) {
    std::unique_ptr<uint8_t[]> back(new (std::nothrow) uint8_t[delivered]);
    if (back) {
      ReadGuest(back.get(), buffer.data(), delivered);
      size_t at = 0, count = 0;
      DiffInfo(back.get(), check.get(), delivered, &at, &count);
      if (count) {
        Disable("guest buffer vs SD", FileName(id), offset, req,
                fmt::format("{} bytes differ after the write into guest memory (host {}), first at +{}, "
                            "guest {:02X} vs SD {:02X}; the cache's own bytes were right",
                            count, static_cast<const void*>(buffer.data()), at, back[at], check[at]));
        return false;
      }
    }
    c.verified.fetch_add(1, std::memory_order_relaxed);
  }

  c.reads.fetch_add(1, std::memory_order_relaxed);
  c.bytes_req.fetch_add(req, std::memory_order_relaxed);
  c.bytes_ram.fetch_add(from_ram, std::memory_order_relaxed);
  if (!demand_missing) {
    c.full_hit.fetch_add(1, std::memory_order_relaxed);
  } else if (demand_missing == nblocks) {
    c.miss.fetch_add(1, std::memory_order_relaxed);
  } else {
    c.partial.fetch_add(1, std::memory_order_relaxed);
  }
  *out_bytes_read = delivered;
  return true;
}

Stats ReadStats() {
  Stats s;
  {
    std::lock_guard lock(g.mu);
    s.enabled = g.max_blocks > 0 && !g.files.empty();
    s.ghost = g.ghost;
    s.async = g.async;
    s.block_kb = g.block / 1024;
    s.cap_mb = (uint64_t(g.max_blocks) * g.block) >> 20;
    s.live_bytes = g.live;
    s.slab_bytes = g.slab_bytes;
    s.slab_failed = g.slab_failed;
    s.ahead_blocks_now = g.ahead_blocks;
  }
  s.reads = c.reads.load(std::memory_order_relaxed);
  s.reads_full_hit = c.full_hit.load(std::memory_order_relaxed);
  s.reads_partial = c.partial.load(std::memory_order_relaxed);
  s.reads_miss = c.miss.load(std::memory_order_relaxed);
  s.reads_bypass = c.bypass.load(std::memory_order_relaxed);
  s.bytes_requested = c.bytes_req.load(std::memory_order_relaxed);
  s.bytes_ram = c.bytes_ram.load(std::memory_order_relaxed);
  s.sd_calls = c.sd_calls.load(std::memory_order_relaxed);
  s.sd_bytes_demand = c.sd_demand.load(std::memory_order_relaxed);
  s.sd_bytes_ahead = c.sd_ahead.load(std::memory_order_relaxed);
  s.sd_us = c.sd_us.load(std::memory_order_relaxed);
  s.ahead_used = c.ahead_used.load(std::memory_order_relaxed);
  s.ahead_wasted = c.ahead_wasted.load(std::memory_order_relaxed);
  s.evictions = c.evictions.load(std::memory_order_relaxed);
  s.waits = c.waits.load(std::memory_order_relaxed);
  s.wait_us = c.wait_us.load(std::memory_order_relaxed);
  s.wait_timeouts = c.wait_timeouts.load(std::memory_order_relaxed);
  s.jobs_queued = c.jobs_queued.load(std::memory_order_relaxed);
  s.jobs_dropped = c.jobs_dropped.load(std::memory_order_relaxed);
  s.jobs_cancelled = c.jobs_cancelled.load(std::memory_order_relaxed);
  s.worker_opens = c.worker_opens.load(std::memory_order_relaxed);
  s.worker_open_us = c.worker_open_us.load(std::memory_order_relaxed);
  s.verified = c.verified.load(std::memory_order_relaxed);
  s.sums_checked = c.sums_checked.load(std::memory_order_relaxed);
  s.disabled = g_disabled.load(std::memory_order_relaxed);
  {
    std::lock_guard lock(g_ghost_mu);
    for (int k = 0; k < Stats::kGhostSizes; ++k) {
      s.ghost_mb[k] = kGhostMb[k];
      s.ghost_hits[k] = g_ghost[k].hits;
    }
  }
  s.ghost_accesses = g_ghost_accesses.load(std::memory_order_relaxed);
  return s;
}

std::string SummaryLine(int period_s) {
  static std::mutex mu;
  static Stats prev;
  const Stats s = ReadStats();
  if (!s.enabled && !s.ghost) {
    return {};
  }
  Stats p;
  {
    std::lock_guard lock(mu);
    p = prev;
    prev = s;
  }
  constexpr double kMB = 1024.0 * 1024.0;
  std::string out;

  if (s.enabled) {
    const uint64_t reads = s.reads - p.reads;
    const uint64_t req = s.bytes_requested - p.bytes_requested;
    const uint64_t ram = s.bytes_ram - p.bytes_ram;
    const uint64_t sd_d = s.sd_bytes_demand - p.sd_bytes_demand;
    const uint64_t sd_a = s.sd_bytes_ahead - p.sd_bytes_ahead;
    const uint64_t sd_us = s.sd_us - p.sd_us;
    // Time saved: the bytes served from RAM priced at what this cache's own SD reads cost per byte
    // in the whole session (per-call overhead included). Before any SD read: 4.1 ms per 128 KB,
    // the session average measured without the cache.
    const double us_per_byte = (s.sd_bytes_demand + s.sd_bytes_ahead) > (1u << 20)
                                   ? double(s.sd_us) / double(s.sd_bytes_demand + s.sd_bytes_ahead)
                                   : 4100.0 / 131072.0;
    const double saved_ms = double(ram) * us_per_byte / 1000.0;
    const double saved_total_ms = double(s.bytes_ram) * us_per_byte / 1000.0;
    out = fmt::format(
        "[io] block-cache {} s: {} reads ({} all RAM, {} partial, {} all SD, {} bypassed), {:.1f} of "
        "{:.1f} MB from RAM ({:.0f} %); SD {} calls {:.1f} MB (demand {:.1f} + ahead {:.1f}) in "
        "{:.0f} ms; saved ~{:.0f} ms | ahead: used {} wasted {}, now {} KB; waits {} ({:.1f} ms, {} "
        "timeouts); jobs {} queued {} dropped {} cancelled, {} opens ({:.0f} ms) | total: {:.1f} MB "
        "from RAM of {:.1f} MB, SD {:.1f} MB, saved ~{:.1f} s | RAM {:.1f} MB in blocks, {:.0f} of {} MB "
        "allocated{}, {} evictions | checks: {} reads verified against the SD, {} block sums{}",
        period_s, reads, s.reads_full_hit - p.reads_full_hit, s.reads_partial - p.reads_partial,
        s.reads_miss - p.reads_miss, s.reads_bypass - p.reads_bypass, ram / kMB, req / kMB,
        req ? 100.0 * double(ram) / double(req) : 0.0, s.sd_calls - p.sd_calls, (sd_d + sd_a) / kMB,
        sd_d / kMB, sd_a / kMB, sd_us / 1000.0, saved_ms, s.ahead_used - p.ahead_used,
        s.ahead_wasted - p.ahead_wasted, uint64_t(std::max(0, s.ahead_blocks_now)) * s.block_kb,
        s.waits - p.waits, (s.wait_us - p.wait_us) / 1000.0, s.wait_timeouts - p.wait_timeouts,
        s.jobs_queued - p.jobs_queued, s.jobs_dropped - p.jobs_dropped,
        s.jobs_cancelled - p.jobs_cancelled, s.worker_opens - p.worker_opens,
        (s.worker_open_us - p.worker_open_us) / 1000.0, s.bytes_ram / kMB, s.bytes_requested / kMB,
        (s.sd_bytes_demand + s.sd_bytes_ahead) / kMB, saved_total_ms / 1000.0, s.live_bytes / kMB,
        s.slab_bytes / kMB, s.cap_mb, s.slab_failed ? " [allocation failed]" : "", s.evictions,
        s.verified, s.sums_checked, s.disabled ? " [OFF after a DIFFERENCE]" : "");
  }
  if (s.ghost) {
    const uint64_t acc = s.ghost_accesses - p.ghost_accesses;
    std::string g_line = fmt::format("[io] block-cache ghost ({} KB blocks), share of demand blocks "
                                     "already in RAM, last {} s / whole session:",
                                     s.block_kb, period_s);
    for (int k = 0; k < Stats::kGhostSizes; ++k) {
      const uint64_t h = s.ghost_hits[k] - p.ghost_hits[k];
      g_line += fmt::format(" {} MB {:.0f}/{:.0f} %", s.ghost_mb[k], acc ? 100.0 * double(h) / double(acc) : 0.0,
                            s.ghost_accesses ? 100.0 * double(s.ghost_hits[k]) / double(s.ghost_accesses) : 0.0);
      if (k + 1 < Stats::kGhostSizes) g_line += ",";
    }
    g_line += fmt::format(" ({} blocks in the interval)", acc);
    out += out.empty() ? g_line : "\n" + g_line;
  }
  return out;
}

}  // namespace rex::filesystem::block_cache

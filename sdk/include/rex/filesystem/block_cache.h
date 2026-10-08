/**
 * @file        rex/filesystem/block_cache.h
 * @brief       RAM block cache for read-only game files (package streaming). See docs/streaming-io.md.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * Everything is off by default (masseffect_io_bcache_mb = 0, masseffect_io_bcache_ghost = false).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace rex::filesystem {

class FileHandle;

namespace block_cache {

/*
 * Called once per opened read-only file (HostPathFile constructor). Returns the file id to pass to
 * Read(), or 0 when the file must not go through the cache (cache and ghost off, skip list, unknown
 * size). The id is stable per guest path, so two handles of the same file share blocks.
 */
uint32_t Register(const std::string& guest_path, const std::filesystem::path& host_path,
                  uint64_t size);

/*
 * Serves a guest read through the cache. Returns true when the read was answered (*out_bytes_read
 * set, status success); false means "not handled": the caller must take its usual path, and nothing
 * the guest can see has been touched.
 *
 * fh is the caller's own handle. It is only used from the calling thread, inside the call (pread on
 * Horizon is lseek+read+lseek, so a handle must never be shared between threads).
 */
bool Read(uint32_t id, FileHandle* fh, std::span<uint8_t> buffer, uint64_t offset,
          size_t* out_bytes_read);

// Cumulative counters since start. The [io] summary prints the difference per interval.
struct Stats {
  bool enabled = false;      // a cap > 0 and at least one registered file
  bool ghost = false;        // metadata-only simulation on
  bool async = false;        // read-ahead runs on the prefetch thread
  uint32_t block_kb = 0;
  uint64_t cap_mb = 0;
  uint64_t live_bytes = 0;   // bytes held in valid blocks right now
  uint64_t slab_bytes = 0;   // RAM actually allocated for block storage (grows up to the cap)
  bool slab_failed = false;  // an allocation failed: the cache stopped growing there

  uint64_t reads = 0;            // guest reads that went through the cache
  uint64_t reads_full_hit = 0;   // answered entirely from RAM
  uint64_t reads_partial = 0;    // some blocks from RAM, some from the SD
  uint64_t reads_miss = 0;       // every block from the SD
  uint64_t reads_bypass = 0;     // eligible file but not handled (too large, past EOF, error)
  uint64_t bytes_requested = 0;  // bytes the guest asked for through the cache
  uint64_t bytes_ram = 0;        // of those, bytes copied from RAM blocks

  uint64_t sd_calls = 0;         // preads issued by the cache (demand + read-ahead + async)
  uint64_t sd_bytes_demand = 0;  // SD bytes of blocks the guest asked for
  uint64_t sd_bytes_ahead = 0;   // SD bytes of read-ahead blocks (sync and async)
  uint64_t sd_us = 0;            // time inside those preads
  uint64_t ahead_used = 0;       // read-ahead blocks that were later hit at least once
  uint64_t ahead_wasted = 0;     // read-ahead blocks evicted without a single hit
  int32_t ahead_blocks_now = 0;  // current adaptive read-ahead, in blocks
  uint64_t evictions = 0;

  uint64_t waits = 0;            // guest reads that waited for a block another thread was reading
  uint64_t wait_us = 0;
  uint64_t wait_timeouts = 0;
  uint64_t jobs_queued = 0;      // async read-ahead jobs
  uint64_t jobs_dropped = 0;     // queue full
  uint64_t jobs_cancelled = 0;   // the guest needed the blocks before the job started
  uint64_t worker_opens = 0;     // files the prefetch thread had to open for itself
  uint64_t worker_open_us = 0;

  uint64_t verified = 0;         // reads compared with a second SD read (masseffect_io_bcache_verify_n)
  uint64_t sums_checked = 0;     // block checksums checked on hits (masseffect_io_bcache_checksum)
  bool disabled = false;         // a DIFFERENCE turned the cache off for the session

  static constexpr int kGhostSizes = 5;
  uint64_t ghost_mb[kGhostSizes] = {};
  uint64_t ghost_hits[kGhostSizes] = {};  // demand blocks that this cap would have had in RAM
  uint64_t ghost_accesses = 0;            // demand blocks seen
};
Stats ReadStats();

// One-line summary for the periodic [io] report (interval figures plus totals). Empty when both the
// cache and the ghost are off, so the report does not grow for people who do not use it.
std::string SummaryLine(int period_s);

}  // namespace block_cache
}  // namespace rex::filesystem

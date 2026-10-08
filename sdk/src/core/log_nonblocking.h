/**
 * @file        core/log_nonblocking.h
 * @brief       Non-blocking rotating file sink (log_nonblocking)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * See log_nonblocking.cpp for the design and docs/platform-notes.md (Threads, "Non-blocking log").
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <spdlog/sinks/sink.h>

namespace rex::log_nb {

struct NonblockingFileSinkOptions {
  std::string path;
  size_t max_file_bytes = 5 * 1024 * 1024;
  size_t max_files = 20;
  size_t ring_bytes = 2 * 1024 * 1024;
  uint32_t interval_ms = 250;
  int writer_priority = 0x2C;  // Horizon priority (lower = more urgent); ignored off Switch
  uint64_t writer_core_mask = 0x3;  // Horizon core mask; ignored off Switch
};

// Creates the sink and starts its writer thread. Only one may exist at a time (the crash drain and
// FlushLogging find it through a global). Returns null if the file cannot be opened.
std::shared_ptr<spdlog::sinks::sink> CreateNonblockingFileSink(const NonblockingFileSinkOptions& opts);

// Writes everything queued so far to the file, from the calling thread, and fflushes. Waits for the
// writer thread if it is in the middle of a batch. For orderly exits (FlushLogging); never call it
// from a hot path.
void DrainNonblockingFileSink();

}  // namespace rex::log_nb

// Crash path (RexSwitchCrashLog: abort, exit, std::terminate, fatal faults): best-effort synchronous
// drain with a bounded wait. Safe to call when no non-blocking sink exists.
extern "C" void RexLogEmergencyDrain(void);

// Developer USB file channel (app/src/me_usb_files.cpp): reads [offset, offset + len) of the non-blocking sink's
// CURRENT log file, which Horizon's FS will not open a second time while the writer holds it. Returns the bytes
// read (0 at or past the end), or < 0: -1 no sink or path is not its current file, -2 the file lock stayed busy,
// -3 the file could not be read. *size_out = the file size (after writing the queued lines) when >= 0 is returned.
// buf may be null with len 0 to ask for the size only. Never call it from a hot path (it closes and reopens the file).
extern "C" long long RexLogReadCurrentFile(const char* path, unsigned long long offset, void* buf, size_t len,
                                          unsigned long long* size_out);

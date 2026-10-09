// Host heap state for the 60 s "[mem]" report and the out-of-memory messages (docs/memory-growth.md).
//
// On the Switch every host allocation, Mesa's GPU memory included (nouveau_horizon_memory.c: memalign + nvMapCreate),
// comes from one newlib heap carved out of the process memory at start-up (me_packaged.cpp __libnx_initheap). The heap
// cannot grow, so what matters is not only how much is free but whether the free space is contiguous:
//
//   used       mallinfo().uordblks: bytes malloc has handed out and not got back.
//   free       mallinfo().fordblks: free bytes inside the part of the heap malloc already took (holes).
//   untouched  fake_heap_end - sbrk(0): heap malloc has never taken. Together with the top chunk this is where a large
//              block can still come from once the holes are too small.
//   largest    the largest block malloc can hand out right now, found by a binary search of malloc/free calls in
//              1 MB steps (about 12 calls). A long run fragments the heap: the 2.4 h tour of 2026-10-09 died with
//              800 MB free and no block large enough for one texture's staging copy.
//
// Everything here is cheap except the probe, which takes the malloc lock a dozen times; it runs once per report.
#pragma once

#include <cstdint>

namespace me::native::heap {

struct Snapshot {
  bool valid = false;           // false where there is no newlib heap to read (PC builds)
  uint64_t used = 0;            // bytes
  uint64_t free_in_arena = 0;   // bytes
  uint64_t untouched = 0;       // bytes
  uint64_t largest = 0;         // bytes; 0 if not probed
  bool largest_capped = false;  // the probe stopped at kProbeCap: the largest block is at least that
  uint64_t process_used = 0;    // svcGetInfo InfoType_UsedMemorySize
  uint64_t process_total = 0;   // svcGetInfo InfoType_TotalMemorySize
};

// The probe never asks for more than this: it only has to tell a healthy heap from a fragmented one, and holding a
// huge block even for a microsecond could make a concurrent allocation on another thread fail.
constexpr uint64_t kProbeCap = uint64_t(512) << 20;

// me_heap_report.cpp (the only file that includes <switch.h> for this).
Snapshot Take(bool probe_largest);

}  // namespace me::native::heap

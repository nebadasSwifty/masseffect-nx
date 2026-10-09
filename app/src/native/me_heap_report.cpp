// Host heap state (me_heap_report.h, docs/memory-growth.md). Kept out of the header so <switch.h> and newlib's
// <malloc.h> stay out of the renderer's translation units.
#include "me_heap_report.h"

#include <cstddef>
#include <cstdlib>

#if defined(__SWITCH__)
#include <malloc.h>
#include <unistd.h>

#include <switch.h>
extern "C" char* fake_heap_end;  // libnx: end of the heap newlib's sbrk hands out
#endif

namespace me::native::heap {

#if defined(__SWITCH__)
namespace {

// Largest block malloc can return now, in 1 MB steps, at most `upper` bytes.
uint64_t ProbeLargestBlock(uint64_t upper) {
  uint64_t lo = 0, hi = upper >> 20;  // in MB
  while (lo < hi) {
    const uint64_t mid = lo + (hi - lo + 1) / 2;
    void* block = std::malloc(size_t(mid) << 20);
    // The pair would otherwise be legal to delete (GCC removes an unused malloc/free pair).
    asm volatile("" : : "r"(block) : "memory");
    if (block) {
      std::free(block);
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return lo << 20;
}

}  // namespace
#endif

Snapshot Take(bool probe_largest) {
  Snapshot s;
#if defined(__SWITCH__)
  const struct mallinfo mi = mallinfo();
  s.valid = true;
  s.used = uint64_t(mi.uordblks);
  s.free_in_arena = uint64_t(mi.fordblks);
  char* top = static_cast<char*>(sbrk(0));
  s.untouched = (fake_heap_end && top && top != reinterpret_cast<char*>(-1) && fake_heap_end > top)
                    ? uint64_t(fake_heap_end - top)
                    : 0;
  u64 value = 0;
  if (R_SUCCEEDED(svcGetInfo(&value, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0))) s.process_used = value;
  if (R_SUCCEEDED(svcGetInfo(&value, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0))) s.process_total = value;
  if (probe_largest) {
    const uint64_t upper = s.free_in_arena + s.untouched;
    s.largest = ProbeLargestBlock(upper < kProbeCap ? upper : kProbeCap);
    s.largest_capped = s.largest >= kProbeCap;
  }
#else
  (void)probe_largest;
#endif
  return s;
}

}  // namespace me::native::heap

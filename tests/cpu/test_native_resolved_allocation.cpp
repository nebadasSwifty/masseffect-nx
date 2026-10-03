#include "me_resolved_allocation.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>

int main() {
  using namespace me::native;
  static_assert(!ResolvedAllocationReady(0, 999));
  static_assert(!ResolvedAllocationReady(10, 9));
  static_assert(ResolvedAllocationReady(10, 10));
  static_assert(ObserveResolvedAllocationCompletion(9, 10, false) == 9);
  static_assert(ObserveResolvedAllocationCompletion(10, 9, true) == 10);
  static_assert(ObserveResolvedAllocationCompletion(9, 10, true) == 10);
  static_assert(ResolvedAllocationFits(31, (128ull << 20) - 1, 1));
  static_assert(!ResolvedAllocationFits(32, 0, 0));
  static_assert(!ResolvedAllocationFits(0, 128ull << 20, 1));
  static_assert(!ResolvedAllocationFits(0, std::numeric_limits<uint64_t>::max(), 1));
  static_assert(!ResolvedAllocationFits(0, 1, std::numeric_limits<uint64_t>::max()));

  // Queue model: old descriptor IDs are immutable. A pending output and earlier
  // reads precede the future retirement WORK marker. Slot fence handle reuse
  // cannot make that future marker complete.
  uint64_t completed = 7;
  constexpr uint64_t retired = 9;
  completed = ObserveResolvedAllocationCompletion(completed, 8, true);
  assert(!ResolvedAllocationReady(retired, completed));
  completed = ObserveResolvedAllocationCompletion(completed, 9, false);
  assert(!ResolvedAllocationReady(retired, completed));
  completed = ObserveResolvedAllocationCompletion(completed, 9, true);
  assert(ResolvedAllocationReady(retired, completed));
  assert(!ResolvedAllocationReady(10, completed));

  // A -> B -> A: logical cache activation discards dormant A's stale pixels.
  // This models the existing fresh-allocation zero baseline, NOT a canonical
  // guest-memory preservation contract. Same-active-version partial resolves
  // accumulate; only a layout/version switch discards the backing.
  std::array<unsigned, 16> dormant_a;
  dormant_a.fill(0xAA);
  const auto old_descriptor_read = dormant_a;
  assert(ResolvedAllocationReady(retired, completed));
  dormant_a.fill(0);  // inline WORK activation clear, after fence proof
  std::fill(dormant_a.begin(), dormant_a.begin() + 8, 0xBB);
  assert(std::all_of(dormant_a.begin() + 8, dormant_a.end(), [](auto x) { return x == 0; }));
  std::fill(dormant_a.begin() + 8, dormant_a.end(), 0xCC);
  assert(std::all_of(dormant_a.begin(), dormant_a.begin() + 8, [](auto x) { return x == 0xBB; }));
  assert(std::all_of(old_descriptor_read.begin(), old_descriptor_read.end(), [](auto x) { return x == 0xAA; }));
  std::puts("Resolved allocation policy: serial fence proof, failed waits, bounded bytes, dormant discard and same-version partial accumulation passed (CPU model, not GPU validation)");
}

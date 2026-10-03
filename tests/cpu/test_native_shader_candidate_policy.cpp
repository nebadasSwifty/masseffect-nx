#include "me_shader_candidate_policy.h"
#include <cassert>
#include <cstdio>

int main() {
  using me::native::ShouldReplaceVertexCandidate;
  // Rows/columns: absent, unproven, proven; last axis: legacy off/on.
  constexpr bool expected[3][3][2]{
      {{false, false}, {false, true}, {true, true}},
      {{false, false}, {false, true}, {true, true}},
      {{false, false}, {false, false}, {true, true}}};
  for (unsigned current = 0; current < 3; ++current)
    for (unsigned candidate = 0; candidate < 3; ++candidate)
      for (unsigned legacy = 0; legacy < 2; ++legacy)
        assert(ShouldReplaceVertexCandidate(current != 0, current == 2,
                                             candidate != 0, candidate == 2, legacy != 0) ==
               expected[current][candidate][legacy]);
  // Presence takes precedence over an unrelated/stale proof bit.
  for (unsigned legacy_bit = 0; legacy_bit < 2; ++legacy_bit) {
    const bool legacy = legacy_bit != 0;
    assert(!ShouldReplaceVertexCandidate(true, true, false, true, legacy));
    assert(ShouldReplaceVertexCandidate(false, true, true, false, legacy) == legacy);
  }
  static_assert(!ShouldReplaceVertexCandidate(true, true, true, false, true));
  static_assert(ShouldReplaceVertexCandidate(true, false, true, true, false));
  static_assert(ShouldReplaceVertexCandidate(true, true, true, true, false));
  std::puts("native shader candidate policy: PASS");
}

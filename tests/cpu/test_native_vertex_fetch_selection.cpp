#include "me_vertex_fetch_selection.h"
#include <array>
#include <cassert>
#include <cstdio>

int main() {
  using namespace me::native;
  std::array<uint32_t, 54> original{}, loaded{};
  const std::array<size_t, 2> declarations{13, 16};
  const auto index = [](size_t i) { return i; };
  for (size_t i : declarations) {
    original[i * 3] = 0x2000; original[i * 3 + 1] = 0x688;
    loaded[i * 3] = 0x05F82000; loaded[i * 3 + 1] = 0x40060688;
  }
  // Real VS8182: same TEMP r2 / xyzw at two different program points.
  loaded[13 * 3 + 1] |= 0x2000; loaded[13 * 3 + 2] = 8u << 8;
  loaded[16 * 3 + 2] = 9u << 8;
  const auto select = [&](size_t own, bool proof) {
    return SelectVertexFetch(original, loaded, own, proof, declarations, index);
  };
  assert(select(13, true).word_offset == 39);
  assert(select(16, true).word_offset == 48);
  assert(select(16, false).failure == VertexFetchSelectionFailure::Ambiguous);
  const auto identity = [&] {
    return VertexShaderIdentityMatches({ShaderIdentityStage::Vertex, loaded},
                                      {ShaderIdentityStage::Vertex, original}, declarations, index);
  };
  assert(identity());
  loaded[0] ^= 1; // CF-only change flips selection proof with identical fetch words.
  assert(!identity());
  assert(select(16, identity()).failure == VertexFetchSelectionFailure::Ambiguous);
  loaded[0] ^= 1;
  loaded[13 * 3] ^= 0x1000; // One uniquely eligible fallback at another instruction.
  assert(select(13, false).word_offset == 48);
  assert(!select(13, true)); // A claimed identity cannot permit a changed TEMP.
  loaded[16 * 3] |= 1; assert(!select(16, true)); // Wrong opcode.
  loaded[16 * 3] &= ~31u;
  loaded[16 * 3 + 1] = 0xFFF; assert(!select(16, true)); // Write mask lost.
  loaded[16 * 3 + 1] = 0xA88; assert(select(16, true)); // Supported W=1 default.
  assert(!select(SIZE_MAX, true));
  assert(!SelectVertexFetch(std::span(original).first(40), loaded, 13, true, declarations, index));
  const std::array<size_t, 1> invalid{SIZE_MAX};
  assert(!SelectVertexFetch(original, loaded, 16, false, invalid, index));
  loaded[16 * 3 + 1] = 0x688;
  const std::array<size_t, 2> duplicate{16, 16};
  assert(SelectVertexFetch(original, loaded, 16, false, duplicate, index).word_offset == 48);
  original[16 * 3 + 1] = 0; loaded[16 * 3 + 1] = 1;
  assert(!select(16, true)); // Same mask but impossible repeated-selector remap.
  std::puts("native vertex fetch selection: PASS");
}

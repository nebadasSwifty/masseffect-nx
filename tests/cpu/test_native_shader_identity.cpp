#include "me_shader_identity.h"
#include <array>
#include <cassert>
#include <cstdio>

// Independent bit-labelled model of the actual native pipeline: RemapCode
// updates one input component; the original translated fetch reads that input
// and emits its baked constants separately. Distinct old register labels make
// lost/added writes observable, including defaults and duplicate selectors.
static bool SymbolicSwizzlesEqual(uint32_t original, uint32_t patched) {
  constexpr std::array<uint32_t, 6> data{0x11111111, 0x22222222, 0x33333333,
                                        0x44444444, 0, 0x3F800000};
  constexpr std::array<uint32_t, 4> previous{0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC, 0xDDDDDDDD};
  std::array<uint32_t, 4> remap{0, 1, 2, 3};
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t a = (original >> (i * 3)) & 7, b = (patched >> (i * 3)) & 7;
    if (a == 6 || b == 6) return false;
    if (a < 4 && b != 7) remap[a] = b;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t a = (original >> (i * 3)) & 7, b = (patched >> (i * 3)) & 7;
    const uint32_t actual = a < 4 ? data[remap[a]] : a == 7 ? previous[i] : data[a];
    const uint32_t expected = b == 7 ? previous[i] : data[b];
    if (actual != expected) return false;
  }
  return true;
}

int main() {
  using namespace me::native;
  constexpr auto P = ShaderIdentityStage::Pixel, V = ShaderIdentityStage::Vertex;
  const std::array<uint32_t, 3> a{0x11223344, 0x55667788, 0xabcdef01};
  const std::array<uint32_t, 3> same = a, different{0x11223344, 0x55667789, 0xabcdef01};
  assert(PixelShaderIdentityMatches({P, a}, {P, same}));
  assert(!PixelShaderIdentityMatches({P, a}, {V, same}));
  assert(!PixelShaderIdentityMatches({V, a}, {P, same}));
  assert(!PixelShaderIdentityMatches({P, a}, {P, different}));
  assert(!PixelShaderIdentityMatches({P, a}, {P, std::span(a).first(2)}));
  assert(!PixelShaderIdentityMatches({P, {}}, {P, {}}));
  assert(!PixelShaderIdentityMatches({P, a}, {P, {}}));
  // A queued/replayed object cannot replace a newer loaded program; the same
  // container words remain acceptable irrespective of external DEF identity.
  assert(!PixelShaderIdentityMatches({P, different}, {P, same}));
  assert(PixelShaderIdentityMatches({P, different}, {P, different}));
  const std::array<uint32_t, 9> normalized{1, 2, 3, 0x11000, 0x80000011, 0x80000000,
                                         0xabcdef01, 0x12345678, 0x44332211};
  const std::array<uint32_t, 1> fetches{1};
  const auto index = [](uint32_t i) { return i; };
  auto loaded = normalized;
  loaded[3] |= 0x80000000; loaded[4] |= 0x12340000; loaded[5] |= 0x12345678;
  const auto matches = [&](const auto& words, const auto& slots) {
    return VertexShaderIdentityMatches({V, words}, {V, normalized}, slots, index);
  };
  assert(matches(loaded, fetches));
  loaded[6] ^= 1; assert(!matches(loaded, fetches)); loaded[6] ^= 1;  // ALU differs.
  loaded[0] ^= 1; assert(!matches(loaded, fetches)); loaded[0] ^= 1;  // CF differs.
  loaded[3] ^= 0x1000; assert(!matches(loaded, fetches)); loaded[3] ^= 0x1000; // TEMP differs.
  assert(!matches(loaded, std::array<uint32_t, 1>{3}));
  assert(!matches(loaded, std::array<uint32_t, 1>{UINT32_MAX}));
  assert(!matches(loaded, std::array<uint32_t, 0>{}));
  assert(!VertexShaderIdentityMatches({P, loaded}, {V, normalized}, fetches, index));
  assert(!VertexShaderIdentityMatches({V, {}}, {V, {}}, fetches, index));
  auto unnormalized = normalized; unnormalized[3] |= 0x80000000;
  assert(!VertexShaderIdentityMatches({V, loaded}, {V, unnormalized}, fetches, index));
  auto permuted = normalized; std::swap(permuted[3], permuted[6]);
  assert(!matches(permuted, fetches));
  auto position = normalized; position[4] = 0x688;
  auto default_one = position; default_one[4] = 0x00393A88;
  assert(VertexShaderIdentityMatches({V, default_one}, {V, position}, fetches, index));
  default_one[4] ^= 0x80000000; // Predication remains strict.
  assert(!VertexShaderIdentityMatches({V, default_one}, {V, position}, fetches, index));
  assert(VertexFetchSwizzleRepresentable(0x688, 0xA88));
  assert(!VertexFetchSwizzleRepresentable(0, 1)); // Repeated x cannot mean both x and y.
  assert(!VertexFetchSwizzleRepresentable(4, 5)); // Baked zero cannot become one.
  assert(!VertexFetchSwizzleRepresentable(7, 0)); // Preserved destination cannot become a write.
  assert(!VertexFetchSwizzleRepresentable(0, 7)); // Nor can a write disappear.
  assert(!VertexFetchSwizzleRepresentable(6, 6)); // Reserved is never silently supported.
  uint64_t accepted = 0;
  for (uint32_t original = 0; original < 4096; ++original) {
    for (uint32_t patched = 0; patched < 4096; ++patched) {
      const bool representable = VertexFetchSwizzleRepresentable(original, patched);
      assert(representable == SymbolicSwizzlesEqual(original, patched));
      accepted += representable;
    }
  }
  std::printf("native swizzle exhaustive: %llu / 16777216 accepted\n",
              static_cast<unsigned long long>(accepted));
  std::puts("native shader identity: PASS");
}

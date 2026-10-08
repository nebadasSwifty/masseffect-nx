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
  // Second-stage lookup: ALU tolerance passed explicitly. A W=1 position patch matches with tolerance 0,
  // and an ALU change is never accepted with tolerance 0, whatever the cvar holds.
  {
    auto p = default_one; p[4] &= ~0x80000000u;
    assert(VertexShaderIdentityMatches({V, p}, {V, position}, fetches, index, 0));
    p[6] ^= 1;
    g_vs_identity_alu_tolerance.store(4);
    assert(!VertexShaderIdentityMatches({V, p}, {V, position}, fetches, index, 0));
    assert(VertexShaderIdentityMatches({V, p}, {V, position}, fetches, index));
    g_vs_identity_alu_tolerance.store(0);
  }
  // Same-register fetch reorder (VS CD057930742AFE84: instructions 11 and 12 write r3.xy and r3.zw; Direct3D
  // swaps them). CF word: one exec clause at address 3, count 2, both fetches (sequence 0b0101).
  {
    std::array<uint32_t, 18> lib{};
    const uint64_t exec = uint64_t(3) | (uint64_t(2) << 12) | (uint64_t(0x5) << 16) | (uint64_t(1) << 44);
    lib[0] = uint32_t(exec); lib[1] = uint32_t(exec >> 32) & 0xFFFF;
    lib[9] = 0x00003000; lib[10] = 0x23F;  // r3.zw
    lib[12] = 0x00003000; lib[13] = 0xFC8; // r3.xy
    lib[15] = 0xC80F0001;                  // ALU
    const std::array<uint32_t, 2> run{3, 4};
    auto swapped = lib;
    swapped[9] = 0x05F83000; swapped[10] = 0x40253FC8; swapped[11] = 0x0000090D;
    swapped[12] = 0x05F83000; swapped[13] = 0x4025323F; swapped[14] = 0x00000B0D;
    assert(InstructionsInOneExecClause(lib, 3, 4));
    assert(!InstructionsInOneExecClause(lib, 3, 5));
    assert(!VertexShaderIdentityMatches({V, swapped}, {V, lib}, run, index, 0));
    assert(VertexShaderFetchPermutationMatches({V, swapped}, {V, lib}, run, index));
    g_vs_identity_fetch_permutation.store(false);
    assert(!VertexShaderProgramMatches({V, swapped}, {V, lib}, run, index));
    g_vs_identity_fetch_permutation.store(true);
    assert(VertexShaderProgramMatches({V, swapped}, {V, lib}, run, index));
    // Not permuted at all: the ordered compare decides, the permutation test says no.
    assert(!VertexShaderFetchPermutationMatches({V, lib}, {V, lib}, run, index));
    auto bad = swapped;
    bad[15] ^= 1; assert(!VertexShaderFetchPermutationMatches({V, bad}, {V, lib}, run, index));  // ALU differs
    bad = swapped; bad[12] ^= 0x1000;                                                       // other TEMP
    assert(!VertexShaderFetchPermutationMatches({V, bad}, {V, lib}, run, index));
    bad = swapped; bad[13] = 0x40253FCF;                                                    // masks overlap
    assert(!VertexShaderFetchPermutationMatches({V, bad}, {V, lib}, run, index));
    bad = swapped; bad[1] = 0; bad[0] = 0;                                                  // no exec clause
    auto lib_no_cf = lib; lib_no_cf[0] = 0; lib_no_cf[1] = 0;
    assert(!VertexShaderFetchPermutationMatches({V, bad}, {V, lib_no_cf}, run, index));
    // Two clauses: instruction 3 alone, then 4 alone. Something may run between them: refused.
    auto split = lib;
    const uint64_t e1 = uint64_t(3) | (uint64_t(1) << 12) | (uint64_t(0x1) << 16) | (uint64_t(1) << 44);
    const uint64_t e2 = uint64_t(4) | (uint64_t(1) << 12) | (uint64_t(0x1) << 16) | (uint64_t(1) << 44);
    split[0] = uint32_t(e1); split[1] = (uint32_t(e1 >> 32) & 0xFFFF) | (uint32_t(e2) << 16);
    split[2] = uint32_t(e2 >> 16);
    auto split_loaded = swapped; split_loaded[0] = split[0]; split_loaded[1] = split[1]; split_loaded[2] = split[2];
    assert(!InstructionsInOneExecClause(split, 3, 4));
    assert(!VertexShaderFetchPermutationMatches({V, split_loaded}, {V, split}, run, index));
    // Declared fetches that are not adjacent are no run.
    const std::array<uint32_t, 1> alone{3};
    assert(!VertexShaderFetchPermutationMatches({V, swapped}, {V, lib}, alone, index));
  }
  std::printf("native swizzle exhaustive: %llu / 16777216 accepted\n",
              static_cast<unsigned long long>(accepted));
  std::puts("native shader identity: PASS");
}

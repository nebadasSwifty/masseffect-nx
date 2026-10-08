#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <span>

namespace me::native {
enum class ShaderIdentityStage : uint8_t { Vertex, Pixel };
struct ShaderIdentityView {
  ShaderIdentityStage stage;
  std::span<const uint32_t> words;  // host-order sequencer words, not container bytes
};

// Full containers with different DEF/ABI metadata may share these words. Keep
// their identity when compatible; this only rejects a different GPU program.
inline bool PixelShaderIdentityMatches(ShaderIdentityView loaded, ShaderIdentityView selected) {
  return loaded.stage == ShaderIdentityStage::Pixel &&
         selected.stage == ShaderIdentityStage::Pixel && !loaded.words.empty() &&
         loaded.words.size() == selected.words.size() &&
         std::equal(loaded.words.begin(), loaded.words.end(), selected.words.begin());
}

// Exactly the mappings RemapCode + remapInput + the original HLSL fetch
// can express. Original constants are emitted after the fetch, and preserved
// destinations are not assigned by that HLSL at all. Reserved selector 6 has
// no supported meaning. Repeated input selectors share one remap component.
inline bool VertexFetchSwizzleRepresentable(uint32_t original, uint32_t patched) {
  uint32_t replacements[4]{7, 7, 7, 7};
  for (uint32_t destination = 0; destination < 4; ++destination) {
    const uint32_t before = (original >> (destination * 3)) & 7;
    const uint32_t after = (patched >> (destination * 3)) & 7;
    if (before == 6 || after == 6 || (before == 7) != (after == 7)) return false;
    if (before < 4) {
      if (after > 5 || (replacements[before] != 7 && replacements[before] != after)) return false;
      replacements[before] = after;
    } else if (before != after) return false;
  }
  return true;
}

// Destination components a vertex fetch writes (bit i = component i), from its 12-bit swizzle.
inline uint32_t VertexFetchWrittenMask(uint32_t swizzle) {
  uint32_t mask = 0;
  for (uint32_t i = 0; i < 4; ++i)
    if (((swizzle >> (i * 3)) & 7) != 7) mask |= 1u << i;
  return mask;
}

// Experimental tolerance (cvar masseffect_native_vs_identity_tolerance, default 0 = exact, as before): how many
// words OUTSIDE the declared fetch triples (ALU/CF words) a loaded vertex shader may differ from the library
// candidate of the same size and still be accepted. Without it such a draw is dropped (no VS identified).
inline std::atomic<uint32_t> g_vs_identity_alu_tolerance{0};

// selected.words must be the library's normalized VS words. Only the declared
// fetch triples may differ in the SAME loader-masked fields or in a swizzle
// proven representable by the existing typed input ABI above.
// No fetch permutation or ALU/CF exception is allowed. Projection returns a
// three-word instruction index (not a word offset). Avoids allocation per draw.
// alu_tolerance: see g_vs_identity_alu_tolerance (the overload below passes the cvar value).
template <class FetchRange, class Projection>
inline bool VertexShaderIdentityMatches(ShaderIdentityView loaded, ShaderIdentityView selected,
                                        const FetchRange& fetches, Projection instruction_index,
                                        uint32_t alu_tolerance) {
  if (loaded.stage != ShaderIdentityStage::Vertex || selected.stage != ShaderIdentityStage::Vertex ||
      loaded.words.empty() || loaded.words.size() != selected.words.size()) return false;
  constexpr uint32_t masks[3]{0x0007FFFFu, 0x80000FFFu, 0x80000000u};
  for (const auto& fetch : fetches) {
    const uint64_t instruction = uint64_t(instruction_index(fetch));
    if (instruction > UINT32_MAX || instruction * 3 + 3 > selected.words.size()) return false;
    const size_t start = size_t(instruction) * 3;
    if ((selected.words[start] & 0x1Fu) != 0u) return false;  // Not a vertex fetch.
    for (size_t lane = 0; lane < 3; ++lane)
      if ((selected.words[start + lane] & masks[lane]) != selected.words[start + lane]) return false;
    if (!VertexFetchSwizzleRepresentable(selected.words[start + 1] & 0xFFFu,
                                         loaded.words[start + 1] & 0xFFFu)) return false;
  }
  uint32_t tolerance_left = alu_tolerance;
  for (size_t word = 0; word < loaded.words.size(); ++word) {
    if (loaded.words[word] == selected.words[word]) continue;
    bool declared_fetch = false;
    for (const auto& fetch : fetches) {
      const size_t start = size_t(instruction_index(fetch)) * 3;
      if (word >= start && word - start < 3) {
        declared_fetch = true;
        const uint32_t mask = word - start == 1 ? 0x80000000u : masks[word - start];
        if ((loaded.words[word] & mask) != (selected.words[word] & mask)) return false;
        break;
      }
    }
    if (!declared_fetch) {
      if (tolerance_left == 0) return false;
      --tolerance_left;
    }
  }
  return true;
}

template <class FetchRange, class Projection>
inline bool VertexShaderIdentityMatches(ShaderIdentityView loaded, ShaderIdentityView selected,
                                        const FetchRange& fetches, Projection instruction_index) {
  return VertexShaderIdentityMatches(loaded, selected, fetches, instruction_index,
                                     g_vs_identity_alu_tolerance.load(std::memory_order_relaxed));
}

// Whether instructions [first, last] run back to back: one exec clause of the control flow holds all of them
// and no exec clause holds only a part of them (same CF walk as FetchesOfVertices in masseffect_native_shaders.cpp:
// exec opcodes 1-6, 13, 14; the first non-zero clause address ends the CF program).
inline bool InstructionsInOneExecClause(std::span<const uint32_t> w, size_t first, size_t last) {
  if (first > last) return false;
  size_t end = w.size() / 3;
  bool whole = false;
  for (size_t par = 0; par < end && par * 3 + 2 < w.size(); ++par) {
    const uint64_t cf[2] = {uint64_t(w[par * 3]) | (uint64_t(w[par * 3 + 1] & 0xFFFF) << 32),
                            uint64_t(w[par * 3 + 1] >> 16) | (uint64_t(w[par * 3 + 2]) << 16)};
    for (uint64_t c : cf) {
      const uint32_t opcode = uint32_t(c >> 44) & 0xF;
      if (!((opcode >= 1 && opcode <= 6) || opcode == 13 || opcode == 14)) continue;
      const size_t address = size_t(c & 0xFFF), count = size_t((c >> 12) & 0x7);
      if (address != 0) end = std::min(end, address);
      if (!count) continue;
      const size_t clause_last = address + count - 1;
      if (address <= first && last <= clause_last) {
        whole = true;
      } else if (address <= last && first <= clause_last) {
        return false;  // a clause runs only part of the range
      }
    }
  }
  return whole;
}

// Direct3D also reorders the vertex fetches of one temporary register (measured: VS CD057930742AFE84, 120
// words, Eden Prime pause menu: the r3.xy and r3.zw fetches of instructions 11 and 12 swap places, every other word
// unchanged). Cvar masseffect_native_vs_fetch_permutation (default on).
inline std::atomic<bool> g_vs_identity_fetch_permutation{true};

// The run of declared fetch instructions around `instruction` with contiguous indices and the same loader-masked
// first word in the library words (same opcode, source, destination register): [first, last].
template <class FetchRange, class Projection>
inline void VertexFetchRun(std::span<const uint32_t> selected, const FetchRange& fetches,
                           Projection instruction_index, size_t instruction, size_t& first, size_t& last) {
  const auto declared = [&](size_t i) {
    for (const auto& fetch : fetches)
      if (uint64_t(instruction_index(fetch)) == uint64_t(i)) return true;
    return false;
  };
  const uint32_t key = selected[instruction * 3] & 0x0007FFFFu;
  const size_t count = selected.size() / 3;
  first = last = instruction;
  while (first > 0 && declared(first - 1) && (selected[(first - 1) * 3] & 0x0007FFFFu) == key) --first;
  while (last + 1 < count && declared(last + 1) && (selected[(last + 1) * 3] & 0x0007FFFFu) == key) ++last;
}

// The loaded program is the library program with the destination swizzles of some declared fetches permuted
// inside runs of back-to-back fetches into the same temporary register, plus the usual loader patches. Exact
// otherwise: every non-fetch word identical (no ALU tolerance), every declared fetch word identical under the
// loader masks with the swizzle left out. In a permuted run the written component masks are non-zero and pairwise
// disjoint in both programs, nothing runs between the fetches (InstructionsInOneExecClause), and each library
// fetch has the loaded fetch with its written mask as partner, through a representable swizzle. The fetches then
// write the same components with the same data, only in another order, so the program computes the same.
// False when no swizzle is permuted (VertexShaderIdentityMatches decides that case).
template <class FetchRange, class Projection>
inline bool VertexShaderFetchPermutationMatches(ShaderIdentityView loaded, ShaderIdentityView selected,
                                                const FetchRange& fetches, Projection instruction_index) {
  if (loaded.stage != ShaderIdentityStage::Vertex || selected.stage != ShaderIdentityStage::Vertex ||
      loaded.words.empty() || loaded.words.size() != selected.words.size()) return false;
  const std::span<const uint32_t> l = loaded.words, s = selected.words;
  constexpr uint32_t keeps[3]{0x0007FFFFu, 0x80000FFFu, 0x80000000u};
  constexpr uint32_t compared[3]{0x0007FFFFu, 0x80000000u, 0x80000000u};
  bool permuted = false;
  for (const auto& fetch : fetches) {
    const uint64_t instruction = uint64_t(instruction_index(fetch));
    if (instruction > UINT32_MAX || instruction * 3 + 3 > s.size()) return false;
    const size_t start = size_t(instruction) * 3;
    if ((s[start] & 0x1Fu) != 0u) return false;  // Not a vertex fetch.
    for (size_t lane = 0; lane < 3; ++lane) {
      if ((s[start + lane] & keeps[lane]) != s[start + lane]) return false;  // Not normalized.
      if ((l[start + lane] & compared[lane]) != (s[start + lane] & compared[lane])) return false;
    }
    const uint32_t original = s[start + 1] & 0xFFFu;
    if (VertexFetchSwizzleRepresentable(original, l[start + 1] & 0xFFFu)) continue;
    size_t first = 0, last = 0;
    VertexFetchRun(s, fetches, instruction_index, size_t(instruction), first, last);
    if (first == last) return false;
    uint32_t selected_union = 0, loaded_union = 0;
    for (size_t k = first; k <= last; ++k) {
      const uint32_t sm = VertexFetchWrittenMask(s[k * 3 + 1] & 0xFFFu);
      const uint32_t lm = VertexFetchWrittenMask(l[k * 3 + 1] & 0xFFFu);
      if (!sm || !lm || (selected_union & sm) || (loaded_union & lm)) return false;
      selected_union |= sm;
      loaded_union |= lm;
    }
    if (selected_union != loaded_union) return false;
    const uint32_t wanted = VertexFetchWrittenMask(original);
    bool partner = false;
    for (size_t k = first; k <= last && !partner; ++k) {
      if (VertexFetchWrittenMask(l[k * 3 + 1] & 0xFFFu) != wanted) continue;
      if (!VertexFetchSwizzleRepresentable(original, l[k * 3 + 1] & 0xFFFu)) return false;
      partner = true;
    }
    if (!partner || !InstructionsInOneExecClause(s, first, last)) return false;
    permuted = true;
  }
  if (!permuted) return false;
  for (size_t word = 0; word < l.size(); ++word) {
    if (l[word] == s[word]) continue;
    bool declared_fetch = false;
    for (const auto& fetch : fetches) {
      const size_t start = size_t(instruction_index(fetch)) * 3;
      if (word >= start && word - start < 3) { declared_fetch = true; break; }
    }
    if (!declared_fetch) return false;  // ALU/CF words: exact, no tolerance.
  }
  return true;
}

// The identity every draw path checks: the ordered compare, or (cvar on) the same-register fetch permutation.
template <class FetchRange, class Projection>
inline bool VertexShaderProgramMatches(ShaderIdentityView loaded, ShaderIdentityView selected,
                                       const FetchRange& fetches, Projection instruction_index) {
  return VertexShaderIdentityMatches(loaded, selected, fetches, instruction_index) ||
         (g_vs_identity_fetch_permutation.load(std::memory_order_relaxed) &&
          VertexShaderFetchPermutationMatches(loaded, selected, fetches, instruction_index));
}
}  // namespace me::native

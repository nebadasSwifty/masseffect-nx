#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace me::native {

/*
 * Texture signs folded into the pixel shader (masseffect_native_fold_texture_signs).
 *
 * The shared constants block (set 4, binding 2) starts with 48 descriptor index words: the 2D, 3D and cube
 * heaps for the 16 fetch registers (word = heap * 16 + register). The renderer packs the four Xenos
 * TextureSign values of the fetch into bits 24-31 of that word, and every sampling helper of
 * shader_common.h does `packedSigns = index >> 24` and then runs a four-component loop over them
 * (x * 2 - 1 for "unsigned biased", a piecewise-linear decode for "gamma"). The signs are constant for a
 * draw, but the GPU still pays the loop: on GM20B NAK leaves two uniform branches per component (each
 * with its 13-cycle ssy/bra/sync) and keeps every path; the heaviest library pixel shaders lose 65-80 %
 * of their NAK instructions when the signs are known (docs/scene-shader-cost.md).
 *
 * FoldTextureSigns rewrites each `OpShiftRightLogical %uint %index %24` whose %index is a load of word w
 * of the shared block (through OpPhi, OpBitcast, OpLoad, OpCopyObject and a constant OpAccessChain) into
 * `OpCopyObject %uint <constant signs[w]>` when bit w of known_words is set. NVK's NIR then folds the
 * loop and the branches. The value is exactly what the shader would have computed, so the image cannot
 * change, provided that:
 *   - the pipeline reads the constants from the UBO (SPEC_CONSTANT_CONSTANTS_UBO set): the other side of
 *     the OpPhi, the 64-bit pointer path, is then dead and is not inspected;
 *   - signs[w] equals (shared word w) >> 24 of every draw that uses the pipeline: the caller puts the
 *     signs in the pipeline key.
 * Shifts whose operand cannot be traced, or traced to a word that is not known, are left alone.
 */
struct TextureSignsFold {
  uint32_t folded = 0;   // shifts replaced by a constant
  uint32_t unknown = 0;  // shifts by 24 of a shared word whose value the key does not carry
  uint32_t other = 0;    // shifts by 24 whose operand is not a shared descriptor word
};

inline bool FoldTextureSigns(const std::vector<uint32_t>& input, const uint8_t (&signs)[48],
                             uint64_t known_words, std::vector<uint32_t>& output,
                             TextureSignsFold& stats, std::string& reason) {
  output.clear();
  reason.clear();
  stats = {};
  const auto fail = [&](const char* why) {
    output.clear();
    reason = why;
    return false;
  };
  if (&input == &output) return fail("input/output alias");
  if (input.size() < 5 || input[0] != 0x07230203 || !input[3] || input[4])
    return fail("invalid SPIR-V header");

  struct Instruction {
    size_t at;
    uint32_t count, op;
  };
  std::vector<Instruction> instructions;
  std::unordered_map<uint32_t, size_t> definitions;  // result id -> instruction (the ops traced below)
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> constants;  // id -> (type, 32-bit value)
  std::unordered_set<uint32_t> int32_types;
  std::unordered_map<uint32_t, uint32_t> sets, bindings;
  std::unordered_map<uint32_t, uint32_t> variables;  // OpVariable id -> instruction index
  size_t first_function = 0;
  for (size_t at = 5; at < input.size();) {
    const uint32_t count = input[at] >> 16, op = input[at] & 0xFFFF;
    if (!count || count > input.size() - at) return fail("malformed instruction");
    const size_t index = instructions.size();
    instructions.push_back({at, count, op});
    const uint32_t* w = input.data() + at;
    switch (op) {
      case 21:  // OpTypeInt
        if (count == 4 && w[2] == 32) int32_types.insert(w[1]);
        break;
      case 43:  // OpConstant
        if (count == 4) constants[w[2]] = {w[1], w[3]};
        break;
      case 71:  // OpDecorate
        if (count == 4 && w[2] == 34) sets[w[1]] = w[3];      // DescriptorSet
        if (count == 4 && w[2] == 33) bindings[w[1]] = w[3];  // Binding
        break;
      case 54:  // OpFunction
        if (!first_function) first_function = at;
        break;
      case 59:  // OpVariable
        if (count >= 4) variables[w[2]] = index;
        break;
      case 245: case 124: case 61: case 65: case 66: case 83: case 194:
        // OpPhi, OpBitcast, OpLoad, OpAccessChain, OpInBoundsAccessChain, OpCopyObject,
        // OpShiftRightLogical: all have <result type> <result id>.
        if (count >= 3) definitions[w[2]] = index;
        break;
      default:
        break;
    }
    at += count;
  }
  if (!first_function) return fail("no function");
  uint32_t shared_block = 0;
  for (const auto& [id, index] : variables) {
    const auto set = sets.find(id), binding = bindings.find(id);
    if (set != sets.end() && set->second == 4 && binding != bindings.end() && binding->second == 2) {
      if (shared_block) return fail("two variables at set 4 binding 2");
      shared_block = id;
    }
  }
  output = input;
  if (!shared_block) return true;  // nothing to fold: the module is returned unchanged

  // The shared word an id was loaded from, or -1.
  const auto constant_value = [&](uint32_t id, uint32_t& value) {
    const auto it = constants.find(id);
    if (it == constants.end() || !int32_types.count(it->second.first)) return false;
    value = it->second.second;
    return true;
  };
  std::unordered_map<uint32_t, int32_t> memo;
  const auto trace = [&](auto&& self, uint32_t id, int depth) -> int32_t {
    if (depth > 8) return -1;
    if (const auto it = memo.find(id); it != memo.end()) return it->second;
    memo[id] = -1;  // cycles through loop phis end here
    const auto def = definitions.find(id);
    if (def == definitions.end()) return -1;
    const Instruction& ins = instructions[def->second];
    const uint32_t* w = input.data() + ins.at;
    int32_t result = -1;
    switch (ins.op) {
      case 245: {  // OpPhi: (value, parent)*; every traceable incoming value must agree
        int32_t agreed = -1;
        bool conflict = false;
        for (uint32_t k = 3; k + 1 < ins.count; k += 2) {
          const int32_t word = self(self, w[k], depth + 1);
          if (word < 0) continue;  // the 64-bit pointer path (dead with SPEC_CONSTANT_CONSTANTS_UBO)
          if (agreed >= 0 && agreed != word) conflict = true;
          agreed = word;
        }
        result = conflict ? -1 : agreed;
        break;
      }
      case 124: case 83:  // OpBitcast, OpCopyObject
        if (ins.count == 4) result = self(self, w[3], depth + 1);
        break;
      case 61:  // OpLoad <type> <id> <pointer> [memory operands]
        if (ins.count >= 4) result = self(self, w[3], depth + 1);
        break;
      case 65: case 66: {  // access chain: g_UboShared.v[k][j] = 0, k, j
        uint32_t member = 0, element = 0, component = 0;
        if (ins.count == 7 && w[3] == shared_block && constant_value(w[4], member) && member == 0 &&
            constant_value(w[5], element) && constant_value(w[6], component) && component < 4 &&
            element < 12) {
          result = int32_t(element * 4 + component);
        }
        break;
      }
      default:
        break;
    }
    memo[id] = result;
    return result;
  };

  // The constant for each folded value: reuse an existing one of the shift's type, otherwise a new id.
  uint32_t bound = input[3];
  std::unordered_map<uint64_t, uint32_t> existing;  // (type << 32 | value) -> id
  for (const auto& [id, constant] : constants)
    existing.emplace((uint64_t(constant.first) << 32) | constant.second, id);
  std::vector<uint32_t> new_constants;
  const auto constant_id = [&](uint32_t type, uint32_t value) {
    const uint64_t k = (uint64_t(type) << 32) | value;
    if (const auto it = existing.find(k); it != existing.end()) return it->second;
    const uint32_t id = bound++;
    existing.emplace(k, id);
    new_constants.insert(new_constants.end(), {(4u << 16) | 43u, type, id, value});
    return id;
  };

  std::vector<uint32_t> rewritten;
  rewritten.reserve(input.size() + 16);
  rewritten.insert(rewritten.end(), input.begin(), input.begin() + 5);
  size_t insert_at = 0;
  for (const Instruction& ins : instructions) {
    if (ins.at == first_function) insert_at = rewritten.size();
    const uint32_t* w = input.data() + ins.at;
    uint32_t shift = 0;
    if (ins.op == 194 && ins.count == 5 && int32_types.count(w[1]) && constant_value(w[4], shift) &&
        shift == 24) {
      const int32_t word = trace(trace, w[3], 0);
      if (word < 0 || word >= 48) {
        ++stats.other;
      } else if (!((known_words >> word) & 1)) {
        ++stats.unknown;
      } else {
        rewritten.insert(rewritten.end(), {(4u << 16) | 83u, w[1], w[2], constant_id(w[1], signs[word])});
        ++stats.folded;
        continue;
      }
    }
    rewritten.insert(rewritten.end(), w, w + ins.count);
  }
  if (!stats.folded) return true;  // output == input
  rewritten.insert(rewritten.begin() + insert_at, new_constants.begin(), new_constants.end());
  rewritten[3] = bound;
  output = std::move(rewritten);
  return true;
}

}  // namespace me::native

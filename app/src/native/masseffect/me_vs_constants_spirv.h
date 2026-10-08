#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace me::native {

/*
 * Vertex shader constants folded into the module (masseffect_native_fold_vs_constants).
 *
 * No vertex (or pixel) shader of the game uses Xenos bool or loop constants (checked on all 275 vertex shaders
 * and 400 random pixel shaders of the library: 0 `g_Booleans` tests, 0 loops). What steers the control flow
 * of the skinned vertex shaders is a FLOAT constant: `MaxBoneInfluences.x > 0 / 1 / 2 / 3` decides, three
 * times per shader (position, tangent frame, light vectors), how many bones are blended, and the translator
 * turns those jumps into a chain of `if (skipTo <= n)` blocks. NAK cannot fold any of it, so every vertex pays
 * for all four bone slots plus the chain, and the bone matrix loads (indexed `ldc`) are not shared between
 * the three passes. The sprite shaders do the same with `ScreenAlignment.x`. On top of that every vertex
 * input goes through remapInput(value, g_InputRemap(location)): with the identity code it takes a fast path,
 * but the 4-component select loop stays in the code and costs registers.
 *
 * Both are per-draw constants that the renderer knows when it builds the pipeline key:
 *   - the float register components (K * 4 + c of the vertex constant block, set 4 binding 0) that the module
 *     compares (direct operand of an OpFOrd* / OpFUnord* comparison): AnalyzeVertexConstants lists them;
 *   - the input remap words (shared block, set 4 binding 2, words 74 + location) whose code is the identity.
 *
 * FoldVertexConstants rewrites, for the listed components,
 *   `%x = OpCompositeExtract %float %v c`  where %v traces (OpPhi, OpLoad, OpCopyObject, constant
 *                                           OpAccessChain) to g_UboVertex.v[K]
 * into `%x = OpCopyObject %float <constant>`, and, for the identity remap locations,
 *   `%x = OpBitcast %uint %f`               where %f traces to shared word 74 + location
 * into `%x = OpCopyObject %uint 0xFFF`. The values are what the shader would have read, so the result is the
 * same by construction, provided that:
 *   - the pipeline reads the constants from the UBO (SPEC_CONSTANT_CONSTANTS_UBO): the 64-bit pointer side of
 *     each OpPhi is then dead and is not inspected;
 *   - every draw that uses the pipeline has the same values: the caller puts them in the pipeline key.
 * Other reads of the same register (not through a component extract) keep reading the UBO, which holds the
 * same value. Untraceable operands are left alone.
 */
constexpr uint32_t kVsConstantsMaxComponents = 3;
constexpr uint32_t kVsConstantsRemapWord = 74;  // shared word of g_InputRemap(0) (byte 296)
constexpr uint32_t kVsConstantsRemapIdentity = 0xFFF;

struct VertexConstantsInfo {
  // K * 4 + c of each compared float register component, ascending; at most kVsConstantsMaxComponents
  // (more_components says some were left out).
  uint32_t components[kVsConstantsMaxComponents] = {};
  uint32_t n_components = 0;
  bool more_components = false;
  uint32_t remap_locations = 0;  // bit L: the module reads g_InputRemap(L)
};

struct VertexConstantsFold {
  uint32_t folded_components = 0;  // extracts replaced by a float constant
  uint32_t folded_remaps = 0;      // remap words replaced by 0xFFF
};

namespace vs_constants_detail {

struct Module {
  struct Instruction {
    size_t at;
    uint32_t count, op;
  };
  const std::vector<uint32_t>* words = nullptr;
  std::vector<Instruction> instructions;
  std::unordered_map<uint32_t, size_t> definitions;  // result id -> instruction (the ops traced below)
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> constants;  // id -> (type, 32-bit value)
  std::unordered_set<uint32_t> int32_types, float32_types;
  size_t first_function = 0;
  uint32_t vertex_block = 0, shared_block = 0;

  bool Parse(const std::vector<uint32_t>& input, std::string& reason) {
    words = &input;
    if (input.size() < 5 || input[0] != 0x07230203 || !input[3] || input[4]) {
      reason = "invalid SPIR-V header";
      return false;
    }
    std::unordered_map<uint32_t, uint32_t> sets, bindings;
    std::vector<uint32_t> variables;
    for (size_t at = 5; at < input.size();) {
      const uint32_t count = input[at] >> 16, op = input[at] & 0xFFFF;
      if (!count || count > input.size() - at) {
        reason = "malformed instruction";
        return false;
      }
      const size_t index = instructions.size();
      instructions.push_back({at, count, op});
      const uint32_t* w = input.data() + at;
      switch (op) {
        case 21:  // OpTypeInt
          if (count == 4 && w[2] == 32) int32_types.insert(w[1]);
          break;
        case 22:  // OpTypeFloat
          if (count >= 3 && w[2] == 32) float32_types.insert(w[1]);
          break;
        case 43:  // OpConstant (32-bit only)
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
          if (count >= 4) variables.push_back(w[2]);
          break;
        case 245: case 124: case 61: case 65: case 66: case 83: case 81:
          // OpPhi, OpBitcast, OpLoad, OpAccessChain, OpInBoundsAccessChain, OpCopyObject,
          // OpCompositeExtract: all have <result type> <result id>.
          if (count >= 3) definitions[w[2]] = index;
          break;
        default:
          break;
      }
      at += count;
    }
    if (!first_function) {
      reason = "no function";
      return false;
    }
    for (const uint32_t id : variables) {
      const auto set = sets.find(id), binding = bindings.find(id);
      if (set == sets.end() || set->second != 4 || binding == bindings.end()) continue;
      if (binding->second != 0 && binding->second != 2) continue;
      uint32_t& slot = binding->second == 0 ? vertex_block : shared_block;
      if (slot) {
        reason = "two variables at the same set 4 binding";
        return false;
      }
      slot = id;
    }
    return true;
  }

  bool IntConstant(uint32_t id, uint32_t& value) const {
    const auto it = constants.find(id);
    if (it == constants.end() || !int32_types.count(it->second.first)) return false;
    value = it->second.second;
    return true;
  }

  // Follows OpPhi (every traceable incoming value must agree), OpBitcast, OpCopyObject and OpLoad down to a
  // constant access chain on `block`, and returns what `leaf` makes of it (-1 = not traceable).
  template <typename Leaf>
  int32_t Trace(uint32_t id, std::unordered_map<uint32_t, int32_t>& memo, const Leaf& leaf, int depth = 0) const {
    if (depth > 8) return -1;
    if (const auto it = memo.find(id); it != memo.end()) return it->second;
    memo[id] = -1;  // cycles through loop phis end here
    const auto def = definitions.find(id);
    if (def == definitions.end()) return -1;
    const Instruction& ins = instructions[def->second];
    const uint32_t* w = words->data() + ins.at;
    int32_t result = -1;
    switch (ins.op) {
      case 245: {  // OpPhi
        int32_t agreed = -1;
        bool conflict = false;
        for (uint32_t k = 3; k + 1 < ins.count; k += 2) {
          const int32_t value = Trace(w[k], memo, leaf, depth + 1);
          if (value < 0) continue;  // the 64-bit pointer path (dead with SPEC_CONSTANT_CONSTANTS_UBO)
          if (agreed >= 0 && agreed != value) conflict = true;
          agreed = value;
        }
        result = conflict ? -1 : agreed;
        break;
      }
      case 124: case 83:  // OpBitcast, OpCopyObject
        if (ins.count == 4) result = Trace(w[3], memo, leaf, depth + 1);
        break;
      case 61:  // OpLoad <type> <id> <pointer> [memory operands]
        if (ins.count >= 4) result = Trace(w[3], memo, leaf, depth + 1);
        break;
      case 65: case 66:  // access chain
        result = leaf(w, ins.count);
        break;
      default:
        break;
    }
    memo[id] = result;
    return result;
  }

  // g_UboVertex.v[K] (float4) -> K.
  int32_t VertexRegister(uint32_t id, std::unordered_map<uint32_t, int32_t>& memo) const {
    return Trace(id, memo, [&](const uint32_t* w, uint32_t count) -> int32_t {
      uint32_t member = 0, element = 0;
      if (vertex_block && count == 6 && w[3] == vertex_block && IntConstant(w[4], member) && member == 0 &&
          IntConstant(w[5], element) && element < 256)
        return int32_t(element);
      return -1;
    });
  }

  // g_UboShared.v[k][j] (float) -> shared word k * 4 + j.
  int32_t SharedWord(uint32_t id, std::unordered_map<uint32_t, int32_t>& memo) const {
    return Trace(id, memo, [&](const uint32_t* w, uint32_t count) -> int32_t {
      uint32_t member = 0, element = 0, component = 0;
      if (shared_block && count == 7 && w[3] == shared_block && IntConstant(w[4], member) && member == 0 &&
          IntConstant(w[5], element) && IntConstant(w[6], component) && element < 31 && component < 4)
        return int32_t(element * 4 + component);
      return -1;
    });
  }

  // For an OpCompositeExtract %float %v c of a vertex register: K * 4 + c, else -1.
  int32_t ExtractedComponent(const Instruction& ins, std::unordered_map<uint32_t, int32_t>& memo) const {
    const uint32_t* w = words->data() + ins.at;
    if (ins.op != 81 || ins.count != 5 || !float32_types.count(w[1]) || w[4] >= 4) return -1;
    const int32_t reg = VertexRegister(w[3], memo);
    return reg < 0 ? -1 : int32_t(reg * 4 + w[4]);
  }

  // For an OpBitcast %uint of a remap word: the location, else -1.
  int32_t RemapLocation(const Instruction& ins, std::unordered_map<uint32_t, int32_t>& memo) const {
    const uint32_t* w = words->data() + ins.at;
    if (ins.op != 124 || ins.count != 4 || !int32_types.count(w[1])) return -1;
    const int32_t word = SharedWord(w[3], memo);
    if (word < int32_t(kVsConstantsRemapWord) || word >= int32_t(kVsConstantsRemapWord + 16)) return -1;
    return word - int32_t(kVsConstantsRemapWord);
  }
};

}  // namespace vs_constants_detail

// What a vertex module could fold. Returns false only for a module it cannot parse.
inline bool AnalyzeVertexConstants(const std::vector<uint32_t>& input, VertexConstantsInfo& info,
                                   std::string& reason) {
  info = {};
  reason.clear();
  vs_constants_detail::Module m;
  if (!m.Parse(input, reason)) return false;
  std::unordered_map<uint32_t, int32_t> memo_vertex, memo_shared;
  std::vector<uint32_t> components;
  for (const auto& ins : m.instructions) {
    const uint32_t* w = input.data() + ins.at;
    if (ins.op >= 180 && ins.op <= 191 && ins.count == 5) {  // OpFOrdEqual .. OpFUnordGreaterThanEqual
      for (const uint32_t operand : {w[3], w[4]}) {
        const auto def = m.definitions.find(operand);
        if (def == m.definitions.end()) continue;
        const int32_t component = m.ExtractedComponent(m.instructions[def->second], memo_vertex);
        if (component >= 0 && std::find(components.begin(), components.end(), uint32_t(component)) ==
                                  components.end())
          components.push_back(uint32_t(component));
      }
    } else if (ins.op == 124) {
      const int32_t location = m.RemapLocation(ins, memo_shared);
      if (location >= 0) info.remap_locations |= 1u << location;
    }
  }
  std::sort(components.begin(), components.end());
  info.more_components = components.size() > kVsConstantsMaxComponents;
  info.n_components = uint32_t(std::min<size_t>(components.size(), kVsConstantsMaxComponents));
  std::copy_n(components.begin(), info.n_components, info.components);
  return true;
}

/*
 * Folds `values[i]` (raw 32-bit float bits) into every extract of component `components[i]` (i < n), and
 * 0xFFF into every read of the remap words of `remap_identity` (bit L = location L). With nothing folded the
 * output is the input.
 */
inline bool FoldVertexConstants(const std::vector<uint32_t>& input, const uint32_t* components,
                                const uint32_t* values, uint32_t n, uint32_t remap_identity,
                                std::vector<uint32_t>& output, VertexConstantsFold& stats, std::string& reason) {
  output.clear();
  reason.clear();
  stats = {};
  if (&input == &output) {
    reason = "input/output alias";
    return false;
  }
  vs_constants_detail::Module m;
  if (!m.Parse(input, reason)) return false;
  output = input;
  if ((!n || !m.vertex_block) && (!remap_identity || !m.shared_block)) return true;

  uint32_t bound = input[3];
  std::unordered_map<uint64_t, uint32_t> existing;  // (type << 32 | value) -> id
  for (const auto& [id, constant] : m.constants)
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

  std::unordered_map<uint32_t, int32_t> memo_vertex, memo_shared;
  std::vector<uint32_t> rewritten;
  rewritten.reserve(input.size() + 16);
  rewritten.insert(rewritten.end(), input.begin(), input.begin() + 5);
  size_t insert_at = 0;
  for (const auto& ins : m.instructions) {
    if (ins.at == m.first_function) insert_at = rewritten.size();
    const uint32_t* w = input.data() + ins.at;
    if (ins.op == 81 && n) {
      const int32_t component = m.ExtractedComponent(ins, memo_vertex);
      const uint32_t* end = components + n;
      const uint32_t* found = component < 0 ? end : std::find(components, end, uint32_t(component));
      if (found != end) {
        rewritten.insert(rewritten.end(), {(4u << 16) | 83u, w[1], w[2], constant_id(w[1], values[found - components])});
        ++stats.folded_components;
        continue;
      }
    } else if (ins.op == 124 && remap_identity) {
      const int32_t location = m.RemapLocation(ins, memo_shared);
      if (location >= 0 && ((remap_identity >> location) & 1)) {
        rewritten.insert(rewritten.end(), {(4u << 16) | 83u, w[1], w[2], constant_id(w[1], kVsConstantsRemapIdentity)});
        ++stats.folded_remaps;
        continue;
      }
    }
    rewritten.insert(rewritten.end(), w, w + ins.count);
  }
  if (!stats.folded_components && !stats.folded_remaps) return true;  // output == input
  rewritten.insert(rewritten.begin() + insert_at, new_constants.begin(), new_constants.end());
  rewritten[3] = bound;
  output = std::move(rewritten);
  return true;
}

}  // namespace me::native

#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace me::native {

/*
 * Exact pixel shader rewrites around descriptor access and clamps (docs/scene-shader-cost.md, "Descriptor and clamp
 * rewrites"). Three independent SPIR-V transforms of a library pixel shader module, each behind its own setting:
 *
 * A. ReadFltBoundsFromShared (masseffect_native_ps_flt_bounds_ubo). shader_common.h defines
 *    FLT_MIN = asfloat(0xFF7FFFFF) and FLT_MAX = asfloat(0x7F7FFFFF), and the translator clamps many results with
 *    clamp(x, FLT_MIN, FLT_MAX). SM50 FMNMX only has a 20-bit float immediate, so NAK materialises each bound with
 *    a 32-bit `mov` before every use. The transform loads both bounds once per function from shared words 122 / 123
 *    (bytes 488 / 492, inside the declared 31-float4 shared block, unused by shader_common.h) and uses those loads
 *    as the operands of the GLSL.std.450 FClamp / FMin / FMax / NClamp / NMin / NMax instructions instead of the
 *    constants. The renderer writes the same bit patterns there (kSharedWordFltMin / kSharedWordFltMax), so every
 *    operation sees the same values: same image by construction. NAK reads a constant-bank operand for free.
 *
 * B. CombineTexture2DFetches (masseffect_native_ps_combined_heap). Every 2D fetch loads a texture descriptor from
 *    the 2D heap (set 0) and a sampler descriptor from the sampler heap (set 3), each with its own index load,
 *    clamp and handle `ldc`, and combines them. The transform declares a third heap of COMBINED_IMAGE_SAMPLER
 *    descriptors (set kCombinedHeapSet, binding 0), redirects every access of the 2D heap to it (a load of the
 *    combined descriptor followed by OpImage gives the same image for size queries) and replaces each
 *    OpSampledImage(image of 2D word w, sampler of sampler word 48 + w) with the combined descriptor itself. The
 *    renderer writes into 2D word w the index of a combined descriptor made of exactly the texture view of the 2D
 *    slot and the sampler of the sampler slot the draw would have used (sign bits 24-31 unchanged), so every
 *    sample is the same operation on the same image with the same sampler. A pair whose sampler cannot be proven
 *    to be the one of the same register is left as OpImage + the separate sampler (still exact, just not cheaper).
 *    Soundness check: every read of shared words 0-15 must end in `>> 24` (sign byte, unchanged) or, through
 *    `& 0xFFFFFF`, in a 2D heap index; anything else and the module is not transformed.
 *
 * C. DropIndexMasks (masseffect_native_ps_no_index_mask). With the texture signs folded into the module
 *    (me_texture_signs_spirv.h) nothing reads bits 24-31 of the descriptor words of the folded registers any more,
 *    so the renderer can write those words with the top byte cleared and the `& 0xFFFFFF` that strips the signs
 *    becomes the identity: it is removed (OpCopyObject). Soundness check: every read of a cleared word must end
 *    in that mask; a remaining `>> 24` (not folded) or any other use and the module is not transformed.
 *
 * Every transform requires SPEC_CONSTANT_CONSTANTS_UBO (the 64-bit pointer side of each OpPhi is dead; it reads the
 * same block content anyway). On failure the output is cleared and `reason` says why; with nothing to do the output
 * equals the input.
 */
constexpr uint32_t kSharedWordFltMin = 122;  // byte 488: 0xFF7FFFFF (-FLT_MAX), what shader_common.h calls FLT_MIN
constexpr uint32_t kSharedWordFltMax = 123;  // byte 492: 0x7F7FFFFF (FLT_MAX)
constexpr uint32_t kFltMinBits = 0xFF7FFFFFu;
constexpr uint32_t kFltMaxBits = 0x7F7FFFFFu;
constexpr uint32_t kCombinedHeapSet = 5;

struct FltBoundsStats {
  uint32_t replaced = 0;   // clamp operands that now read the shared words
  uint32_t functions = 0;  // functions that got the two loads
};

struct CombinedFetchStats {
  uint32_t chains = 0;    // 2D heap accesses redirected to the combined heap
  uint32_t combined = 0;  // OpSampledImage replaced by the combined descriptor
  uint32_t separate = 0;  // OpSampledImage kept (image from the combined heap, separate sampler)
};

struct IndexMaskStats {
  uint32_t removed = 0;  // `& 0xFFFFFF` of a cleared descriptor word replaced by the word itself
};

namespace ps_desc_detail {

using Words = std::vector<uint32_t>;

enum : uint32_t {
  kOpName = 5, kOpMemberName = 6, kOpString = 7, kOpLine = 8, kOpExtInstImport = 11, kOpExtInst = 12,
  kOpEntryPoint = 15, kOpExecutionMode = 16, kOpTypeInt = 21, kOpTypeFloat = 22, kOpTypeImage = 25,
  kOpTypeSampledImage = 27, kOpTypeRuntimeArray = 29, kOpTypePointer = 32, kOpConstant = 43, kOpFunction = 54,
  kOpVariable = 59, kOpLoad = 61, kOpAccessChain = 65, kOpInBoundsAccessChain = 66, kOpDecorate = 71,
  kOpMemberDecorate = 72, kOpDecorationGroup = 73, kOpGroupDecorate = 74, kOpGroupMemberDecorate = 75,
  kOpCompositeExtract = 81, kOpCopyObject = 83, kOpSampledImage = 86, kOpImage = 100, kOpBitcast = 124,
  kOpShiftRightLogical = 194, kOpBitwiseAnd = 199, kOpPhi = 245, kOpLabel = 248, kOpNoLine = 317,
  kOpDecorateId = 332, kOpDecorateString = 5632, kOpMemberDecorateString = 5633,
};
enum : uint32_t { kDecorationBinding = 33, kDecorationDescriptorSet = 34, kDecorationNonUniform = 5300 };

struct Instruction {
  uint32_t op = 0;
  Words w;  // operands, without the opcode/length word
};

inline bool IsAnnotationOrDebug(uint32_t op) {
  switch (op) {
    case 3: case 4: case kOpName: case kOpMemberName: case kOpString: case kOpLine: case kOpEntryPoint:
    case kOpExecutionMode: case kOpDecorate: case kOpMemberDecorate: case kOpDecorationGroup: case kOpGroupDecorate:
    case kOpGroupMemberDecorate: case kOpNoLine: case kOpDecorateId: case kOpDecorateString:
    case kOpMemberDecorateString: case 331:  // OpExecutionModeId
      return true;
    default:
      return false;
  }
}

// Instructions whose result id this file needs to find (operand 1 = result, operand 0 = result type).
inline bool HasTypedResult(uint32_t op) {
  switch (op) {
    case kOpExtInst: case kOpConstant: case 41: case 42: case 44: case 46: case 50: case 51: case 52:
    case kOpVariable: case kOpLoad: case kOpAccessChain: case kOpInBoundsAccessChain: case 79: case 80:
    case kOpCompositeExtract: case kOpCopyObject: case kOpSampledImage: case kOpImage: case kOpBitcast:
    case kOpShiftRightLogical: case kOpBitwiseAnd: case kOpPhi: case kOpFunction:
      return true;
    default:
      return false;
  }
}

// How many leading operands of an instruction may be ids (the literal tails of the common instructions are left out;
// anything not listed counts every operand, which can only make a check fail).
inline size_t IdOperands(const Instruction& ins) {
  const size_t n = ins.w.size();
  switch (ins.op) {
    case 14: case 17: case 21: case 22: case 10: case 11:  // memory model, capability, int, float, extension, import
      return 0;
    case kOpTypePointer:  // result, storage class, type
      return n == 3 ? 1 : n;
    case kOpTypeImage:  // result, sampled type, literals
      return std::min<size_t>(n, 2);
    case kOpConstant: case 50:  // type, result, literal value(s)
      return std::min<size_t>(n, 2);
    case kOpLoad:  // type, result, pointer, memory operands
      return std::min<size_t>(n, 3);
    case 62:  // OpStore: pointer, object, memory operands
      return std::min<size_t>(n, 2);
    case kOpCompositeExtract:  // type, result, composite, literal indices
      return std::min<size_t>(n, 3);
    case 82:  // OpCompositeInsert: type, result, object, composite, literal indices
      return std::min<size_t>(n, 4);
    case 79:  // OpVectorShuffle: type, result, v1, v2, literal components
      return std::min<size_t>(n, 4);
    case kOpExtInst:  // type, result, set, literal instruction, operands: the literal stays (cannot be a use)
      return n;
    case 247: case 246:  // OpSelectionMerge (merge, mask), OpLoopMerge (merge, continue, mask)
      return ins.op == 247 ? std::min<size_t>(n, 1) : std::min<size_t>(n, 2);
    case kOpFunction:  // type, result, control mask, function type
      return n;
    default:
      return n;
  }
}

// Calls fn(id) for each operand of `ins` that may be an id (literal positions of the common instructions skipped).
template <typename Fn>
inline void ForEachIdOperand(const Instruction& ins, Fn&& fn) {
  const size_t ids = IdOperands(ins);
  for (size_t k = 0; k < ids; ++k) {
    if (ins.op == kOpVariable && k == 2) continue;  // storage class literal
    if (ins.op == kOpExtInst && k == 3) continue;   // instruction number literal
    if (ins.op == 52 && k == 2) continue;           // OpSpecConstantOp: opcode literal
    if (ins.op == 251 && k >= 2 && !(k & 1)) continue;  // OpSwitch: case literals (32-bit selector)
    if (ins.op >= 87 && ins.op <= 97) {             // image sample / fetch / gather: image operands mask
      const bool dref = ins.op == 89 || ins.op == 90 || ins.op == 92 || ins.op == 93 || ins.op == 97;
      if (k == (dref ? 5u : 4u)) continue;
    }
    if (ins.op == 250 && k >= 3) continue;          // OpBranchConditional: branch weights
    fn(ins.w[k]);
  }
}

struct Module {
  Words header;
  std::vector<Instruction> code;
  std::unordered_map<uint32_t, size_t> def;  // result id -> instruction index
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> constants;  // 32-bit OpConstant: id -> (type, value)
  std::unordered_set<uint32_t> int32_types, float32_types;
  std::unordered_map<uint32_t, uint32_t> sets, bindings;
  uint32_t glsl = 0;  // OpExtInstImport "GLSL.std.450"

  bool Parse(const Words& input, std::string& reason) {
    if (input.size() < 5 || input[0] != 0x07230203 || !input[3] || input[4]) {
      reason = "invalid SPIR-V header";
      return false;
    }
    header.assign(input.begin(), input.begin() + 5);
    for (size_t at = 5; at < input.size();) {
      const uint32_t count = input[at] >> 16, op = input[at] & 0xFFFF;
      if (!count || count > input.size() - at) {
        reason = "malformed instruction";
        return false;
      }
      Instruction ins;
      ins.op = op;
      ins.w.assign(input.begin() + at + 1, input.begin() + at + count);
      code.push_back(std::move(ins));
      at += count;
    }
    for (size_t i = 0; i < code.size(); ++i) Index(i);
    return true;
  }

  void Index(size_t i) {
    const Instruction& ins = code[i];
    const Words& w = ins.w;
    if (HasTypedResult(ins.op) && w.size() >= 2) def[w[1]] = i;
    if (ins.op >= 19 && ins.op <= 38 && !w.empty()) def[w[0]] = i;  // OpType*
    if ((ins.op == kOpExtInstImport || ins.op == kOpLabel) && !w.empty()) def[w[0]] = i;
    switch (ins.op) {
      case kOpTypeInt:
        if (w.size() == 3 && w[1] == 32) int32_types.insert(w[0]);
        break;
      case kOpTypeFloat:
        if (w.size() >= 2 && w[1] == 32) float32_types.insert(w[0]);
        break;
      case kOpConstant:
        if (w.size() == 3) constants[w[1]] = {w[0], w[2]};
        break;
      case kOpDecorate:
        if (w.size() == 3 && w[1] == kDecorationDescriptorSet) sets[w[0]] = w[2];
        if (w.size() == 3 && w[1] == kDecorationBinding) bindings[w[0]] = w[2];
        break;
      case kOpExtInstImport: {
        static const char kName[] = "GLSL.std.450";
        if (w.size() >= 2 && std::memcmp(w.data() + 1, kName, std::min(sizeof(kName), (w.size() - 1) * 4)) == 0)
          glsl = w[0];
        break;
      }
      default:
        break;
    }
  }

  bool IntConstant(uint32_t id, uint32_t& value) const {
    const auto it = constants.find(id);
    if (it == constants.end() || !int32_types.count(it->second.first)) return false;
    value = it->second.second;
    return true;
  }

  // The module-scope variable at (set, binding); 0 if none; `twice` if more than one.
  uint32_t Variable(uint32_t set, uint32_t binding, bool& twice) const {
    uint32_t found = 0;
    twice = false;
    for (const Instruction& ins : code) {
      if (ins.op == kOpFunction) break;
      if (ins.op != kOpVariable || ins.w.size() < 3) continue;
      const auto s = sets.find(ins.w[1]), b = bindings.find(ins.w[1]);
      if (s == sets.end() || b == bindings.end() || s->second != set || b->second != binding) continue;
      if (found) twice = true;
      found = ins.w[1];
    }
    return found;
  }

  const Instruction* Def(uint32_t id) const {
    const auto it = def.find(id);
    return it == def.end() ? nullptr : &code[it->second];
  }

  uint32_t NewId() { return header[3]++; }

  // A 32-bit OpConstant of `type` with `value`: the existing one, or a new one inserted before the first function.
  uint32_t Constant(uint32_t type, uint32_t value, std::vector<Instruction>& added) {
    for (const auto& [id, c] : constants)
      if (c.first == type && c.second == value) return id;
    const uint32_t id = NewId();
    added.push_back({kOpConstant, {type, id, value}});
    constants[id] = {type, value};
    return id;
  }

  void InsertBeforeFirstFunction(std::vector<Instruction>& added) {
    if (added.empty()) return;
    size_t at = 0;
    while (at < code.size() && code[at].op != kOpFunction) ++at;
    code.insert(code.begin() + at, added.begin(), added.end());
    added.clear();
  }

  void Emit(Words& out) const {
    out = header;
    for (const Instruction& ins : code) {
      out.push_back((uint32_t(ins.w.size() + 1) << 16) | ins.op);
      out.insert(out.end(), ins.w.begin(), ins.w.end());
    }
  }

  // Every instruction that has `id` among its operands (annotations, debug and the definition itself excluded).
  // Conservative: a literal operand equal to the id also counts (it can only make a check fail).
  std::unordered_map<uint32_t, std::vector<size_t>> Uses() const {
    std::unordered_map<uint32_t, std::vector<size_t>> uses;
    for (size_t i = 0; i < code.size(); ++i) {
      if (IsAnnotationOrDebug(code[i].op)) continue;
      ForEachIdOperand(code[i], [&](uint32_t x) {
        auto& list = uses[x];
        if (list.empty() || list.back() != i) list.push_back(i);
      });
    }
    for (auto& [id, list] : uses) {
      const auto d = def.find(id);
      if (d == def.end()) continue;
      for (size_t k = 0; k < list.size(); ++k)
        if (list[k] == d->second) { list.erase(list.begin() + k); break; }
    }
    return uses;
  }
};

// Shared block word an id was read from (through OpPhi agreement, OpBitcast, OpCopyObject, OpLoad, a constant
// 3-index OpAccessChain and, with through_mask, `& 0x00FFFFFF`), or -1.
inline int32_t TraceSharedWord(const Module& m, uint32_t shared, uint32_t id, bool through_mask, int depth = 0) {
  if (depth > 12) return -1;
  const Instruction* ins = m.Def(id);
  if (!ins) return -1;
  const Words& w = ins->w;
  switch (ins->op) {
    case kOpPhi: {
      int32_t agreed = -1;
      for (size_t k = 2; k + 1 < w.size(); k += 2) {
        if (w[k] == id) continue;
        const int32_t word = TraceSharedWord(m, shared, w[k], through_mask, depth + 1);
        if (word < 0) continue;  // the 64-bit pointer path (dead with SPEC_CONSTANT_CONSTANTS_UBO)
        if (agreed >= 0 && agreed != word) return -1;
        agreed = word;
      }
      return agreed;
    }
    case kOpBitcast: case kOpCopyObject:
      return w.size() == 3 ? TraceSharedWord(m, shared, w[2], through_mask, depth + 1) : -1;
    case kOpLoad:
      return w.size() >= 3 ? TraceSharedWord(m, shared, w[2], through_mask, depth + 1) : -1;
    case kOpBitwiseAnd: {
      if (!through_mask || w.size() != 4) return -1;
      uint32_t value = 0;
      if (m.IntConstant(w[3], value) && value == 0x00FFFFFFu)
        return TraceSharedWord(m, shared, w[2], through_mask, depth + 1);
      if (m.IntConstant(w[2], value) && value == 0x00FFFFFFu)
        return TraceSharedWord(m, shared, w[3], through_mask, depth + 1);
      return -1;
    }
    case kOpAccessChain: case kOpInBoundsAccessChain: {
      uint32_t member = 0, element = 0, component = 0;
      if (w.size() == 6 && w[2] == shared && m.IntConstant(w[3], member) && member == 0 &&
          m.IntConstant(w[4], element) && m.IntConstant(w[5], component) && component < 4 && element < 31)
        return int32_t(element * 4 + component);
      return -1;
    }
    default:
      return -1;
  }
}

/*
 * Forward taint of the values read from the shared block words in `words` (bit w = word w, w < 128). Values
 * propagate through OpLoad, OpCompositeExtract (per component), OpBitcast, OpCopyObject and OpPhi; every other use
 * is passed to `policy(instruction index, tainted id)`, which returns 0 = fail, 1 = sink (allowed, the result is
 * not tainted), 2 = propagate (the result is tainted). An access to the block that may reach a tainted word
 * without constant indices fails. Returns false with `reason` on failure; `sinks` gets the sink instructions.
 */
template <typename Policy>
bool TaintSharedWords(const Module& m, uint32_t shared, const std::array<uint64_t, 2>& words, Policy&& policy,
                      std::vector<size_t>& sinks, std::string& reason) {
  sinks.clear();
  const auto has = [&](uint32_t word) { return word < 128 && ((words[word >> 6] >> (word & 63)) & 1); };
  struct Taint {
    bool vector = false;
    uint32_t base = 0;   // first word of the float4 (vectors)
    uint32_t mask = 0;   // tainted components (vectors)
  };
  std::unordered_map<uint32_t, Taint> tainted;
  std::vector<uint32_t> work;
  const auto add = [&](uint32_t id, const Taint& t) {
    if (tainted.emplace(id, t).second) work.push_back(id);
  };
  const auto uses = m.Uses();
  if (const auto u = uses.find(shared); u != uses.end()) {
    for (const size_t i : u->second) {
      const Instruction& ins = m.code[i];
      const Words& w = ins.w;
      if ((ins.op != kOpAccessChain && ins.op != kOpInBoundsAccessChain) || w.size() < 3 || w[2] != shared) {
        reason = "the shared block is used other than through an access chain (op " + std::to_string(ins.op) + ")";
        return false;
      }
      uint32_t member = 0, element = 0, component = 0;
      if (w.size() == 6 && m.IntConstant(w[3], member) && member == 0 && m.IntConstant(w[4], element) &&
          m.IntConstant(w[5], component) && component < 4) {
        if (has(element * 4 + component)) add(w[1], {false, element * 4 + component, 0});
      } else if (w.size() == 5 && m.IntConstant(w[3], member) && member == 0 && m.IntConstant(w[4], element)) {
        uint32_t mask = 0;
        for (uint32_t c = 0; c < 4; ++c)
          if (has(element * 4 + c)) mask |= 1u << c;
        if (mask) add(w[1], {true, element * 4, mask});
      } else {
        reason = "dynamic or whole-block access to the shared block";
        return false;
      }
    }
  }
  while (!work.empty()) {
    const uint32_t id = work.back();
    work.pop_back();
    const Taint t = tainted[id];
    const auto u = uses.find(id);
    if (u == uses.end()) continue;
    for (const size_t i : u->second) {
      const Instruction& ins = m.code[i];
      const Words& w = ins.w;
      switch (ins.op) {
        case kOpLoad:
          if (w.size() >= 3 && w[2] == id) {
            add(w[1], t);
            continue;
          }
          break;
        case kOpCompositeExtract:
          if (w.size() == 4 && w[2] == id && t.vector && w[3] < 4) {
            if ((t.mask >> w[3]) & 1) add(w[1], {false, t.base + w[3], 0});
            continue;
          }
          break;
        case kOpBitcast: case kOpCopyObject:
          if (w.size() == 3 && w[2] == id) {
            add(w[1], t);
            continue;
          }
          break;
        case kOpPhi:
          if (!t.vector) {
            add(w[1], t);
            continue;
          }
          break;
        default:
          break;
      }
      if (t.vector) {
        reason = "a float4 holding a descriptor word is used as a whole";
        return false;
      }
      const int verdict = policy(i, id);
      if (verdict == 0) {
        reason = "a descriptor word has a use the rewrite cannot account for (op " + std::to_string(ins.op) + ")";
        return false;
      }
      if (verdict == 1) {
        sinks.push_back(i);
      } else if (w.size() >= 2) {
        add(w[1], {false, 0, 0});
      }
    }
  }
  return true;
}

inline bool IsMaskOf(const Module& m, const Instruction& ins, uint32_t id) {
  if (ins.op != kOpBitwiseAnd || ins.w.size() != 4) return false;
  uint32_t value = 0;
  return (ins.w[2] == id && m.IntConstant(ins.w[3], value) && value == 0x00FFFFFFu) ||
         (ins.w[3] == id && m.IntConstant(ins.w[2], value) && value == 0x00FFFFFFu);
}

}  // namespace ps_desc_detail

// --- A ------------------------------------------------------------------------------------------------------------
inline bool ReadFltBoundsFromShared(const std::vector<uint32_t>& input, std::vector<uint32_t>& output,
                                    FltBoundsStats& stats, std::string& reason) {
  using namespace ps_desc_detail;
  output.clear();
  reason.clear();
  stats = {};
  Module m;
  if (!m.Parse(input, reason)) return false;
  bool twice = false;
  const uint32_t shared = m.Variable(4, 2, twice);
  if (twice) {
    reason = "two variables at set 4 binding 2";
    return false;
  }
  std::unordered_set<uint32_t> lo, hi;
  for (const auto& [id, c] : m.constants) {
    if (!m.float32_types.count(c.first)) continue;
    if (c.second == kFltMinBits) lo.insert(id);
    if (c.second == kFltMaxBits) hi.insert(id);
  }
  // A 3-index access chain into the shared block gives the pointer type and the index types to reuse.
  const Instruction* model = nullptr;
  for (const Instruction& ins : m.code) {
    uint32_t member = 0;
    if ((ins.op == kOpAccessChain || ins.op == kOpInBoundsAccessChain) && ins.w.size() == 6 && shared &&
        ins.w[2] == shared && m.IntConstant(ins.w[3], member) && member == 0 && m.constants.count(ins.w[4]) &&
        m.constants.count(ins.w[5])) {
      const Instruction* pointer = m.Def(ins.w[0]);
      if (pointer && pointer->op == kOpTypePointer && pointer->w.size() == 3 && m.float32_types.count(pointer->w[2])) {
        model = &ins;
        break;
      }
    }
  }
  const auto is_bound_operand = [&](const Instruction& ins) {
    if (ins.op != kOpExtInst || ins.w.size() < 5 || !m.glsl || ins.w[2] != m.glsl) return false;
    switch (ins.w[3]) {
      case 37: case 40: case 43: case 79: case 80: case 81: break;  // FMin FMax FClamp NMin NMax NClamp
      default: return false;
    }
    for (size_t k = 4; k < ins.w.size(); ++k)
      if (lo.count(ins.w[k]) || hi.count(ins.w[k])) return true;
    return false;
  };
  bool any = false;
  for (const Instruction& ins : m.code) any |= is_bound_operand(ins);
  if (!any || !model) {
    output = input;  // nothing to rewrite (or no shared block access to model the loads on)
    return true;
  }
  const uint32_t pointer_type = model->w[0], member0 = model->w[3];
  const uint32_t element_type = m.constants[model->w[4]].first, component_type = m.constants[model->w[5]].first;
  const uint32_t float_type = m.Def(pointer_type)->w[2];
  std::vector<Instruction> added;
  const uint32_t element = m.Constant(element_type, kSharedWordFltMin / 4, added);
  const uint32_t component_lo = m.Constant(component_type, kSharedWordFltMin % 4, added);
  const uint32_t component_hi = m.Constant(component_type, kSharedWordFltMax % 4, added);
  m.InsertBeforeFirstFunction(added);

  std::vector<Instruction> code;
  code.reserve(m.code.size() + 16);
  uint32_t value_lo = 0, value_hi = 0;
  // Two passes per function: find whether it uses a bound, then copy it with the loads after its OpVariables.
  for (size_t i = 0; i < m.code.size();) {
    if (m.code[i].op != kOpFunction) {
      code.push_back(m.code[i]);
      ++i;
      continue;
    }
    size_t end = i;
    bool uses = false;
    while (end < m.code.size() && m.code[end].op != 56) {  // OpFunctionEnd
      uses |= is_bound_operand(m.code[end]);
      ++end;
    }
    if (end < m.code.size()) ++end;
    if (!uses) {
      code.insert(code.end(), m.code.begin() + i, m.code.begin() + end);
      i = end;
      continue;
    }
    ++stats.functions;
    value_lo = m.NewId();
    value_hi = m.NewId();
    const uint32_t pointer_lo = m.NewId(), pointer_hi = m.NewId();
    bool in_first_block = false, inserted = false;
    for (size_t k = i; k < end; ++k) {
      Instruction ins = m.code[k];
      if (!inserted && in_first_block && ins.op != kOpVariable) {
        code.push_back({kOpAccessChain, {pointer_type, pointer_lo, shared, member0, element, component_lo}});
        code.push_back({kOpLoad, {float_type, value_lo, pointer_lo}});
        code.push_back({kOpAccessChain, {pointer_type, pointer_hi, shared, member0, element, component_hi}});
        code.push_back({kOpLoad, {float_type, value_hi, pointer_hi}});
        inserted = true;
      }
      if (ins.op == kOpLabel && !in_first_block) in_first_block = true;
      if (is_bound_operand(ins)) {
        for (size_t o = 4; o < ins.w.size(); ++o) {
          if (lo.count(ins.w[o])) {
            ins.w[o] = value_lo;
            ++stats.replaced;
          } else if (hi.count(ins.w[o])) {
            ins.w[o] = value_hi;
            ++stats.replaced;
          }
        }
      }
      code.push_back(std::move(ins));
    }
    if (!inserted) {
      reason = "function without a body";
      return false;
    }
    i = end;
  }
  m.code = std::move(code);
  m.Emit(output);
  return true;
}

// --- C ------------------------------------------------------------------------------------------------------------
// cleared_words: bit w (w < 48) = the renderer writes descriptor word w with bits 24-31 cleared.
inline bool DropIndexMasks(const std::vector<uint32_t>& input, uint64_t cleared_words, std::vector<uint32_t>& output,
                           IndexMaskStats& stats, std::string& reason) {
  using namespace ps_desc_detail;
  output.clear();
  reason.clear();
  stats = {};
  Module m;
  if (!m.Parse(input, reason)) return false;
  bool twice = false;
  const uint32_t shared = m.Variable(4, 2, twice);
  if (twice) {
    reason = "two variables at set 4 binding 2";
    return false;
  }
  cleared_words &= (uint64_t(1) << 48) - 1;
  if (!shared || !cleared_words) {
    output = input;
    return true;
  }
  std::vector<size_t> sinks;
  const auto policy = [&](size_t i, uint32_t id) { return IsMaskOf(m, m.code[i], id) ? 1 : 0; };
  if (!TaintSharedWords(m, shared, {cleared_words, 0}, policy, sinks, reason)) return false;
  for (const size_t i : sinks) {
    Instruction& ins = m.code[i];
    uint32_t value = 0;
    const uint32_t kept = (m.IntConstant(ins.w[3], value) && value == 0x00FFFFFFu) ? ins.w[2] : ins.w[3];
    ins = {kOpCopyObject, {ins.w[0], ins.w[1], kept}};
    ++stats.removed;
  }
  if (!stats.removed) {
    output = input;
    return true;
  }
  m.Emit(output);
  return true;
}

// --- B ------------------------------------------------------------------------------------------------------------
inline bool CombineTexture2DFetches(const std::vector<uint32_t>& input, std::vector<uint32_t>& output,
                                    CombinedFetchStats& stats, std::string& reason,
                                    uint32_t combined_set = kCombinedHeapSet, uint32_t combined_binding = 0) {
  using namespace ps_desc_detail;
  output.clear();
  reason.clear();
  stats = {};
  Module m;
  if (!m.Parse(input, reason)) return false;
  bool twice = false;
  const uint32_t heap = m.Variable(0, 0, twice);
  if (twice) {
    reason = "two variables at set 0 binding 0";
    return false;
  }
  if (!heap) {
    output = input;  // no 2D fetch
    return true;
  }
  {
    bool taken = false;
    if (m.Variable(combined_set, combined_binding, taken) || taken) {
      reason = "the combined heap binding is already used";
      return false;
    }
  }
  const uint32_t shared = m.Variable(4, 2, twice);
  if (!shared || twice) {
    reason = "no shared block (set 4 binding 2)";
    return false;
  }
  const uint32_t samplers = m.Variable(3, 0, twice);
  // Types: heap = OpVariable %ptr UniformConstant, %ptr = OpTypePointer UniformConstant %ra, %ra = runtime array.
  const Instruction* var = m.Def(heap);
  const Instruction* pointer = var ? m.Def(var->w[0]) : nullptr;
  const Instruction* array = pointer && pointer->op == kOpTypePointer && pointer->w.size() == 3 ? m.Def(pointer->w[2])
                                                                                               : nullptr;
  if (!array || array->op != kOpTypeRuntimeArray || array->w.size() != 2) {
    reason = "the 2D heap is not a runtime array";
    return false;
  }
  const uint32_t image_type = array->w[1];
  const Instruction* image = m.Def(image_type);
  if (!image || image->op != kOpTypeImage) {
    reason = "the 2D heap does not hold images";
    return false;
  }

  // Soundness: shared words 0-15 may only feed `>> 24` (sign byte, unchanged) or, through `& 0xFFFFFF` or a copy,
  // a 2D heap index.
  const auto uses = m.Uses();
  {
    std::vector<size_t> sinks;
    const auto policy = [&](size_t i, uint32_t id) -> int {
      const Instruction& ins = m.code[i];
      uint32_t value = 0;
      if (ins.op == kOpShiftRightLogical && ins.w.size() == 4 && ins.w[2] == id && m.IntConstant(ins.w[3], value) &&
          value == 24)
        return 1;
      if (IsMaskOf(m, ins, id)) return 2;
      if ((ins.op == kOpAccessChain || ins.op == kOpInBoundsAccessChain) && ins.w.size() == 4 && ins.w[2] == heap &&
          ins.w[3] == id)
        return 1;
      return 0;
    };
    if (!TaintSharedWords(m, shared, {0xFFFFull, 0}, policy, sinks, reason)) return false;
  }
  // Every use of the heap: a one-index access chain whose index is a 2D descriptor word; every use of such a chain:
  // an OpLoad.
  std::unordered_map<uint32_t, uint32_t> chain_word;  // chain id -> word
  if (const auto u = uses.find(heap); u != uses.end()) {
    for (const size_t i : u->second) {
      const Instruction& ins = m.code[i];
      if ((ins.op != kOpAccessChain && ins.op != kOpInBoundsAccessChain) || ins.w.size() != 4 || ins.w[2] != heap) {
        reason = "the 2D heap is used other than through a one-index access chain (op " + std::to_string(ins.op) + ")";
        return false;
      }
      const int32_t word = TraceSharedWord(m, shared, ins.w[3], true);
      if (word < 0 || word >= 16) {
        reason = "a 2D heap index is not a 2D descriptor word";
        return false;
      }
      chain_word[ins.w[1]] = uint32_t(word);
    }
  }
  std::unordered_map<uint32_t, uint32_t> load_chain;  // image load id -> chain id
  for (const auto& [chain, word] : chain_word) {
    (void)word;
    const auto u = uses.find(chain);
    if (u == uses.end()) continue;
    for (const size_t i : u->second) {
      const Instruction& ins = m.code[i];
      if (ins.op != kOpLoad || ins.w.size() < 3 || ins.w[2] != chain || ins.w[0] != image_type) {
        reason = "a 2D heap element is used other than through an OpLoad (op " + std::to_string(ins.op) + ")";
        return false;
      }
      load_chain[ins.w[1]] = chain;
    }
  }

  // New types and the combined heap variable, right after the 2D heap variable.
  std::vector<Instruction> types;
  uint32_t sampled_type = 0;
  for (const Instruction& ins : m.code)
    if (ins.op == kOpTypeSampledImage && ins.w.size() == 2 && ins.w[1] == image_type) sampled_type = ins.w[0];
  if (!sampled_type) {
    sampled_type = m.NewId();
    types.push_back({kOpTypeSampledImage, {sampled_type, image_type}});
  }
  const uint32_t array2 = m.NewId(), pointer_array2 = m.NewId(), pointer_sampled = m.NewId(), combined = m.NewId();
  types.push_back({kOpTypeRuntimeArray, {array2, sampled_type}});
  types.push_back({kOpTypePointer, {pointer_array2, 0, array2}});
  types.push_back({kOpTypePointer, {pointer_sampled, 0, sampled_type}});
  types.push_back({kOpVariable, {pointer_array2, combined, 0}});

  std::unordered_set<uint32_t> nonuniform;
  for (const Instruction& ins : m.code)
    if (ins.op == kOpDecorate && ins.w.size() == 2 && ins.w[1] == kDecorationNonUniform) nonuniform.insert(ins.w[0]);

  // Sampler pairing: OpLoad of a one-index access chain into the sampler heap whose index is word 48 + register.
  const auto sampler_word = [&](uint32_t sampler) -> int32_t {
    const Instruction* load = m.Def(sampler);
    if (!samplers || !load || load->op != kOpLoad || load->w.size() < 3) return -1;
    const Instruction* chain = m.Def(load->w[2]);
    if (!chain || (chain->op != kOpAccessChain && chain->op != kOpInBoundsAccessChain) || chain->w.size() != 4 ||
        chain->w[2] != samplers)
      return -1;
    return TraceSharedWord(m, shared, chain->w[3], false);
  };

  const uint32_t version = m.header[1];
  std::unordered_map<uint32_t, uint32_t> load_combined;  // image load id -> combined load id
  std::vector<Instruction> code;
  code.reserve(m.code.size() + 2 * load_chain.size() + 16);
  bool decorated = false;
  size_t decorations_at = 0;
  std::vector<uint32_t> nonuniform_loads;  // new combined loads whose image load was NonUniform
  for (size_t i = 0; i < m.code.size(); ++i) {
    Instruction ins = m.code[i];
    if (ins.op == kOpEntryPoint && version >= 0x00010400) {
      // SPIR-V 1.4+: every global the entry point uses is in its interface.
      bool lists_heap = false;
      size_t k = 2;
      while (k < ins.w.size()) {  // skip the name (nul-terminated string)
        const uint32_t x = ins.w[k++];
        if (!(x & 0xFF000000u) || !(x & 0x00FF0000u) || !(x & 0x0000FF00u) || !(x & 0x000000FFu)) break;
      }
      for (; k < ins.w.size(); ++k) lists_heap |= ins.w[k] == heap;
      if (lists_heap) ins.w.push_back(combined);
      code.push_back(std::move(ins));
      continue;
    }
    if (ins.op == kOpDecorate && !decorated && ins.w.size() == 3 && ins.w[0] == heap &&
        ins.w[1] == kDecorationDescriptorSet) {
      code.push_back(std::move(ins));
      code.push_back({kOpDecorate, {combined, kDecorationDescriptorSet, combined_set}});
      code.push_back({kOpDecorate, {combined, kDecorationBinding, combined_binding}});
      decorations_at = code.size();
      decorated = true;
      continue;
    }
    if (ins.op == kOpVariable && ins.w.size() >= 2 && ins.w[1] == heap) {
      code.push_back(std::move(ins));
      code.insert(code.end(), types.begin(), types.end());
      continue;
    }
    if ((ins.op == kOpAccessChain || ins.op == kOpInBoundsAccessChain) && ins.w.size() == 4 && ins.w[2] == heap) {
      ins.w[0] = pointer_sampled;
      ins.w[2] = combined;
      ++stats.chains;
      code.push_back(std::move(ins));
      continue;
    }
    if (ins.op == kOpLoad && ins.w.size() >= 3 && load_chain.count(ins.w[1])) {
      const uint32_t original = ins.w[1], loaded = m.NewId();
      ins.w[0] = sampled_type;
      ins.w[1] = loaded;
      code.push_back(std::move(ins));
      code.push_back({kOpImage, {image_type, original, loaded}});
      load_combined[original] = loaded;
      if (nonuniform.count(original)) nonuniform_loads.push_back(loaded);
      continue;
    }
    if (ins.op == kOpSampledImage && ins.w.size() == 4 && load_chain.count(ins.w[2]) && ins.w[0] == sampled_type) {
      const uint32_t word = chain_word[load_chain[ins.w[2]]];
      if (sampler_word(ins.w[3]) == int32_t(48 + word)) {
        // The combined load is emitted at the image load, which dominates this OpSampledImage.
        code.push_back({kOpCopyObject, {ins.w[0], ins.w[1], ins.w[2]}});  // operand resolved after the walk
        ++stats.combined;
        continue;
      }
      ++stats.separate;
    }
    code.push_back(std::move(ins));
  }
  if (!decorated) {
    reason = "the 2D heap has no DescriptorSet decoration";
    return false;
  }
  // The copies take the combined load of their image load (which dominates them).
  for (Instruction& ins : code)
    if (ins.op == kOpCopyObject && ins.w.size() == 3 && ins.w[0] == sampled_type && load_combined.count(ins.w[2]))
      ins.w[2] = load_combined[ins.w[2]];
  std::vector<Instruction> decorations;
  for (const uint32_t id : nonuniform_loads) decorations.push_back({kOpDecorate, {id, kDecorationNonUniform}});
  code.insert(code.begin() + decorations_at, decorations.begin(), decorations.end());
  m.code = std::move(code);
  m.Emit(output);
  return true;
}

// --- Analysis helpers ---------------------------------------------------------------------------------------------
// True if the module may read any shared block word in [first, last) (or the block as a whole / dynamically).
inline bool ReadsSharedWords(const std::vector<uint32_t>& input, uint32_t first, uint32_t last) {
  using namespace ps_desc_detail;
  Module m;
  std::string reason;
  if (!m.Parse(input, reason)) return true;
  bool twice = false;
  const uint32_t shared = m.Variable(4, 2, twice);
  if (twice) return true;
  if (!shared) return false;
  for (const Instruction& ins : m.code) {
    if (IsAnnotationOrDebug(ins.op) || ins.op == kOpVariable) continue;
    bool references = false;
    ForEachIdOperand(ins, [&](uint32_t x) { references |= x == shared; });
    if (!references) continue;
    const Words& w = ins.w;
    uint32_t member = 0, element = 0, component = 0;
    if ((ins.op == kOpAccessChain || ins.op == kOpInBoundsAccessChain) && w.size() >= 5 && w[2] == shared &&
        m.IntConstant(w[3], member) && member == 0 && m.IntConstant(w[4], element)) {
      if (w.size() == 6 && m.IntConstant(w[5], component)) {
        const uint32_t word = element * 4 + component;
        if (word >= first && word < last) return true;
        continue;
      }
      if (w.size() == 5) {
        if (element * 4 + 4 > first && element * 4 < last) return true;
        continue;
      }
    }
    return true;
  }
  return false;
}

}  // namespace me::native

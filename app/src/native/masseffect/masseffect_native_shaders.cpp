// masseffect - native renderer: shader identification (see masseffect_native_shaders.h).
//
// Shader container format: see Read(). All fields are big-endian.

#include "masseffect_native_shaders.h"

#include "masseffect_shader_library.h"
#include "../me_shader_identity.h"

#include <rex/logging.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>

/*
 * xxHash reads with memcpy (XXH_FORCE_MEMORY_ACCESS 0). With the method it picks for GCC (1) it reads through
 * 64- and 32-bit pointers without may_alias, and GCC may move that read ahead of the store of the data being
 * hashed (strict aliasing). With that, the texture key read keys[4] before writing it and the same texture was
 * created several times. Same fingerprint values; on AArch64, the same LDR.
 */
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h was already included with its implementation before this point: XXH_FORCE_MEMORY_ACCESS 0 would come too late"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace masseffect::native {
namespace {

// Bits of a vertex fetch instruction that D3D does not touch
// (VertexFetchInstruction in XenosRecomp shader_code.h): opcode, source and
// destination registers, destination swizzle and predicate. The rest (fetch
// constant, format, stride, offset, numeric modes and isMiniFetch come from the declaration. In
// particular D3D may turn a full fetch from the container into a mini fetch in the ring.
constexpr uint32_t kFetchKeeps[3] = {0x0007FFFF, 0x80000FFF, 0x80000000};

constexpr uint32_t kMaxWarnings = 48;

constexpr const char* kUses[] = {"position",   "weight",   "indices",    "normal",
                                 "point_size", "texcoord", "tangent",    "binormal",
                                 "tessellation", "position_t", "color",  "fog",
                                 "depth",      "sample",   "usage14",    "usage15"};

struct Container {
  const std::vector<uint8_t>& o;

  bool Has(size_t position, size_t bytes) const {
    return position <= o.size() && bytes <= o.size() - position;
  }
  uint32_t U32(size_t p) const {
    return uint32_t(o[p]) << 24 | uint32_t(o[p + 1]) << 16 | uint32_t(o[p + 2]) << 8 | o[p + 3];
  }
  uint16_t U16(size_t p) const { return uint16_t(uint32_t(o[p]) << 8 | o[p + 1]); }
};

// The full vertex fetches of a microcode (instruction index, destination register), walking the
// exec clauses of the control flow. Same walk as XenosRecomp vertexFetchesByRegister:
// Direct3D's own vertex shaders (no constant table) are linked by fetch destination
// register, one TEXCOORD<register> element per fetch, because Direct3D rewrites those fetches by register.
std::vector<std::pair<uint32_t, uint32_t>> FetchesOfVertices(const std::vector<uint32_t>& w) {
  std::vector<std::pair<uint32_t, uint32_t>> fetches;
  uint32_t end = uint32_t(w.size() / 3);
  for (uint32_t par = 0; par < end && par * 3 + 2 < w.size(); ++par) {
    const uint64_t cf[2] = {uint64_t(w[par * 3]) | (uint64_t(w[par * 3 + 1] & 0xFFFF) << 32),
                            uint64_t(w[par * 3 + 1] >> 16) | (uint64_t(w[par * 3 + 2]) << 16)};
    for (uint64_t c : cf) {
      const uint32_t opcode = uint32_t(c >> 44) & 0xF;
      if (!((opcode >= 1 && opcode <= 6) || opcode == 13 || opcode == 14)) continue;
      const uint32_t address = uint32_t(c) & 0xFFF, count = uint32_t(c >> 12) & 0x7;
      const uint32_t sequence = uint32_t(c >> 16) & 0xFFF;
      if (address != 0) end = std::min(end, address);
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t index = address + i;
        if (!((sequence >> (i * 2)) & 0x1) || size_t(index) * 3 + 2 >= w.size()) continue;
        const uint32_t d0 = w[index * 3];
        if ((d0 & 0x1F) != 0) continue;
        fetches.emplace_back(index, (d0 >> 12) & 0x3F);
      }
    }
  }
  return fetches;
}

// Texture fetch constant registers referenced by a table-less pixel shader. Direct3D's generated
// Scaleform shaders have an empty CTAB even though their microcode samples s0; without this scan the
// native renderer leaves descriptor index 0 in the shared block and every glyph samples the transparent
// fallback texture. The control-flow walk is the same one used above for vertex fetches.
std::vector<uint32_t> FetchesOfTextures(const std::vector<uint32_t>& w) {
  std::vector<uint32_t> registers;
  uint32_t end = uint32_t(w.size() / 3);
  for (uint32_t par = 0; par < end && par * 3 + 2 < w.size(); ++par) {
    const uint64_t cf[2] = {uint64_t(w[par * 3]) | (uint64_t(w[par * 3 + 1] & 0xFFFF) << 32),
                            uint64_t(w[par * 3 + 1] >> 16) | (uint64_t(w[par * 3 + 2]) << 16)};
    for (uint64_t c : cf) {
      const uint32_t opcode_cf = uint32_t(c >> 44) & 0xF;
      if (!((opcode_cf >= 1 && opcode_cf <= 6) || opcode_cf == 13 || opcode_cf == 14)) continue;
      const uint32_t address = uint32_t(c) & 0xFFF, count = uint32_t(c >> 12) & 0x7;
      const uint32_t sequence = uint32_t(c >> 16) & 0xFFF;
      if (address != 0) end = std::min(end, address);
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t index = address + i;
        if (!((sequence >> (i * 2)) & 0x1) || size_t(index) * 3 + 2 >= w.size()) continue;
        const uint32_t d0 = w[index * 3];
        const uint32_t opcode_fetch = d0 & 0x1F;
        // TextureFetch and GetTextureWeights. Both need the texture and sampler descriptors.
        if (opcode_fetch != 1 && opcode_fetch != 19) continue;
        const uint32_t register_value = (d0 >> 20) & 0x1F;
        if (std::find(registers.begin(), registers.end(), register_value) == registers.end()) {
          registers.push_back(register_value);
        }
      }
    }
  }
  return registers;
}

// Reads what is needed from a shader container. Two layouts are understood: the 0x102A11xx one of
// XenosRecomp shader.h (Mass Effect), and an older 0x102A0Exx one (24-byte header with signature, virtual
// part, physical part, definitions (+12), CTAB (+16) and shader header (+20); the microcode is the whole
// physical part), which the game does not use. Returns the reason if it cannot be read.
const char* Read(const masseffect::native::Shader& shader, ShaderEntry& e) {
  const Container c{shader.original};
  if (!c.Has(0, 24)) return "container too short";
  const uint32_t virtual_value = c.U32(4);
  const uint32_t table = c.U32(16);
  // D3D's built-in shaders carry a syntactically valid CTAB at 0x24, but its constant count is
  // zero. Semantically this is the same as no table: inputs, outputs, constants and texture fetches
  // are linked by their hardware register numbers. Checking only a null offset misclassified every
  // such Mass Effect shader as a normal package shader.
  const bool empty_table = !table || (c.Has(size_t(table) + 4, 16) && c.U32(size_t(table) + 16) == 0);
  // The 0x102A11xx layout (XenosRecomp shader.h ShaderContainer) keeps the shader
  // header at +0x18 and the microcode at virtualSize + shader.physicalOffset, shader.size bytes; the
  // physical part can hold more than the microcode.
  const bool is2008 = (c.U32(0) & 0xFFFFFF00u) == 0x102A1100u;
  const uint32_t header = is2008 ? c.U32(24) : c.U32(20);
  if (is2008 && !c.Has(header, 8)) return "shader header outside the container";
  const uint32_t microcode_start = is2008 ? virtual_value + c.U32(header) : virtual_value;
  const uint32_t physical = is2008 ? c.U32(header + 4) : c.U32(8);
  if (!physical || (physical % 4) || !c.Has(microcode_start, physical)) {
    return "microcode outside the container";
  }
  if (header >= virtual_value || size_t(header) + (shader.vertices ? 40 : 32) > virtual_value) {
    return "shader header outside the virtual part";
  }

  e.vertices = shader.vertices;
  e.microcode.resize(physical / 4);
  for (size_t i = 0; i < e.microcode.size(); ++i) {
    e.microcode[i] = c.U32(size_t(microcode_start) + i * 4);
  }

  if (e.vertices) {
    // List at +0x28, after skipping the words at +0x18; +0x1C = element count.
    const uint32_t previous = c.U32(header + 24);
    const uint32_t count = c.U32(header + 28);
    const size_t beginning = size_t(header) + (is2008 ? 36 : 40) + size_t(previous) * 4;
    if (count > 64 || previous > 1024 || beginning + size_t(count) * 4 > virtual_value) {
      return "vertex elements outside the virtual part";
    }
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t value = c.U32(beginning + size_t(i) * 4);
      ElementVertex element;
      element.instruction = uint16_t(value & 0xFFF);
      element.usage = uint8_t((value >> 12) & 0xF);
      element.usage_index = uint8_t((value >> 16) & 0xF);
      if ((size_t(element.instruction) + 1) * 3 > e.microcode.size()) {
        return "vertex element points outside the microcode";
      }
      e.elements.push_back(element);
    }
    if (empty_table) {
      e.binding_per_register = true;
      // Linked by fetch destination register (FetchesOfVertices).
      e.elements.clear();
      for (const auto& [index, register_value] : FetchesOfVertices(e.microcode)) {
        if (register_value >= 8) return "vertex fetch into a register with no binding slot (>= r8)";
        ElementVertex element;
        element.instruction = uint16_t(index);
        element.usage = 5;  // texcoord
        element.usage_index = uint8_t(register_value);
        e.elements.push_back(element);
      }
    }
    // Normalize only AFTER selecting the authoritative fetch set. Table-less
    // D3D shaders may carry stale header DECL entries pointing at ALU triples;
    // masking those first would irreversibly corrupt strict ALU/CF identity.
    for (const auto& element : e.elements) {
      for (size_t j = 0; j < 3; ++j)
        e.microcode[size_t(element.instruction) * 3 + j] &= kFetchKeeps[j];
    }
  } else {
    if (!c.Has(header + 24, 8)) return "pixel shader header too short";
    e.outputs = c.U32(header + 28);
  }

  // Constant table: only the samplers (register and type).
  // Direct3D's own shaders (clears, copies) have none: no named samplers, and the whole
  // float buffer, since XenosRecomp reads their registers by number.
  if (empty_table) {
    e.binding_per_register = true;
    e.constants_bytes = 256 * 16;
    if (!e.vertices) {
      for (uint32_t register_value : FetchesOfTextures(e.microcode)) {
        SamplerShader sampler;
        sampler.register_value = uint16_t(register_value);
        e.samplers.push_back(sampler);
      }
    }
    e.fingerprint = XXH3_64bits(e.microcode.data(), e.microcode.size() * sizeof(uint32_t));
    return nullptr;
  }
  if (!c.Has(table + 4, 28)) return "no constant table";
  const size_t base = size_t(table) + 4;
  const uint32_t constants = c.U32(base + 12);
  const uint32_t info = c.U32(base + 16);
  if (constants > 1024 || !c.Has(base + info, size_t(constants) * 20)) {
    return "constant table outside the container";
  }
  // Float registers the SPIR-V reads: the highest in the table, or up to the end of the
  // buffer if there is an array with relative indexing (XenosRecomp shader_recompiler.cpp:
  // 1172-1183: tailCount = 256 in VS and 224 in PS).
  uint32_t registers_float = 0;
  for (uint32_t i = 0; i < constants; ++i) {
    const size_t p = base + info + size_t(i) * 20;
    if (c.U16(p + 4) == 2) {  // RegisterSet::Float4
      const uint32_t index = c.U16(p + 6);
      const uint32_t how_many = c.U16(p + 8);
      registers_float = std::max(registers_float, how_many > 1 ? (e.vertices ? 256u : 224u)
                                                              : index + 1);
    }
  }
  e.constants_bytes = std::min<uint32_t>(std::max<uint32_t>(registers_float, 1), 256) * 16;
  for (uint32_t i = 0; i < constants; ++i) {
    const size_t p = base + info + size_t(i) * 20;
    if (c.U16(p + 4) != 3) continue;  // RegisterSet::Sampler
    SamplerShader sampler;
    sampler.register_value = c.U16(p + 6);
    const uint32_t type = c.U32(p + 12);
    sampler.type = c.Has(base + type, 4) ? c.U16(base + type + 2) : 0;
    e.samplers.push_back(sampler);
  }

  // Mass Effect has shaders whose CTAB omits anonymous hardware samplers. The movie YUV shader is
  // one: tex0-tex2 are named in the table, while s3 is still sampled by the microcode and supplies
  // the movie alpha. XenosRecomp emits the s3 descriptor access, so leaving it out here binds slot 0
  // accidentally. Treat the microcode as authoritative and add every referenced fetch register that
  // the table did not name. This is also safe for ordinary shaders, where the two sets are identical.
  if (!e.vertices) {
    for (uint32_t register_value : FetchesOfTextures(e.microcode)) {
      const auto existing = std::find_if(e.samplers.begin(), e.samplers.end(), [register_value](const auto& s) {
        return s.register_value == register_value;
      });
      if (existing == e.samplers.end()) {
        SamplerShader sampler;
        sampler.register_value = uint16_t(register_value);
        e.samplers.push_back(sampler);
      }
    }
  }

  e.fingerprint = XXH3_64bits(e.microcode.data(), e.microcode.size() * sizeof(uint32_t));
  return nullptr;
}

struct RawKey {
  uint64_t fingerprint;
  uint32_t words;
  bool vertices;
  bool operator==(const RawKey&) const = default;
};

struct HashRawKey {
  size_t operator()(const RawKey& c) const {
    return size_t(c.fingerprint ^ (uint64_t(c.words) << 1) ^ uint64_t(c.vertices));
  }
};

}  // namespace

struct ShadersNative::Data {
  masseffect::native::LibraryShaders library;
  std::vector<ShaderEntry> inputs;
  std::unordered_map<const masseffect::native::Shader*, uint32_t> per_shader;
  std::unordered_map<uint64_t, uint32_t> per_fingerprint;  // container XXH3 -> entries (PerFingerprint)
  // (vertices, words) -> candidate entries.
  std::map<std::pair<bool, uint32_t>, std::vector<uint32_t>> candidates;
  struct Cached {
    const ShaderEntry* entry = nullptr;
    bool patched = false;  // found by the second-stage lookup
  };
  std::unordered_map<RawKey, Cached, HashRawKey> cache;
  std::vector<uint32_t> temporal;
  StatsShaders stats;
  uint32_t warnings = 0;
  uint32_t patched_logged = 0;
  bool loaded = false;
  bool patched_lookup = true;
};

ShadersNative::ShadersNative() : data_(std::make_unique<Data>()) {}
ShadersNative::~ShadersNative() = default;

bool ShadersNative::loaded() const {
  return data_->loaded;
}

bool FetchCoherent(const ShaderEntry& vs, std::span<const uint32_t> patched) {
  if (!vs.vertices || patched.size() != vs.microcode.size()) {
    return false;
  }
  for (const ElementVertex& element : vs.elements) {
    const size_t p = size_t(element.instruction) * 3;
    const uint32_t register_value = (vs.microcode[p] >> 12) & 0x3F;
    bool found = false;
    for (const ElementVertex& other : vs.elements) {
      const uint32_t d0 = patched[size_t(other.instruction) * 3];
      if (((d0 >> 12) & 0x3F) == register_value && (d0 & 0x1F) == 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

bool ShadersNative::Load(const std::filesystem::path& file, bool use_index) {
  Data& d = *data_;
  const auto start = std::chrono::steady_clock::now();
  // With <package>.idx only the index is read (entry table and original containers) and
  // each SPIR-V is read from the package on first use. Without it, or if it does not match the package,
  // the whole package is loaded as before.
  bool is_indexed = false;
  if (use_index) {
    std::filesystem::path index = file;
    index += ".idx";
    std::error_code ec;
    if (std::filesystem::exists(index, ec)) {
      try {
        d.library.LoadIndexed(file, index);
        is_indexed = true;
      } catch (const std::exception& e) {
        REXLOG_WARN("[native] shaders: index {} not used ({}); loading the whole package", index.string(),
                    e.what());
      }
    } else {
      REXLOG_INFO("[native] shaders: no index {}: loading the whole package", index.string());
    }
  }
  if (!is_indexed) {
    try {
      d.library.Load(file);
    } catch (const std::exception& e) {
      REXLOG_WARN("[native] shaders: shader library not available ({}): {}", file.string(),
                  e.what());
      return false;
    }
  }
  const auto& shaders = d.library.shaders();
  uint32_t with_kill = 0, no_kill = 0;
  d.inputs.clear();
  d.inputs.reserve(shaders.size());
  uint32_t vertex = 0, pixel = 0;
  for (uint32_t i = 0; i < shaders.size(); ++i) {
    ShaderEntry e;
    e.shader = &shaders[i];
    e.number = i;
    if (const char* reason = Read(shaders[i], e)) {
      REXLOG_WARN("[native] shaders: library container {} ignored: {}", i, reason);
      continue;
    }
    if (!e.vertices) {
      e.kills = shaders[i].indexed() ? shaders[i].kills : masseffect::native::CountKillsSpirv(shaders[i].spirv);
      e.discards = e.kills != 1;                // 1 = only the alpha test one
      (e.discards ? with_kill : no_kill) += 1;
    }
    (e.vertices ? vertex : pixel) += 1;
    d.inputs.push_back(std::move(e));
  }
  d.candidates.clear();
  d.per_shader.clear();
  d.per_fingerprint.clear();
  std::map<std::tuple<bool, uint32_t, uint64_t>, std::vector<uint32_t>> groups;
  for (uint32_t i = 0; i < d.inputs.size(); ++i) {
    const ShaderEntry& e = d.inputs[i];
    const uint32_t words = uint32_t(e.microcode.size());
    d.candidates[{e.vertices, words}].push_back(i);
    d.per_shader[e.shader] = i;
    d.per_fingerprint.emplace(e.shader->fingerprint, i);  // the first in library order, as the linear search did
    groups[{e.vertices, words, e.fingerprint}].push_back(i);
  }
  uint32_t repeated = 0, repeated_distinct = 0;
  for (const auto& [key, members] : groups) {
    if (members.size() < 2) continue;
    ++repeated;
    // Indexed: the SPIR-V is compared by its length and XXH3 (stored in the index) instead of read.
    const masseffect::native::Shader& first = *d.inputs[members[0]].shader;
    for (uint32_t m : members) {
      const masseffect::native::Shader& other = *d.inputs[m].shader;
      if (first.indexed() ? (other.words != first.words || other.spirv_fingerprint != first.spirv_fingerprint)
                             : other.spirv != first.spirv) {
        ++repeated_distinct;
        break;
      }
    }
  }
  d.cache.clear();
  d.loaded = !d.inputs.empty();
  REXLOG_INFO("[native] shaders: library with {} shaders ({} vertex, {} pixel); {} groups sharing the "
              "same microcode, {} of them with different SPIR-V",
              d.inputs.size(), vertex, pixel, repeated, repeated_distinct);
  REXLOG_INFO("[native] shaders: library loaded in {} ms ({}, {:.1f} MB of SPIR-V resident)",
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count(),
              is_indexed ? "index only; SPIR-V on demand" : "whole package",
              is_indexed ? 0.0 : [&] {
                size_t bytes = 0;
                for (const auto& s : shaders) bytes += s.spirv.size() * 4;
                return double(bytes) / (1024.0 * 1024.0);
              }());
  REXLOG_INFO("[native] shaders: pixel shaders that can discard pixels {} of {} (the rest only carry the "
              "alpha-test kill: no color to write, so their stage can be dropped)",
              with_kill, with_kill + no_kill);
  return d.loaded;
}

void ShadersNative::SetPatchedLookup(bool on) {
  data_->patched_lookup = on;
}

const ShaderEntry* ShadersNative::Identify(bool vertices,
                                                 std::span<const uint32_t> microcode, bool* patched) {
  Data& d = *data_;
  ++d.stats.loads;
  if (patched) *patched = false;
  const RawKey key{XXH3_64bits(microcode.data(), microcode.size_bytes()),
                         uint32_t(microcode.size()), vertices};
  if (auto it = d.cache.find(key); it != d.cache.end()) {
    if (patched) *patched = it->second.patched;
    return it->second.entry;
  }
  ++d.stats.distinct;

  const ShaderEntry* chosen = nullptr;
  uint32_t matches = 0;
  if (auto c = d.candidates.find({vertices, uint32_t(microcode.size())});
      c != d.candidates.end()) {
    for (uint32_t index : c->second) {
      const ShaderEntry& e = d.inputs[index];
      d.temporal.assign(microcode.begin(), microcode.end());
      for (const ElementVertex& element : e.elements) {
        for (size_t j = 0; j < 3; ++j) {
          d.temporal[size_t(element.instruction) * 3 + j] &= kFetchKeeps[j];
        }
      }
      if (XXH3_64bits(d.temporal.data(), d.temporal.size() * sizeof(uint32_t)) != e.fingerprint ||
          d.temporal != e.microcode) {
        continue;
      }
      if (!chosen) {
        chosen = &e;
      }
      ++matches;
    }
  }

  // Second stage (SetPatchedLookup): the fetch swizzles D3D patched per vertex declaration, and the same-register
  // fetch reorder. Only after the exact stage found nothing, so library variants stored with their patched
  // swizzles (repair_vertex_variant_declarations) keep winning.
  bool found_patched = false;
  if (!chosen && vertices && d.patched_lookup) {
    if (auto c = d.candidates.find({vertices, uint32_t(microcode.size())}); c != d.candidates.end()) {
      const auto instruction = [](const ElementVertex& element) { return uint32_t(element.instruction); };
      bool permuted = false;
      std::string others;
      for (uint32_t index : c->second) {
        const ShaderEntry& e = d.inputs[index];
        const me::native::ShaderIdentityView loaded{me::native::ShaderIdentityStage::Vertex, microcode};
        const me::native::ShaderIdentityView selected{me::native::ShaderIdentityStage::Vertex, e.microcode};
        bool by_permutation = false;
        if (!me::native::VertexShaderIdentityMatches(loaded, selected, e.elements, instruction, 0)) {
          if (!me::native::g_vs_identity_fetch_permutation.load(std::memory_order_relaxed) ||
              !me::native::VertexShaderFetchPermutationMatches(loaded, selected, e.elements, instruction)) {
            continue;
          }
          by_permutation = true;
        }
        if (!chosen) {
          chosen = &e;
          permuted = by_permutation;
        } else if (others.size() < 64) {
          others += fmt::format(" n{}", e.number);
        }
        ++matches;
      }
      if (chosen) {
        found_patched = true;
        ++d.stats.patched;
        if (permuted) ++d.stats.patched_permuted;
        if (matches > 1) ++d.stats.patched_ambiguous;
        if (d.patched_logged < kMaxWarnings) {
          ++d.patched_logged;
          std::string swizzles;
          for (const ElementVertex& element : chosen->elements) {
            const size_t p = size_t(element.instruction) * 3 + 1;
            if ((microcode[p] & 0xFFF) != (chosen->microcode[p] & 0xFFF)) {
              swizzles += fmt::format(" {}{}@{}:{:03X}->{:03X}", kUses[element.usage & 0xF], element.usage_index,
                                      element.instruction, chosen->microcode[p] & 0xFFF, microcode[p] & 0xFFF);
            }
          }
          REXLOG_INFO("[native] shaders: VS {:016X} ({} words) identified by the patched-fetch lookup as n{} "
                      "(container {:016X}){}; swizzles library->loaded:{}{}",
                      key.fingerprint, microcode.size(), chosen->number,
                      chosen->shader ? chosen->shader->fingerprint : 0,
                      permuted ? ", same-register fetches reordered" : "", swizzles,
                      matches > 1 ? fmt::format("; {} candidates, first in library order kept, others:{}", matches,
                                                others)
                                  : std::string());
        }
      }
    }
  }
  if (patched) *patched = found_patched;

  // Vertex shaders arrive patched: a mismatch is normal.
  const bool warn = d.warnings < kMaxWarnings && !vertices;
  if (chosen) {
    ++d.stats.identified;
    if (matches > 1) {
      ++d.stats.ambiguous;
    }
    if (warn && vertices) {
      // What D3D has patched in each fetch: it becomes the vertex input.
      ++d.warnings;
      std::string detail;
      for (const ElementVertex& element : chosen->elements) {
        const size_t p = size_t(element.instruction) * 3;
        const uint32_t d0 = microcode[p], d1 = microcode[p + 1], d2 = microcode[p + 2];
        const uint32_t fetch = ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 0x3);
        const int32_t offset = int32_t(d2 << 1) >> 9;
        detail += fmt::format(" {}{}:f{}/fmt{}/z{}/o{}{}", kUses[element.usage & 0xF],
                               element.usage_index, fetch, (d1 >> 16) & 0x3F, d2 & 0xFF, offset,
                               ((d1 >> 30) & 0x1) ? "/mini" : "");
      }
      REXLOG_INFO("[native] shaders: VS n{} ({} words, {} matches):{}", chosen->number,
                  microcode.size(), matches, detail);
    }
  } else {
    ++d.stats.no_identify;
    if (warn) {
      ++d.warnings;
      const auto c = d.candidates.find({vertices, uint32_t(microcode.size())});
      REXLOG_WARN("[native] shaders: unidentified {} shader: {} words, fingerprint {:016X} "
                  "({} containers of that type and length)",
                  vertices ? "vertex" : "pixel", microcode.size(), key.fingerprint,
                  c != d.candidates.end() ? c->second.size() : 0);
      // Diagnostics: which words change against the containers of the same
      // length (incoming / container original, without the mask).
      for (size_t k = 0; c != d.candidates.end() && k < c->second.size() && k < 2; ++k) {
        const ShaderEntry& e = d.inputs[c->second[k]];
        const auto& o = e.shader->original;
        const size_t virtual_value = size_t(o[4]) << 24 | size_t(o[5]) << 16 | size_t(o[6]) << 8 | o[7];
        std::string differences;
        uint32_t how_many = 0;
        for (size_t i = 0; i < microcode.size(); ++i) {
          uint32_t incoming = microcode[i];
          for (const ElementVertex& element : e.elements) {
            const size_t p = size_t(element.instruction) * 3;
            if (i >= p && i < p + 3) {
              incoming &= kFetchKeeps[i - p];
            }
          }
          if (incoming == e.microcode[i]) {
            continue;
          }
          if (++how_many <= 24) {
            const size_t b = virtual_value + i * 4;
            const uint32_t original = uint32_t(o[b]) << 24 | uint32_t(o[b + 1]) << 16 |
                                      uint32_t(o[b + 2]) << 8 | o[b + 3];
            differences += fmt::format(" {}:{:08X}/{:08X}", i, microcode[i], original);
          }
        }
        std::string fetch;
        for (const ElementVertex& element : e.elements) {
          fetch += fmt::format(" {}", element.instruction);
        }
        REXLOG_WARN("[native] shaders:   versus n{}: {} words differ; fetch at "
                    "instructions{}:{}",
                    e.number, how_many, fetch, differences);
      }
    }
  }
  d.cache.emplace(key, Data::Cached{chosen, found_patched});
  return chosen;
}

const ShaderEntry* ShadersNative::IdentifyContainer(
    std::span<const uint8_t> container) const {
  const Data& d = *data_;
  if (!d.loaded) {
    return nullptr;
  }
  const masseffect::native::Shader* shader = d.library.Search(container);
  if (!shader) {
    return nullptr;
  }
  const auto it = d.per_shader.find(shader);
  return it != d.per_shader.end() ? &d.inputs[it->second] : nullptr;
}

// Entries are in library order (Load), with gaps where a container was skipped.
const ShaderEntry* ShadersNative::PerNumber(uint32_t number) const {
  const Data& d = *data_;
  if (!d.loaded) {
    return nullptr;
  }
  const auto it = std::lower_bound(d.inputs.begin(), d.inputs.end(), number,
                                   [](const ShaderEntry& e, uint32_t n) { return e.number < n; });
  return it != d.inputs.end() && it->number == number ? &*it : nullptr;
}

const ShaderEntry* ShadersNative::PerFingerprint(uint64_t fingerprint) const {
  const Data& d = *data_;
  if (!d.loaded || !fingerprint) {
    return nullptr;
  }
  const auto it = d.per_fingerprint.find(fingerprint);
  return it != d.per_fingerprint.end() ? &d.inputs[it->second] : nullptr;
}

bool ShadersNative::is_indexed() const {
  return data_->loaded && data_->library.is_indexed();
}

masseffect::native::StatsPreload ShadersNative::PreloadSpirv(std::span<const uint64_t> fingerprints,
                                                                   const std::atomic<bool>* stop) const {
  const Data& d = *data_;
  if (!d.loaded || !d.library.is_indexed()) {
    return {};
  }
  std::vector<const masseffect::native::Shader*> shaders;
  shaders.reserve(fingerprints.size());
  for (uint64_t fingerprint : fingerprints) {
    if (const ShaderEntry* e = PerFingerprint(fingerprint)) {
      shaders.push_back(e->shader);
    }
  }
  return d.library.Preload(std::move(shaders), stop);
}

const char* UsageName(uint8_t usage) {
  return kUses[usage & 0xF];
}

StatsShaders ShadersNative::Stats() const {
  return data_->stats;
}

}  // namespace masseffect::native

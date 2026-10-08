#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace me::native {

/*
 * Xenos k_16_16 render targets (masseffect_native_velocity_16_16, docs/image-defects-feros.md section 3).
 *
 * What the Xenos does (sources: rexglue-sdk include/rex/graphics/xenos.h "Fixed point -32...32";
 * xenia-canary src/xenia/gpu/shaders/pixel_formats.xesli XeUnpackR16G16Edram / XePackR16G16Edram / XePackFixed,
 * resolve.xesli XeResolveLoad*, draw_util.cc GetCopyShader; render_target_cache.cc clamps k_16_16 to -32...32):
 *
 *   EDRAM word of a k_16_16 pixel: two signed 16-bit fixed-point numbers, value = max(int16 * 32 / 32767, -32).
 *     A pixel shader output v is stored as int16(round(clamp(v, -32, 32) * 32767 / 32)). A colour clear stores
 *     the RB_COLOR_CLEAR word as is; the k_8_8_8_8 <-> k_16_16 EDRAM aliasing moves the word as is.
 *   Resolve to a k_16_16 texture is NOT a raw copy (IsColorResolveFormatBitwiseEquivalent is false for k_16_16):
 *     the word is unpacked to the value above, multiplied by 2^RB_COPY_DEST_INFO.copy_dest_exp_bias and packed
 *     into the destination format according to copy_dest_number (0 = unsigned fraction: round(saturate(v) *
 *     65535); 1 = signed fraction: round(clamp(v, -1, 1) * 32767); 2/3 = integers).
 *   Texture fetch of format 25 (k_16_16) with signs 0, exp_adjust 0, num_format 0: unsigned normalized,
 *     bits / 65535, i.e. what a host R16G16_UNORM view returns.
 *
 * Host storage with this mode: the k_16_16 render target stays a VK_FORMAT_R16G16_UNORM image, but it now holds
 * the Xenos EDRAM word itself (texel = word / 65535). Clears (ClearValueColor), the EDRAM aliasing conversions and
 * the raw copies already treat the image as that word, so only two places change: the pixel shader output gets
 * the encode epilogue below (TransformFixed16Encode), and a resolve from such an image decodes the word
 * (me_resolve_fixed16_frag.frag, Fixed16ResolveModel below).
 *
 * The epilogue, per encoded channel c of a vec4 output x (z and w are stored unchanged: an R16G16 attachment
 * ignores them):
 *   v    = IsNan(x.c) ? 0 : FClamp(x.c, -32, 32)
 *   n    = RoundEven(v * (32767 / 32))                      (an integer in [-32767, 32767], as a float)
 *   word = n < 0 ? n + 65536 : n                            (the 16-bit pattern, exact in float)
 *   out  = (word + 0.25) * (1 / 65535)                      (UNORM16 conversion gives `word` back whether the
 *                                                            attachment rounds to nearest or toward zero)
 * Exact for blending ONE/ZERO (the velocity pass); any other blend would blend encoded words.
 */

inline uint32_t Fixed16BitsOfFloat(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline float Fixed16FloatOfBits(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

// The 16-bit EDRAM pattern of a pixel shader output component (the Xenos ROP).
inline uint16_t Fixed16EncodeModel(float x) {
  float v = std::isnan(x) ? 0.0f : (x < -32.0f ? -32.0f : (x > 32.0f ? 32.0f : x));
  const float n = std::nearbyint(v * (32767.0f / 32.0f));  // FE_TONEAREST = RoundEven
  return uint16_t(int32_t(n) & 0xFFFF);
}

// The float the epilogue writes to the UNORM16 attachment for that pattern (operation for operation).
inline float Fixed16EpilogueOutputModel(float x) {
  float v = std::isnan(x) ? 0.0f : (x < -32.0f ? -32.0f : (x > 32.0f ? 32.0f : x));
  float n = std::nearbyint(v * (32767.0f / 32.0f));
  float word = n < 0.0f ? n + 65536.0f : n;
  return (word + 0.25f) * (1.0f / 65535.0f);
}

// A UNORM16 attachment store of `f`: round to nearest (rtz = toward zero), saturated.
inline uint16_t Fixed16UnormStoreModel(float f, bool rtz = false) {
  if (!(f > 0.0f)) return 0;
  if (f >= 1.0f) return 0xFFFF;
  const float scaled = f * 65535.0f;
  return uint16_t(rtz ? std::floor(scaled) : std::nearbyint(scaled));
}

// The value a k_16_16 EDRAM pattern stands for (XeUnpackR16G16Edram).
inline float Fixed16DecodeModel(uint16_t word) {
  const float v = float(int16_t(word)) * (32.0f / 32767.0f);
  return v < -32.0f ? -32.0f : v;
}

// The texel a resolve writes into the destination (one 16-bit channel, XePackFixed with 16 bits).
inline uint16_t Fixed16ResolveModel(uint16_t word, int32_t exp_bias, uint32_t number) {
  const float v = Fixed16DecodeModel(word) * std::ldexp(1.0f, exp_bias);
  switch (number) {
    case 1: {  // signed repeating fraction
      const float c = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
      return uint16_t(int32_t(c * 32767.0f + (v >= 0.0f ? 0.5f : -0.5f)) & 0xFFFF);
    }
    case 2:  // unsigned integer
      return uint16_t(uint32_t((v < 0.0f ? 0.0f : (v > 65535.0f ? 65535.0f : v)) + 0.5f));
    case 3: {  // signed integer
      const float c = v < -32768.0f ? -32768.0f : (v > 32767.0f ? 32767.0f : v);
      return uint16_t(int32_t(c + (v >= 0.0f ? 0.5f : -0.5f)) & 0xFFFF);
    }
    default:  // unsigned repeating fraction (and anything unexpected)
      return uint16_t(uint32_t((v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v)) * 65535.0f + 0.5f));
  }
}

struct Fixed16Stats {
  uint32_t stores = 0;  // OpStores rewritten
};

/*
 * Rewrites every OpStore to the fragment output variable at each Location in `locations` (bit L) so that it
 * stores the k_16_16 encode of x and y. Same shape requirements as TransformRestore7e3 (one Fragment entry point,
 * a GLSL.std.450 import, each selected output a direct Output float32 vec4 variable without Index/Component
 * decorations, used only as the pointer of whole-vector OpStores). `missing` returns the selected Locations the
 * module has no output for or never stores (nothing to encode there); they are not an error.
 */
inline bool TransformFixed16Encode(const std::vector<uint32_t>& input, uint32_t locations,
                                   std::vector<uint32_t>& output, Fixed16Stats& stats, uint32_t& missing,
                                   std::string& reason) {
  output.clear();
  reason.clear();
  stats = {};
  missing = 0;
  const auto fail = [&](const char* why) {
    output.clear();
    reason = why;
    return false;
  };
  if (&input == &output) return fail("input/output alias");
  if (!locations || locations > 0xFu) return fail("no or invalid locations");
  if (input.size() < 5 || input[0] != 0x07230203 || !input[3] || input[4]) return fail("invalid SPIR-V header");
  struct Ins { size_t at; uint32_t wc, op; };
  std::vector<Ins> instructions;
  uint32_t entries = 0, glsl = 0, float_type = 0, bool_type = 0;
  std::unordered_map<uint32_t, uint32_t> location_of;
  std::unordered_set<uint32_t> indexed;
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> vectors;
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> pointers;
  std::unordered_map<uint32_t, uint32_t> floats;
  std::unordered_map<uint32_t, uint32_t> output_variables;
  size_t first_function = input.size();
  for (size_t at = 5; at < input.size();) {
    const uint32_t wc = input[at] >> 16, op = input[at] & 0xFFFFu;
    if (!wc || wc > input.size() - at) return fail("malformed instruction");
    const uint32_t* w = input.data() + at;
    instructions.push_back({at, wc, op});
    switch (op) {
      case 11:  // OpExtInstImport
        if (wc >= 3 && std::strncmp(reinterpret_cast<const char*>(w + 2), "GLSL.std.450", (wc - 2) * 4) == 0)
          glsl = w[1];
        break;
      case 15:  // OpEntryPoint
        if (wc < 4 || w[1] != 4) return fail("requires a Fragment entry point only");
        ++entries;
        break;
      case 71:  // OpDecorate
        if (wc == 4 && w[2] == 30) location_of[w[1]] = w[3];
        if (wc >= 3 && (w[2] == 32 || w[2] == 31)) indexed.insert(w[1]);  // Index, Component
        break;
      case 74: case 75: return fail("group decorations unsupported");
      case 20: if (wc == 2 && !bool_type) bool_type = w[1]; break;
      case 22: if (wc == 3) floats[w[1]] = w[2]; break;
      case 23: if (wc == 4) vectors[w[1]] = {w[2], w[3]}; break;
      case 32: if (wc == 4) pointers[w[1]] = {w[2], w[3]}; break;
      case 59:  // OpVariable
        if (first_function == input.size() && wc >= 4 && w[3] == 3) output_variables[w[2]] = w[1];
        break;
      case 54: if (first_function == input.size()) first_function = at; break;
      default: break;
    }
    at += wc;
  }
  if (entries != 1) return fail("requires exactly one entry point");
  if (!glsl) return fail("no GLSL.std.450 import");
  if (first_function == input.size()) return fail("no functions");
  std::unordered_map<uint32_t, uint32_t> targets;  // variable id -> vec4 type
  uint32_t found = 0;
  for (const auto& [id, pointer_type] : output_variables) {
    const auto l = location_of.find(id);
    if (l == location_of.end() || l->second > 3 || !((locations >> l->second) & 1u)) continue;
    if (indexed.count(id)) return fail("output with Index/Component decoration");
    const auto p = pointers.find(pointer_type);
    if (p == pointers.end() || p->second.first != 3) return fail("output pointer type");
    const auto v = vectors.find(p->second.second);
    if (v == vectors.end() || v->second.second != 4) return fail("output is not a 4-component vector");
    const auto f = floats.find(v->second.first);
    if (f == floats.end() || f->second != 32) return fail("output is not float32");
    if (float_type && float_type != v->second.first) return fail("outputs of different float types");
    float_type = v->second.first;
    if (found & (1u << l->second)) return fail("two outputs at one Location");
    found |= 1u << l->second;
    targets[id] = p->second.second;
  }
  missing = locations & ~found;
  const auto pointer_operand = [](uint32_t op, uint32_t k) {
    switch (op) {
      case 61: return k == 3;                                 // OpLoad
      case 62: return k == 1 || k == 2;                       // OpStore
      case 63: case 64: return k == 1 || k == 2;              // OpCopyMemory(Sized)
      case 65: case 66: case 67: case 70: return k == 3;      // access chains
      case 57: return k >= 4;                                 // OpFunctionCall arguments
      case 83: return k == 3;                                 // OpCopyObject
      case 12: return k >= 5;                                 // OpExtInst operands
      case 245: return k >= 3 && (k & 1);                     // OpPhi values
      case 169: return k >= 4;                                // OpSelect values
      case 124: return k == 3;                                // OpBitcast
      default: return false;
    }
  };
  std::unordered_map<uint32_t, uint32_t> stores_of;  // variable id -> stores
  for (const auto& ins : instructions) {
    if (ins.at < first_function) continue;
    const uint32_t* w = input.data() + ins.at;
    for (uint32_t k = 1; k < ins.wc; ++k) {
      if (!targets.count(w[k]) || !pointer_operand(ins.op, k)) continue;
      if (ins.op != 62 || k != 1) return fail("output read, chained or passed (not a plain store)");
      ++stats.stores;
      ++stores_of[w[k]];
    }
  }
  for (const auto& [id, type] : targets)
    if (!stores_of.count(id)) missing |= 1u << location_of[id];
  if (!stats.stores) {  // nothing to encode: the module is returned unchanged
    output = input;
    return true;
  }
  uint32_t next = input[3];
  if (uint64_t(next) + uint64_t(stats.stores) * 32 + 32 >= 0x3FFFFFu) return fail("ID bound overflow");
  std::vector<uint32_t> declarations;
  const auto emit = [](std::vector<uint32_t>& dst, uint32_t op, std::initializer_list<uint32_t> args) {
    dst.push_back((uint32_t(args.size() + 1) << 16) | op);
    dst.insert(dst.end(), args);
  };
  if (!bool_type) { bool_type = next++; emit(declarations, 20, {bool_type}); }
  const auto constant = [&](uint32_t bits) {
    const uint32_t id = next++;
    emit(declarations, 43, {float_type, id, bits});
    return id;
  };
  const uint32_t f_zero = constant(0x00000000u);
  const uint32_t f_low = constant(0xC2000000u);      // -32
  const uint32_t f_high = constant(0x42000000u);     // 32
  const uint32_t f_scale = constant(0x447FFE00u);    // 32767 / 32 = 1023.96875 (exact)
  const uint32_t f_wrap = constant(0x47800000u);     // 65536
  const uint32_t f_quarter = constant(0x3E800000u);  // 0.25
  const uint32_t f_unorm = constant(Fixed16BitsOfFloat(1.0f / 65535.0f));
  output.assign(input.begin(), input.begin() + 5);
  for (const auto& ins : instructions) {
    const uint32_t* w = input.data() + ins.at;
    if (ins.at == first_function) output.insert(output.end(), declarations.begin(), declarations.end());
    if (ins.op == 62 && ins.wc >= 3 && targets.count(w[1])) {
      const uint32_t vector_type = targets[w[1]];
      const auto unary = [&](uint32_t op, uint32_t type, uint32_t a) {
        const uint32_t id = next++; emit(output, op, {type, id, a}); return id;
      };
      const auto binary = [&](uint32_t op, uint32_t type, uint32_t a, uint32_t b) {
        const uint32_t id = next++; emit(output, op, {type, id, a, b}); return id;
      };
      const auto select = [&](uint32_t condition, uint32_t a, uint32_t b) {
        const uint32_t id = next++; emit(output, 169, {float_type, id, condition, a, b}); return id;
      };
      const auto ext = [&](uint32_t instruction, std::initializer_list<uint32_t> operands) {
        const uint32_t id = next++;
        output.push_back((uint32_t(5 + operands.size()) << 16) | 12);  // OpExtInst
        output.insert(output.end(), {float_type, id, glsl, instruction});
        output.insert(output.end(), operands);
        return id;
      };
      uint32_t channels[4];
      for (uint32_t c = 0; c < 4; ++c) {
        const uint32_t component = next++;
        emit(output, 81, {float_type, component, w[2], c});  // OpCompositeExtract
        if (c >= 2) {
          channels[c] = component;
          continue;
        }
        const uint32_t is_nan = unary(156, bool_type, component);                       // OpIsNan
        const uint32_t clamped = ext(43, {component, f_low, f_high});                    // FClamp
        const uint32_t value = select(is_nan, f_zero, clamped);
        const uint32_t n = ext(2, {binary(133, float_type, value, f_scale)});            // RoundEven(v * k)
        const uint32_t negative = binary(184, bool_type, n, f_zero);                     // OpFOrdLessThan
        const uint32_t word = select(negative, binary(129, float_type, n, f_wrap), n);   // OpFAdd
        channels[c] = binary(133, float_type, binary(129, float_type, word, f_quarter), f_unorm);
      }
      const uint32_t result = next++;
      emit(output, 80, {vector_type, result, channels[0], channels[1], channels[2], channels[3]});
      const size_t at = output.size();
      output.insert(output.end(), w, w + ins.wc);
      output[at + 2] = result;
      continue;
    }
    output.insert(output.end(), w, w + ins.wc);
  }
  output[3] = next;
  return true;
}

}  // namespace me::native

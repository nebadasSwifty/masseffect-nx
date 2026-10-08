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
 * Restore into 7e3 (masseffect_native_restore_into_7e3, docs/vulkan-frame-time.md section 8).
 *
 * UE3's Xenon light path draws the resolved scene back into EDRAM through a k_2_10_10_10 (UNORM10, "f2") view
 * with a plain copy (VS6906/PS21415, blending off, mask F, full overwrite) and the additive light pass then
 * binds the k_2_10_10_10_FLOAT (7e3, "f3") view of the same tiles. Mode 4 keeps one host RGBA16F image per view,
 * so every light paid an f2 -> f3 conversion of the whole region. Both views are host RGBA16F, so the restore
 * draw can render straight into the f3 image if its fragment shader produces what the conversion would have
 * produced. Today's chain for one colour output x (float4) is:
 *
 *   1. the f2 attachment stores h = f16(x)                                   (RGBA16F colour attachment)
 *   2. the conversion (me_edram_color_to_color.frag / masseffect_edram_16f_to_16f.comp) reads h and packs
 *        code_c = uint(round(clamp(h.c, 0, 1) * 1023))   c = r, g, b
 *        code_a = uint(round(clamp(h.a, 0, 1) * 3))
 *   3. it unpacks the word as 7e3: From7e3(code_c), float(code_a) * (1.0 / 3.0), stored into the f3 RGBA16F image.
 *
 * TransformRestore7e3 rewrites every OpStore to the colour output variable at a selected Location into a store
 * of the same chain computed in the shader (the epilogue):
 *
 *   q    = OpQuantizeToF16(x.c)                (models step 1: RTNE, half denormals flushed - see below)
 *   code = OpConvertFToU(Round(FClamp(q, 0, 1) * 1023))     (the same GLSL.std.450 ops as the conversion)
 *   rgb  = code < 128 ? float(code) * 2^-9 : bitcast((code + (124 << 7)) << 16)   (= From7e3(code) exactly)
 *   a    = float(OpConvertFToU(Round(FClamp(q, 0, 1) * 3))) * float(1.0 / 3.0)
 *
 * Why it is bit-exact (tests/cpu/test_native_restore_7e3_spirv.cpp checks every statement on the CPU models
 * below, for all 1024 codes and a dense sweep of all float bit patterns):
 *   - From7e3 as written in the shaders and the closed form above agree for all 1024 codes.
 *   - Denormal halves flushed by OpQuantizeToF16 are below 2^-14: x * 1023 < 0.07 and x * 3 < 0.001, so both
 *     round to code 0 either way. Infinity / NaN clamp the same way on both sides.
 *   - For the values the restore draw really writes (a point-sampled UNORM10 texture, x = k / 1023), the code
 *     is the same whether the attachment rounds to nearest-even or toward zero, so the exactness does not depend
 *     on the ROP's float -> half rounding mode. For other values it assumes round-to-nearest-even.
 *   - The f3 attachment receives the identical float32 values the conversion pass writes, so its own float ->
 *     half rounding (only the alpha thirds are inexact) is the same on both paths.
 *   - Any later 32-bit colour consumer re-packs the word from the f3 image: Pack32_7e3(f16(epilogue(x))) equals
 *     Pack32_UNORM10(f16(x)) for every x, so f3 -> f0 / f2 / k_16_16 conversions see the same EDRAM word.
 *     The one difference left is a consumer that reads the f2 host image directly while it still owns the
 *     tiles (a blend into f2, a resolve from f2): today it sees f16(x), with this path code / 1023. They are
 *     equal for x = k / 1023. The targets side guards this with a learned consumer (see the targets code).
 */

// ---- CPU models (the test compares them; the SPIR-V below emits exactly these operations) --------------------

inline uint32_t BitsOfFloat(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline float FloatOfBits(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

// float -> half, round to nearest even, denormals kept (an RGBA16F attachment or image store).
inline uint16_t F32ToF16Rtne(float value) {
  const uint32_t f = BitsOfFloat(value);
  const uint32_t sign = (f >> 16) & 0x8000u;
  const uint32_t abs = f & 0x7FFFFFFFu;
  if (abs > 0x7F800000u) return uint16_t(sign | 0x7E00u);  // NaN
  if (abs >= 0x477FF000u) return uint16_t(sign | 0x7C00u);  // rounds to infinity (>= 65520) or is infinity
  if (abs < 0x38800000u) {  // half denormal or zero
    if (abs < 0x33000000u) return uint16_t(sign);  // below half of the smallest denormal (ties to 0 included)
    const uint32_t exponent = abs >> 23;
    const uint32_t mantissa = (abs & 0x7FFFFFu) | 0x800000u;
    // value = mantissa * 2^(exponent - 150); the half denormal unit is 2^-24, so units = mantissa >> (126 - exponent)
    const uint32_t shift = 126u - exponent;
    const uint32_t units = mantissa >> shift;
    const uint32_t rest = mantissa & ((1u << shift) - 1u);
    const uint32_t halfway = 1u << (shift - 1u);
    const uint32_t rounded = units + ((rest > halfway || (rest == halfway && (units & 1u))) ? 1u : 0u);
    return uint16_t(sign | rounded);
  }
  const uint32_t rebiased = abs - (112u << 23);  // exponent 127 -> 15
  const uint32_t units = rebiased >> 13;
  const uint32_t rest = rebiased & 0x1FFFu;
  const uint32_t rounded = units + ((rest > 0x1000u || (rest == 0x1000u && (units & 1u))) ? 1u : 0u);
  return uint16_t(sign | rounded);
}

// float -> half, toward zero (used only to show that the restore draw's real inputs do not depend on the mode).
inline uint16_t F32ToF16Rtz(float value) {
  const uint32_t f = BitsOfFloat(value);
  const uint32_t sign = (f >> 16) & 0x8000u;
  const uint32_t abs = f & 0x7FFFFFFFu;
  if (abs > 0x7F800000u) return uint16_t(sign | 0x7E00u);
  if (abs >= 0x47800000u) return uint16_t(sign | (abs == 0x7F800000u ? 0x7C00u : 0x7BFFu));
  if (abs < 0x38800000u) {
    if (abs < 0x33800000u) return uint16_t(sign);
    const uint32_t exponent = abs >> 23;
    const uint32_t mantissa = (abs & 0x7FFFFFu) | 0x800000u;
    return uint16_t(sign | (mantissa >> (126u - exponent)));
  }
  return uint16_t(sign | ((abs - (112u << 23)) >> 13));
}

inline float F16ToF32(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  const uint32_t exponent = (h >> 10) & 0x1Fu;
  const uint32_t mantissa = h & 0x3FFu;
  if (exponent == 0) return FloatOfBits(sign | BitsOfFloat(float(mantissa) * 0x1p-24f));
  if (exponent == 31) return FloatOfBits(sign | 0x7F800000u | (mantissa << 13));
  return FloatOfBits(sign | ((exponent + 112u) << 23) | (mantissa << 13));
}

// OpQuantizeToF16 as Mesa lowers it (fquantize2f16): |x| < 2^-14 -> signed zero, else f2f16_rtne then back.
inline float QuantizeToF16Model(float x) {
  if (std::fabs(x) < 0x1p-14f) return FloatOfBits(BitsOfFloat(x) & 0x80000000u);
  return F16ToF32(F32ToF16Rtne(x));
}

// GLSL.std.450 FClamp(x, 0, 1) on the GPU: fmin(fmax(x, 0), 1); NVIDIA FMNMX returns the non-NaN operand.
inline float Clamp01Model(float x) {
  if (std::isnan(x)) return 0.0f;
  return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

// GLSL.std.450 Round: NIR fround_even.
inline float RoundEvenModel(float x) { return std::nearbyint(x); }  // default FE_TONEAREST

// From7e3 as written in me_edram_color_to_color.frag and masseffect_edram_16f_to_16f.comp.
inline float From7e3Reference(uint32_t f10) {
  f10 &= 0x3FFu;
  if (f10 == 0u) return 0.0f;
  uint32_t mantissa = f10 & 0x7Fu;
  uint32_t exponent = f10 >> 7u;
  if (exponent == 0u) {
    uint32_t msb = 31u;
    while (!(mantissa >> msb)) --msb;
    const uint32_t displacement = 7u - msb;
    exponent = 1u - displacement;  // wraps, as uint arithmetic does in GLSL
    mantissa = (mantissa << displacement) & 0x7Fu;
  }
  return FloatOfBits(((exponent + 124u) << 23u) | (mantissa << 16u));
}

// A7e3 as written in the shaders (float -> 10-bit 7e3 code).
inline uint32_t A7e3Reference(float value) {
  const float clamped = std::isnan(value) ? 0.0f : (value < 0.0f ? 0.0f : (value > 31.875f ? 31.875f : value));
  const uint32_t f32 = BitsOfFloat(clamped);
  const uint32_t shift = 125u - (f32 >> 23u);
  const uint32_t denormal = ((f32 & 0x7FFFFFu) | 0x800000u) >> (shift < 24u ? shift : 24u);
  const uint32_t normal = f32 - (124u << 23u);
  const uint32_t biased = f32 < 0x3E800000u ? denormal : normal;
  return ((biased + 0x7FFFu + ((biased >> 16u) & 1u)) >> 16u) & 0x3FFu;
}

// Pack32 / Unpack32 of the conversion shaders (format 2 = UNORM10, 3 = 7e3), on host RGBA16F texel values.
inline uint32_t Pack32Model(const float v[4], uint32_t format) {
  if (format == 2u) {
    uint32_t n[4];
    for (int c = 0; c < 4; ++c)
      n[c] = uint32_t(RoundEvenModel(Clamp01Model(v[c]) * (c < 3 ? 1023.0f : 3.0f)));
    return n[0] | (n[1] << 10u) | (n[2] << 20u) | (n[3] << 30u);
  }
  return A7e3Reference(v[0]) | (A7e3Reference(v[1]) << 10u) | (A7e3Reference(v[2]) << 20u) |
         (uint32_t(RoundEvenModel(Clamp01Model(v[3]) * 3.0f)) << 30u);
}
inline void Unpack32Model(uint32_t word, uint32_t format, float out[4]) {
  if (format == 2u) {
    out[0] = float(word & 0x3FFu) / 1023.0f;
    out[1] = float((word >> 10u) & 0x3FFu) / 1023.0f;
    out[2] = float((word >> 20u) & 0x3FFu) / 1023.0f;
    out[3] = float(word >> 30u) / 3.0f;
    return;
  }
  out[0] = From7e3Reference(word);
  out[1] = From7e3Reference(word >> 10u);
  out[2] = From7e3Reference(word >> 20u);
  out[3] = float(word >> 30u) * (1.0f / 3.0f);
}

// The closed form of From7e3 the epilogue emits.
inline float Decode7e3Epilogue(uint32_t code) {
  return code < 128u ? float(code) * 0x1p-9f : FloatOfBits((code + (124u << 7u)) << 16u);
}

// The epilogue, operation for operation.
inline void Restore7e3EpilogueModel(const float x[4], float out[4]) {
  for (int c = 0; c < 3; ++c) {
    const uint32_t code = uint32_t(RoundEvenModel(Clamp01Model(QuantizeToF16Model(x[c])) * 1023.0f));
    out[c] = Decode7e3Epilogue(code);
  }
  const uint32_t a = uint32_t(RoundEvenModel(Clamp01Model(QuantizeToF16Model(x[3])) * 3.0f));
  out[3] = float(a) * (1.0f / 3.0f);
}

// Today's chain: f2 attachment store (half, given rounding) -> conversion f2 -> f3 (float32 written to f3).
inline void Restore7e3TodayModel(const float x[4], float out[4], bool rtz_attachment = false) {
  float h[4];
  for (int c = 0; c < 4; ++c) h[c] = F16ToF32(rtz_attachment ? F32ToF16Rtz(x[c]) : F32ToF16Rtne(x[c]));
  Unpack32Model(Pack32Model(h, 2u), 3u, out);
}

// ---- SPIR-V transform -------------------------------------------------------------------------------------------

struct Restore7e3Stats {
  uint32_t stores = 0;  // OpStores rewritten
};

/*
 * Rewrites every OpStore to the fragment output variable at each Location in `locations` (bit L) so that it
 * stores epilogue(value). Requirements (otherwise false and a reason; the caller then keeps the conversion):
 * one Fragment entry point, a GLSL.std.450 import, each selected output a direct Output float32 vec4 variable
 * without an Index/Component decoration, used only as the pointer of whole-vector OpStores (no loads, access
 * chains or function arguments), at least one store per selected output.
 */
inline bool TransformRestore7e3(const std::vector<uint32_t>& input, uint32_t locations,
                                std::vector<uint32_t>& output, Restore7e3Stats& stats, std::string& reason) {
  output.clear();
  reason.clear();
  stats = {};
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
  uint32_t entries = 0, glsl = 0, float_type = 0, uint_type = 0, bool_type = 0;
  std::unordered_map<uint32_t, uint32_t> location_of;     // id -> Location
  std::unordered_set<uint32_t> indexed;                   // ids with Index / Component decorations
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> vectors;   // id -> (component type, count)
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> pointers;  // id -> (storage, pointee)
  std::unordered_map<uint32_t, uint32_t> floats;           // id -> width
  std::unordered_map<uint32_t, uint32_t> output_variables; // id -> pointer type
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
      case 21: if (wc == 4 && w[2] == 32 && w[3] == 0 && !uint_type) uint_type = w[1]; break;
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
  // The selected outputs.
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
  if (found != locations) return fail("selected output Location not found");
  // Every use of a selected output in the function section must be the pointer of an OpStore. The operand
  // positions that can hold a pointer in a Logical-addressing Shader module are checked; the other words of
  // an instruction may be literals (extended instruction numbers, image operand masks, indices) that merely
  // equal the variable's id.
  const auto pointer_operand = [](uint32_t op, uint32_t k) {
    switch (op) {
      case 61: return k == 3;                                 // OpLoad
      case 62: return k == 1 || k == 2;                       // OpStore
      case 63: case 64: return k == 1 || k == 2;              // OpCopyMemory(Sized)
      case 65: case 66: case 67: case 70: return k == 3;      // access chains
      case 57: return k >= 4;                                 // OpFunctionCall arguments
      case 83: return k == 3;                                 // OpCopyObject
      case 12: return k >= 5;                                 // OpExtInst operands (Modf, Frexp, InterpolateAt)
      case 245: return k >= 3 && (k & 1);                     // OpPhi values
      case 169: return k >= 4;                                // OpSelect values
      case 124: return k == 3;                                // OpBitcast
      default: return false;
    }
  };
  for (const auto& ins : instructions) {
    if (ins.at < first_function) continue;
    const uint32_t* w = input.data() + ins.at;
    for (uint32_t k = 1; k < ins.wc; ++k) {
      if (!targets.count(w[k]) || !pointer_operand(ins.op, k)) continue;
      if (ins.op != 62 || k != 1) return fail("output read, chained or passed (not a plain store)");
      ++stats.stores;
    }
  }
  if (!stats.stores) return fail("selected output never stored");
  // New ids.
  uint32_t next = input[3];
  if (uint64_t(next) + uint64_t(stats.stores) * 64 + 64 >= 0x3FFFFFu) return fail("ID bound overflow");
  std::vector<uint32_t> declarations;
  const auto emit = [](std::vector<uint32_t>& dst, uint32_t op, std::initializer_list<uint32_t> args) {
    dst.push_back((uint32_t(args.size() + 1) << 16) | op);
    dst.insert(dst.end(), args);
  };
  if (!uint_type) { uint_type = next++; emit(declarations, 21, {uint_type, 32, 0}); }
  if (!bool_type) { bool_type = next++; emit(declarations, 20, {bool_type}); }
  const auto constant = [&](uint32_t type, uint32_t bits) {
    const uint32_t id = next++;
    emit(declarations, 43, {type, id, bits});
    return id;
  };
  const uint32_t f_zero = constant(float_type, 0x00000000u);
  const uint32_t f_one = constant(float_type, 0x3F800000u);
  const uint32_t f_1023 = constant(float_type, 0x447FC000u);
  const uint32_t f_three = constant(float_type, 0x40400000u);
  const uint32_t f_third = constant(float_type, 0x3EAAAAABu);   // float(1.0 / 3.0)
  const uint32_t f_denormal = constant(float_type, 0x3B000000u);  // 2^-9
  const uint32_t u_128 = constant(uint_type, 128u);
  const uint32_t u_bias = constant(uint_type, 124u << 7u);
  const uint32_t u_16 = constant(uint_type, 16u);
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
      const auto ext = [&](uint32_t instruction, std::initializer_list<uint32_t> operands) {
        const uint32_t id = next++;
        output.push_back((uint32_t(5 + operands.size()) << 16) | 12);  // OpExtInst
        output.insert(output.end(), {float_type, id, glsl, instruction});
        output.insert(output.end(), operands);
        return id;
      };
      // code = uint(Round(FClamp(QuantizeToF16(x), 0, 1) * scale))
      const auto code_of = [&](uint32_t component, uint32_t scale) {
        const uint32_t quantized = unary(116, float_type, component);          // OpQuantizeToF16
        const uint32_t clamped = ext(43, {quantized, f_zero, f_one});          // FClamp
        const uint32_t scaled = binary(133, float_type, clamped, scale);       // OpFMul
        const uint32_t rounded = ext(1, {scaled});                             // Round
        return unary(109, uint_type, rounded);                                 // OpConvertFToU
      };
      uint32_t channels[4];
      for (uint32_t c = 0; c < 4; ++c) {
        const uint32_t component = next++;
        emit(output, 81, {float_type, component, w[2], c});  // OpCompositeExtract
        if (c < 3) {
          const uint32_t code = code_of(component, f_1023);
          const uint32_t denormal = binary(133, float_type, unary(112, float_type, code), f_denormal);
          const uint32_t normal = unary(124, float_type,
                                        binary(196, uint_type, binary(128, uint_type, code, u_bias), u_16));
          const uint32_t is_denormal = binary(176, bool_type, code, u_128);  // OpULessThan
          channels[c] = next++;
          emit(output, 169, {float_type, channels[c], is_denormal, denormal, normal});  // OpSelect
        } else {
          const uint32_t code = code_of(component, f_three);
          channels[c] = binary(133, float_type, unary(112, float_type, code), f_third);
        }
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

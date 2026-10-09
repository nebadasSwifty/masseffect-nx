// The layout checks the runtime makes before it uses a 2008 (0x102A11xx) shader container
// (app/src/native/masseffect/masseffect_native_shaders.cpp Read(), the same checks as
// tools/check_shader_coverage.py Entry._read). A container that fails them is ignored by the game, so the
// offline tools treat it as "not a shader container":
//   * ue3_shader_scan finds containers by a byte pattern; random package bytes that match the pattern
//     (about 240 hits on the RU discs + DLC) fail these checks and are dropped,
//   * masseffect_hlsl refuses them before translation: XenosRecomp trusts every offset of the header
//     and reads far outside such a "container" (a crash natively, "memory access out of bounds" in
//     WebAssembly).
// No dependencies, header only: shared by the scanner (which has no include path) and the translator.
#pragma once

#include <cstddef>
#include <cstdint>

namespace shader_container {

inline uint32_t ReadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

// Returns nullptr if the container passes, else the reason (the wording of the runtime's Read()).
inline const char* LayoutProblem(const uint8_t* data, size_t size) {
  if (size < 28) return "container too short";
  const uint32_t signature = ReadBE32(data);
  if ((signature & 0xFFFFFF00u) != 0x102A1100u) return "not a 2008 (0x102A11xx) container";
  const bool vertex = (signature & 1) != 0;
  const uint64_t virtual_size = ReadBE32(data + 4);
  const uint64_t header = ReadBE32(data + 24);
  if (header + 8 > size) return "shader header outside the container";
  const uint64_t microcode_start = virtual_size + ReadBE32(data + header);
  const uint64_t microcode_size = ReadBE32(data + header + 4);
  if (microcode_size == 0 || microcode_size % 4 != 0 || microcode_start + microcode_size > size) {
    return "microcode outside the container";
  }
  if (header >= virtual_size || header + (vertex ? 40 : 32) > virtual_size) {
    return "shader header outside the virtual part";
  }
  return nullptr;
}

}  // namespace shader_container

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>

#include <rex/graphics/format/ucode.h>

namespace me::native {

// Semantic no-guest-KILL proof from the currently loaded HOST-order PS, not
// an OpKill count heuristic. This intentionally accepts only short straight
// ALU programs; textures, branches, loops, memexports and unknown opcodes fail
// closed. It does not execute code, inspect guest memory or prove no depth export
// (the separate stencil-clear gate must still prohibit depth exports).
inline bool ProveStraightLinePixelNoKill(std::span<const uint32_t> host_code) {
  using namespace rex::graphics::ucode;
  if (host_code.empty() || host_code.size() > 128 || host_code.size() % 3) return false;
  uint32_t cf_pairs = uint32_t(host_code.size() / 3);
  uint32_t instruction_count = 0;
  bool ended = false;
  for (uint32_t index = 0; index < cf_pairs * 2; ++index) {
    ControlFlowInstruction pair[2];
    UnpackControlFlowInstructions(host_code.data() + (index / 2) * 3, pair);
    const auto& cf = pair[index & 1];
    if (ended) {
      // Validate unreachable CF too: no latent exec address / nonlinear CF.
      if (cf.opcode() != ControlFlowOpcode::kNop) return false;
      continue;
    }
    switch (cf.opcode()) {
      case ControlFlowOpcode::kNop: break;
      case ControlFlowOpcode::kAlloc:
        if (cf.alloc.alloc_type() == AllocType::kMemory) return false;
        break;
      case ControlFlowOpcode::kExec:
      case ControlFlowOpcode::kExecEnd: {
        const auto& exec = cf.exec;
        cf_pairs = std::min(cf_pairs, exec.address());
        if (index / 2 >= cf_pairs || !exec.count() ||
            uint64_t(exec.address()) + exec.count() > host_code.size() / 3 ||
            (instruction_count += exec.count()) > 64) return false;
        for (uint32_t i = 0; i < exec.count(); ++i) {
          if ((exec.sequence() >> (2 * i)) & 1) return false;
          AluInstruction alu;
          std::memcpy(&alu, host_code.data() + 3 * (exec.address() + i), sizeof(alu));
          const uint32_t vector = uint32_t(alu.vector_opcode());
          const uint32_t scalar = uint32_t(alu.scalar_opcode());
          // KILL side effects happen even with no destination write mask and
          // can be predicated. Never exempt such instructions based on masks.
          if (vector >= uint32_t(AluVectorOpcode::kKillEq) &&
              vector <= uint32_t(AluVectorOpcode::kKillNe)) return false;
          if (scalar >= uint32_t(AluScalarOpcode::kKillsEq) &&
              scalar <= uint32_t(AluScalarOpcode::kKillsOne)) return false;
          if (vector > uint32_t(AluVectorOpcode::kMaxA) ||
              scalar > uint32_t(AluScalarOpcode::kRetainPrev) || scalar == 41) return false;
          if (alu.is_export() && alu.vector_dest() >= 32 &&
              alu.vector_dest() != uint32_t(ExportRegister::kPSDepth)) return false;
        }
        ended = cf.opcode() == ControlFlowOpcode::kExecEnd;
        break;
      }
      default: return false;
    }
  }
  return ended;
}

// Wider proof: no KILL instruction anywhere in the shader's exec blocks (conditional ones included);
// texture fetches and branches are allowed, they cannot discard a pixel. Alpha test / alpha to coverage
// are checked by the caller.
inline bool ProvePixelNoKillAnywhere(std::span<const uint32_t> host_code) {
  using namespace rex::graphics::ucode;
  if (host_code.empty() || host_code.size() > 3 * 4096 || host_code.size() % 3) return false;
  uint32_t cf_pairs = uint32_t(host_code.size() / 3);
  bool any_exec = false;
  for (uint32_t index = 0; index < cf_pairs * 2; ++index) {
    ControlFlowInstruction pair[2];
    UnpackControlFlowInstructions(host_code.data() + (index / 2) * 3, pair);
    const auto& cf = pair[index & 1];
    if (cf.opcode() == ControlFlowOpcode::kAlloc && cf.alloc.alloc_type() == AllocType::kMemory) return false;
    if (!IsControlFlowOpcodeExec(cf.opcode())) continue;
    const auto& exec = cf.exec;
    cf_pairs = std::min(cf_pairs, exec.address());
    if (uint64_t(exec.address()) + exec.count() > host_code.size() / 3) return false;
    any_exec = true;
    for (uint32_t i = 0; i < exec.count(); ++i) {
      if ((exec.sequence() >> (2 * i)) & 1) continue;  // fetch
      AluInstruction alu;
      std::memcpy(&alu, host_code.data() + 3 * (exec.address() + i), sizeof(alu));
      const uint32_t vector = uint32_t(alu.vector_opcode());
      const uint32_t scalar = uint32_t(alu.scalar_opcode());
      if (vector >= uint32_t(AluVectorOpcode::kKillEq) && vector <= uint32_t(AluVectorOpcode::kKillNe)) return false;
      if (scalar >= uint32_t(AluScalarOpcode::kKillsEq) && scalar <= uint32_t(AluScalarOpcode::kKillsOne)) return false;
      if (vector > uint32_t(AluVectorOpcode::kMaxA) || scalar > uint32_t(AluScalarOpcode::kRetainPrev) ||
          scalar == 41) return false;
    }
  }
  return any_exec;
}

}  // namespace me::native

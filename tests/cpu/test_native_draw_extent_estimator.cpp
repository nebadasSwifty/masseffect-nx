// Standalone CPU SDK fixture: no Vulkan device, game, shader package, or GPU.
#include "me_native_draw_extent_estimator.h"

#include <cassert>
#include <cstdio>
#include <string_view>

using namespace rex::graphics;

int main() {
  // Actual loaded utility VS extracted from the 2008 container's 108-byte code.
  // Full float3 r1 (explicit W=1) followed by mini float4 r0;
  // position is MAX r1,r1. Guest formats 57/38 are float3/float4, respectively.
  constexpr std::array<uint32_t, 27> original = {
      0x30052003, 0x00001200, 0xC4000000,
      0x00001005, 0x00001200, 0xC2000000,
      0x00001006, 0x10071200, 0x22000000,
      0x30081000, 0x00393A88, 0x00000007,
      0x00080000, 0x40263688, 0x00000300,
      0xC80F8000, 0x00000000, 0xC2000000,
      0xC80F803E, 0x00000000, 0xC2010100,
      0xC8000000, 0x00000000, 0x02000000,
      0x00000000, 0x00000000, 0x00000000};
  rex::memory::Memory memory;
  assert(memory.Initialize());
  constexpr uint32_t physical = 0x01000000;
  auto* heap = memory.LookupHeap(0xA0000000u + physical);
  assert(heap);
  assert(heap->AllocFixed(0xA0000000u + physical, 0x10000, 0x10000,
      rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite));
  auto* vertices = memory.TranslatePhysical<uint32_t*>(physical);
  auto set_vertices = [&](std::array<std::array<float, 2>, 3> xy) {
    for (uint32_t v = 0; v < 3; ++v) {
      const std::array<float, 7> data = {xy[v][0], xy[v][1], -0x1.000002p-28f,
                                       0, 0, 0, 0};
      for (uint32_t c = 0; c < 7; ++c) vertices[v * 7 + c] = std::bit_cast<uint32_t>(data[c]);
    }
  };
  const std::array<std::array<float, 2>, 3> axis = {{{-.5f, -.5f},
                                                  {-.5f, 439.5f},
                                                  {439.5f, -.5f}}};
  set_vertices(axis);
  std::array<uint32_t, RegisterFile::kRegisterCount> regs{};
  reg::VGT_DRAW_INITIATOR draw{};
  draw.prim_type = xenos::PrimitiveType::kRectangleList;
  draw.source_select = xenos::SourceSelect::kAutoIndex;
  draw.num_indices = 3;
  regs[XE_GPU_REG_VGT_DRAW_INITIATOR] = draw.value;
  regs[XE_GPU_REG_VGT_MAX_VTX_INDX] = 0xFFFFFF;
  regs[XE_GPU_REG_PA_CL_CLIP_CNTL] = 0x10000;
  regs[XE_GPU_REG_PA_CL_VTE_CNTL] = 0x300;
  regs[XE_GPU_REG_PA_SU_VTX_CNTL] = 4;
  regs[XE_GPU_REG_RB_SURFACE_INFO] = (2u << 16) | 440;
  xenos::xe_gpu_vertex_fetch_t fetch{};
  fetch.type = xenos::FetchConstantType::kVertex;
  fetch.address = physical / 4;
  fetch.size = 21;
  fetch.endian = xenos::Endian::kNone;
  regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0] = fetch.dword_0;
  regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 1] = fetch.dword_1;
  me::native::NativeDrawExtentEstimator estimator(memory);
  me::native::NativeDrawExtentEstimator::Diagnostics d;
  auto estimate = estimator.Estimate(regs, original, &d);
  if (!estimate) std::fprintf(stderr, "positive fixture rejected: %s\n", d.reason);
  assert(estimate && *estimate == 440);
  for (uint32_t v = 0; v < 3; ++v) {
    assert(d.position[v][0] == axis[v][0] && d.position[v][1] == axis[v][1]);
    assert(d.position[v][3] == 1.0f);
  }

  // Add an inert CF pair after actual CF5 ExecEnd, move the bodies one triple,
  // and adjust each EXEC address. Then corrupt unreachable CF6 into invalid
  // EXEC: SDK analysis must never inspect that out-of-range instruction address.
  std::array<uint32_t, 30> padded{};
  std::copy_n(original.begin(), 9, padded.begin());
  std::copy(original.begin() + 9, original.end(), padded.begin() + 12);
  ++padded[0];  // CF0 body3 ->4
  ++padded[3];  // CF2 body5 ->6
  ++padded[6];  // CF4 body6 ->7
  padded[7] += 0x10000;  // CF5 body7 ->8
  assert(estimator.Estimate(regs, padded, &d) == std::optional<uint32_t>(440));
  padded[9] = 0x1FFFu;
  padded[10] = 0x1000u;
  assert(!estimator.Estimate(regs, padded, &d));
  assert(std::string_view(d.reason) == "non-inert-cf-padding");

  // Full float3 position fetch now reads unavailable W instead of setting W=1.
  // That component is not native-equivalent despite CPU zero initialization.
  auto mutated = original;
  mutated[10] = (mutated[10] & ~0xFFFu) | 0x688u;  // XYZ1 -> XYZW.
  assert(!estimator.Estimate(regs, mutated, &d));
  if (std::string_view(d.reason) != "position-undefined-fetch-components")
    std::fprintf(stderr, "missing-W fixture reason: %s\n", d.reason);
  assert(std::string_view(d.reason) == "position-undefined-fetch-components");

  set_vertices({{{0, 0}, {100, 200}, {200, 0}}});
  assert(!estimator.Estimate(regs, original, &d));
  assert(std::string_view(d.reason) == "not-axis-aligned-corners");
  set_vertices(axis);
  regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0] &= ~3u;
  assert(!estimator.Estimate(regs, original, &d));
  assert(std::string_view(d.reason) == "fetch-constant-type");
  std::puts("native SDK draw extent estimator: PASS");
}

#include "me_stencil_clear_ownership.h"
#include <cassert>
#include <limits>

using namespace me::native;

static StencilClearOwnershipInput ActualClear() {
  StencilClearOwnershipInput in;
  in.depth_control = 0x8701;
  in.stencil_front = 0x00FFFF00;
  in.color_control = 7;
  in.su_sc_mode = 0x10000;
  in.vtx_cntl = 4;
  in.vte_cntl = 0x300;
  in.clip_cntl = 0x10000;
  in.msaa_enum = 2;
  in.physical_pitch_tiles = 16;
  in.scissor_x1 = in.scissor_y1 = 8192;
  in.post_vs_position = {{{-.5f,-.5f,0,1}, {639.5f,-.5f,0,1}, {639.5f,359.5f,0,1}}};
  in.rectangle_proven = in.ps_identity_proven = in.ps_no_discard =
      in.ps_no_depth_export = in.sample_coverage_proven_full = true;
  return in;
}

int main() {
  assert(!ProveStencilClearOwnership({}).eligible);
  const auto in = ActualClear();
  const auto proof = ProveStencilClearOwnership(in);
  assert(proof.eligible && proof.stencil_reference == 0);
  assert(proof.guest_width == 640 && proof.guest_height == 360);
  assert(proof.physical_width == 1280 && proof.physical_height == 720);
  assert(proof.length_tiles == 720 && 379 < proof.length_tiles);
  auto bad = [&](auto mutate) { auto changed = in; mutate(changed); assert(!ProveStencilClearOwnership(changed).eligible); };
  bad([](auto& p){p.depth_control |= 2;});
  bad([](auto& p){p.depth_control |= 4;});
  bad([](auto& p){p.depth_control &= ~1u;});
  bad([](auto& p){p.depth_control ^= 1u << 8;});
  bad([](auto& p){p.depth_control ^= 1u << 14;});
  bad([](auto& p){p.stencil_front &= ~(1u << 16);});
  for (uint32_t bit : {8u,16u}) bad([&](auto& p){p.color_control |= bit;});
  for (uint32_t bit : {1u,2u,8u}) bad([&](auto& p){p.su_sc_mode |= bit;});
  bad([](auto& p){p.vtx_cntl = 0;});
  bad([](auto& p){p.vte_cntl |= 1;});
  bad([](auto& p){p.vte_cntl &= ~0x100u;});
  bad([](auto& p){p.clip_cntl = 0;});
  bad([](auto& p){p.msaa_enum = 3;});
  bad([](auto& p){p.physical_pitch_tiles = 17;});
  bad([](auto& p){p.scissor_x0 = 1;});
  bad([](auto& p){p.scissor_y1 = 359;});
  bad([](auto& p){p.post_vs_position[2][1] -= .25f;});
  bad([](auto& p){p.post_vs_position[2][1] -= 1;});
  bad([](auto& p){p.post_vs_position[0][0] = 0;});
  bad([](auto& p){p.post_vs_position[1] = p.post_vs_position[0];});
  bad([](auto& p){p.post_vs_position[1][0] = 100;});
  bad([](auto& p){p.post_vs_position[0][0] = std::numeric_limits<float>::infinity();});
  bad([](auto& p){p.post_vs_position[0][2] = std::numeric_limits<float>::quiet_NaN();});
  bad([](auto& p){p.post_vs_position[2][3] = 2;});
  bad([](auto& p){p.rectangle_proven = false;});
  bad([](auto& p){p.ps_identity_proven = false;});
  bad([](auto& p){p.ps_no_discard = false;});
  bad([](auto& p){p.ps_no_depth_export = false;});
  bad([](auto& p){p.sample_coverage_proven_full = false;});
  auto back = in;
  back.depth_control |= 0x80u | (7u << 20) | (2u << 26);
  back.stencil_back = back.stencil_front;
  assert(ProveStencilClearOwnership(back).eligible);
  back.stencil_back ^= 1;
  assert(!ProveStencilClearOwnership(back).eligible);
  // Different references are allowed generically when both active faces agree.
  auto nonzero = in; nonzero.stencil_front |= 173;
  assert(ProveStencilClearOwnership(nonzero).stencil_reference == 173);
  // Window offsets may recover an exact origin; they are signed 15-bit fields.
  auto window = in; window.window_offset = 0x00020001;
  for (auto& p : window.post_vs_position) { p[0] -= 1; p[1] -= 2; }
  assert(ProveStencilClearOwnership(window).eligible);
  auto negative = in; negative.window_offset = 0x7FFF7FFF;
  for (auto& p : negative.post_vs_position) { p[0] += 1; p[1] += 1; }
  assert(ProveStencilClearOwnership(negative).eligible);
  // A 1x fully tile-aligned box is admitted by the same physical contract.
  auto single = in; single.msaa_enum = 0; single.physical_pitch_tiles = 8;
  for (auto& p : single.post_vs_position) if (p[1] > 0) p[1] = 351.5f;
  assert(ProveStencilClearOwnership(single).length_tiles == 176);
}

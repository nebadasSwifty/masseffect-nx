#include "me_resolved_sampling.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>

using namespace me::native;

int main() {
  ResolvedSamplingContract c{322, 182, 352, 352, 182, 8, true, true, true, true};
  auto p = AnalyzeResolvedSampling(c);
  assert(p.mode == ResolvedSamplingMode::Crop);
  assert(p.failure == ResolvedSamplingFailure::None);
  assert(p.texel_count == 58604 && p.copy_bytes == 468832);
  assert(p.source_bytes_required == (uint64_t(181) * 352 + 322) * 8);
  assert(AnalyzeResolvedSampling(c, p.copy_bytes).mode == ResolvedSamplingMode::Crop);
  assert(AnalyzeResolvedSampling(c, p.copy_bytes - 1).failure == ResolvedSamplingFailure::CopyLimit);
  assert(AnalyzeResolvedSampling(c, UINT64_MAX, p.source_bytes_required).mode == ResolvedSamplingMode::Crop);
  assert(AnalyzeResolvedSampling(c, UINT64_MAX, p.source_bytes_required - 1).failure == ResolvedSamplingFailure::SourceBounds);

  c = {1280, 720, 1280, 1280, 720, 4, true, true, true, true};
  assert(AnalyzeResolvedSampling(c).mode == ResolvedSamplingMode::Direct);
  assert(AnalyzeResolvedSampling(c).copy_bytes == 3686400);
  // SDK aliases 54->7 and 29->32 are explicitly proved by the caller.
  for (uint32_t texel_bytes : {4u, 8u}) {
    c.bytes_per_texel = texel_bytes;
    assert(AnalyzeResolvedSampling(c).mode == ResolvedSamplingMode::Direct);
  }
  c.base_formats_compatible = false;  // Real 32->7 mismatch, not an alias.
  assert(AnalyzeResolvedSampling(c).failure == ResolvedSamplingFailure::IncompatibleFormat);
  c.base_formats_compatible = true;
  c.logical_width = 1281;
  assert(AnalyzeResolvedSampling(c).failure == ResolvedSamplingFailure::ExtentOutsideBacking);
  c.logical_width = 1280;
  c.logical_height = 721;
  assert(AnalyzeResolvedSampling(c).failure == ResolvedSamplingFailure::ExtentOutsideBacking);
  c.logical_height = 720;
  c.guest_pitch = 1312;
  assert(AnalyzeResolvedSampling(c).failure == ResolvedSamplingFailure::WrongPitch);
  c.guest_pitch = 1280;
  for (int field = 0; field < 4; ++field) {
    auto empty = c;
    if (field == 0) empty.logical_width = 0;
    if (field == 1) empty.logical_height = 0;
    if (field == 2) empty.backing_width = 0;
    if (field == 3) empty.backing_height = 0;
    assert(AnalyzeResolvedSampling(empty).failure == ResolvedSamplingFailure::EmptyExtent);
  }
  for (int flag = 0; flag < 3; ++flag) {
    auto unproven = c;
    if (flag == 0) unproven.source_origin_zero = false;
    if (flag == 1) unproven.single_level_2d = false;
    if (flag == 2) unproven.scale_1x = false;
    assert(AnalyzeResolvedSampling(unproven).failure == ResolvedSamplingFailure::UnprovenLayout);
  }
  c.bytes_per_texel = 0;
  assert(AnalyzeResolvedSampling(c).failure == ResolvedSamplingFailure::InvalidTexelSize);
  c = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
       8, true, true, true, true};
  assert(AnalyzeResolvedSampling(c).failure == ResolvedSamplingFailure::Overflow);
  c = {1, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
       8, true, true, true, true};
  assert(AnalyzeResolvedSampling(c).failure == ResolvedSamplingFailure::Overflow);
  c = {1, 1, 1, 1, 1, 8, true, true, true, true};
  assert(AnalyzeResolvedSampling(c, 0).failure == ResolvedSamplingFailure::CopyLimit);
  assert(AnalyzeResolvedSampling(c, 8, 0).failure == ResolvedSamplingFailure::SourceBounds);
  assert(AnalyzeResolvedSampling(c, 8, 8).mode == ResolvedSamplingMode::Direct);
  std::cout << "resolved sampling contracts: PASS\n";
}

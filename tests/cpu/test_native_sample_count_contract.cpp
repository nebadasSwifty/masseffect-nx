#include "me_sample_count_contract.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
void Require(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "sample-count contract FAIL: %s\n", message);
    std::exit(1);
  }
}
}  // namespace

int main() {
  using namespace me::native;
  static_assert(IsSupportedHardwareSampleCount(1));
  static_assert(IsSupportedHardwareSampleCount(2));
  static_assert(IsSupportedHardwareSampleCount(4));
  static_assert(!IsSupportedHardwareSampleCount(0));
  static_assert(!IsSupportedHardwareSampleCount(3));
  static_assert(!IsSupportedHardwareSampleCount(8));
  static_assert(CommonHardwareSampleCount(std::array<uint32_t, 5>{0, 2, 0, 2, 2}) == 2);
  static_assert(CommonHardwareSampleCount(std::array<uint32_t, 5>{}) == 0);

  constexpr std::array<uint32_t, 12> values{0, 1, 2, 3, 4, 5, 7, 8, 16, 32, 64, 0xFFFFFFFFu};
  for (uint32_t count : values) {
    const bool valid = count == 1 || count == 2 || count == 4;
    Require(IsSupportedHardwareSampleCount(count) == valid, "bounded supported counts");
    Require(IsSingleSample(count) == (count == 1), "single-sample classification");
    Require(CanBufferImageTransferSamples(count) == (count == 1), "buffer-image transfer");
    for (uint32_t mask = 0; mask != 128; ++mask)
      Require(IsHardwareSampleCountSupportedByMask(count, mask) ==
                  (valid && (mask & count) == count), "exact supported mask bit");
    Require(IsHardwareSampleCountSupportedByMask(count, 0xFFFFFFFFu) == valid,
            "all-bit mask cannot admit invalid count");
    for (uint32_t other : values) {
      Require(CanCopyImageSamples(count, other) == (valid && count == other),
              "copy equal supported counts only");
      Require(CanBlitImageSamples(count, other) == (count == 1 && other == 1),
              "blit two single-sample images only");
    }
  }
  // Exhaust every sparse five-attachment combination, including invalid counts.
  constexpr std::array<uint32_t, 7> attachment_values{0, 1, 2, 3, 4, 8, 64};
  constexpr uint32_t combinations = 7 * 7 * 7 * 7 * 7;
  for (uint32_t pattern = 0; pattern < combinations; ++pattern) {
    std::array<uint32_t, 5> attachments{};
    uint32_t encoded = pattern, expected = 0;
    bool valid = true;
    for (uint32_t& count : attachments) {
      count = attachment_values[encoded % attachment_values.size()];
      encoded /= attachment_values.size();
      if (!count) continue;
      if ((count != 1 && count != 2 && count != 4) || (expected && expected != count))
        valid = false;
      if (!expected) expected = count;
    }
    Require(CommonHardwareSampleCount(attachments) == (valid ? expected : 0),
            "exhaustive sparse attachment combination");
  }
  const uint32_t raw[5]{0, 4, 0, 0, 4};
  Require(CommonHardwareSampleCount(raw) == 4, "raw five-slot array API");
  Require(!IsHardwareSampleCountSupportedByMask(2, 1 | 4), "2x requires its exact bit");
  std::puts("native hardware sample-count contract PASS (16,807 sparse combinations)");
}

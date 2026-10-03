#include "me_depth.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <set>
#include <utility>

// CPU transcription of the two experimental GLSL A20e4 implementations.
// This tests numeric/addressing contracts, not Vulkan shader execution.
static uint32_t ShaderA20e4(float z, bool round) {
  if (!(z > 0.0f)) return 0;
  uint32_t bits = std::bit_cast<uint32_t>(z);
  if (bits >= 0x3FFFFFF8u) return 0xFFFFFF;
  if (bits < 0x38800000u)
    bits = ((bits & 0x7FFFFF) | 0x800000) >> std::min(113u - (bits >> 23), 24u);
  else bits += 0xC8000000u;
  if (round) bits += 3u + ((bits >> 3) & 1u);
  return (bits >> 3) & 0xFFFFFF;
}

static uint32_t SwapColumn(uint32_t word) { return word < 40 ? word + 40 : word - 40; }

static uint32_t PackUNorm24(float z) {
  return uint32_t(std::nearbyint(std::clamp(z, 0.0f, 1.0f) * 16777215.0f));
}

// CPU transcription of the same-guest-format shader branch. This deliberately
// avoids guest pack/unpack and preserves all host mantissa bits.
static float TransferSameDepth(float z, bool source_half, bool dest_half) {
  if (source_half != dest_half) z *= source_half ? 2.0f : 0.5f;
  return z;
}

// xenos.h::UNorm24To32, independently of the shader's division expression.
static float SdkUNorm24To32(uint32_t bits) {
  return float(bits + (bits >> 23)) * 0x1p-24f;
}

int main() {
  using me::native::DecodeDepthFloat20e4;
  using me::native::NativeDepthClearValue;
  // Every guest float24 encoding is exactly representable in float32. Both
  // shader rounding modes must preserve every exactly decoded guest value.
  uint32_t lost_clear_codes = 0;
  for (uint32_t packed = 0; packed < 0x1000000; ++packed) {
    const float z = DecodeDepthFloat20e4(packed);
    assert(ShaderA20e4(z, false) == packed);
    assert(ShaderA20e4(z, true) == packed);
    const float host_half = NativeDepthClearValue(1u << 16, (packed << 8) | 0xA5u, true);
    assert(host_half >= 0.0f && host_half < 1.0f);
    const float restored = me::native::NativeDepthToGuest(host_half, true);
    assert(restored == z);
    assert(ShaderA20e4(restored, false) == packed);
    assert(ShaderA20e4(restored, true) == packed);
    if (ShaderA20e4(NativeDepthClearValue(1u << 16, packed << 8), false) != packed)
      ++lost_clear_codes;
  }
  assert(lost_clear_codes == 0xFFFFF);
  assert(me::native::GuestDepthToNative(1.0f, true) == 0.5f);
  assert(me::native::GuestDepthToNative(2.0f, true) == 1.0f);
  assert(((ShaderA20e4(me::native::NativeDepthToGuest(
      NativeDepthClearValue(1u << 16, 0xFFFFFFA5u, true), true), false) << 8) | 0xA5u) == 0xFFFFFFA5u);
  assert(ShaderA20e4(0.5f, false) == 0xE00000);
  assert(ShaderA20e4(1.0f, false) == 0xF00000);
  assert(ShaderA20e4(2.0f, false) == 0xFFFFFF);
  assert(ShaderA20e4(std::numeric_limits<float>::infinity(), true) == 0xFFFFFF);
  assert(ShaderA20e4(std::numeric_limits<float>::quiet_NaN(), true) == 0);
  assert(ShaderA20e4(-0.0f, true) == 0);
  // Adjacent source float32 halfway between two float24 values, even-low and
  // odd-low, respectively. This distinguishes nearest-even from truncation.
  assert(ShaderA20e4(std::bit_cast<float>(0x3F000004u), false) == 0xE00000);
  assert(ShaderA20e4(std::bit_cast<float>(0x3F000004u), true) == 0xE00000);
  assert(ShaderA20e4(std::bit_cast<float>(0x3F00000Cu), false) == 0xE00001);
  assert(ShaderA20e4(std::bit_cast<float>(0x3F00000Cu), true) == 0xE00002);

  const float half_scaled = 0.5f * 16777215.0f;
  assert(half_scaled == 8388607.5f);
  assert(uint32_t(std::nearbyint(half_scaled)) == 0x800000);
  // Important SDK ME1 clear regression: +0.5f itself rounds the odd integer
  // 8388609 upward in float32, while nearest-even preserves the integer.
  const float exact_odd = 8388609.0f;
  assert(uint32_t(std::nearbyint(exact_odd)) == 8388609u);
  assert(uint32_t(exact_odd + 0.5f) == 8388610u);

  // Different guest depth encodings reinterpret the packed word, not numeric Z.
  // Same-format transfers preserve host precision instead (SDK host RT path).
  // Both depth formats share the depth tile-column permutation: never apply
  // the color/depth 40-word swap between these two depth views.
  uint32_t sdk_unorm_roundtrip_loss = 0, division_unorm_roundtrip_loss = 0;
  for (uint32_t bits = 0; bits <= 0xFFFFFF; ++bits) {
    sdk_unorm_roundtrip_loss += PackUNorm24(SdkUNorm24To32(bits)) != bits;
    division_unorm_roundtrip_loss += PackUNorm24(float(bits) / 16777215.0f) != bits;
  }
  // Do not call the SDK decode a bit-exact inverse of this shader pack. Its
  // optimized binary scaling has half-ULP boundary cases; the SDK additionally
  // maintains host-depth ownership to avoid unnecessary round-trip conversion.
  assert(sdk_unorm_roundtrip_loss == 2097152);
  assert(division_unorm_roundtrip_loss == 0);
  std::printf("UNorm24 host decode/pack roundtrip differences: SDK decode %u; shader division %u\n",
              sdk_unorm_roundtrip_loss, division_unorm_roundtrip_loss);
  assert(PackUNorm24(1.0f) == 0xFFFFFF);
  const uint32_t unorm_half = PackUNorm24(0.5f);
  assert(unorm_half == 0x800000);
  assert(DecodeDepthFloat20e4(unorm_half) == 0x1p-7f);
  assert(DecodeDepthFloat20e4(PackUNorm24(1.0f)) == 2.0f - 0x1p-20f);
  // Current-native D24S8->D24FS8 1.0 import demonstrably loses these bits.
  assert(ShaderA20e4(NativeDepthClearValue(1u << 16, 0xFFFFFF00), false) == 0xF00000);
  assert(SdkUNorm24To32(ShaderA20e4(0.5f, false)) != 0.5f);
  assert(SdkUNorm24To32(ShaderA20e4(1.0f, false)) != 1.0f);
  for (uint32_t source_msaa = 0; source_msaa < 3; ++source_msaa) {
    for (uint32_t dest_msaa = 0; dest_msaa < 3; ++dest_msaa) {
      const uint32_t sx = source_msaa >= 2, sy = source_msaa >= 1;
      const uint32_t dx = dest_msaa >= 2, dy = dest_msaa >= 1;
      constexpr uint32_t source_first = 3, dest_first = 5, count = 9;
      constexpr uint32_t source_pitch = 7, dest_pitch = 13;
      for (uint32_t relative = 0; relative < count; ++relative) {
        for (uint32_t row = 0; row < 16; row += 1u << dy) {
          for (uint32_t column = 0; column < 80; column += 1u << dx) {
            const uint32_t dtile = dest_first + relative;
            const uint32_t dest_x = ((dtile % dest_pitch) * 80 + column) >> dx;
            const uint32_t dest_y = ((dtile / dest_pitch) * 16 + row) >> dy;
            const uint32_t physical_x = dest_x << dx, physical_y = dest_y << dy;
            const uint32_t recovered_tile = physical_y / 16 * dest_pitch + physical_x / 80;
            assert(recovered_tile == dtile);
            const uint32_t stile = source_first + recovered_tile - dest_first;
            const uint32_t source_x = ((stile % source_pitch) * 80 + physical_x % 80) >> sx;
            const uint32_t source_y = ((stile / source_pitch) * 16 + physical_y % 16) >> sy;
            assert(source_x == ((stile % source_pitch) * (80 >> sx) + (column >> sx)));
            assert(source_y == ((stile / source_pitch) * (16 >> sy) + (row >> sy)));
            // Swapping columns would select an unrelated depth sample.
            assert(source_x != (((stile % source_pitch) * 80 + SwapColumn(column)) >> sx));
            // Distinct valid host depths with arbitrary low mantissa bits,
            // including all values discarded by the old float24 round-trip.
            // Exercise this preservation at every mapped sample for all nine
            // source/destination MSAA pairs, not just at one numeric value.
            const uint32_t bits = 0x3E000000u |
                ((source_x * 109u + source_y * 4099u + relative) & 0x7FFFFFu);
            const float depth = std::bit_cast<float>(bits);
            for (bool source_half : {false, true}) {
              for (bool dest_half : {false, true}) {
                const float transferred = TransferSameDepth(depth, source_half, dest_half);
                const uint32_t expected = bits + (source_half == dest_half ? 0u :
                    (source_half ? 0x00800000u : uint32_t(-0x00800000)));
                assert(std::bit_cast<uint32_t>(transferred) == expected);
                assert(std::bit_cast<uint32_t>(TransferSameDepth(
                    transferred, dest_half, source_half)) == bits);
              }
            }
          }
        }
      }
    }
  }

  // Matching representations perform no arithmetic even for signed zero and
  // subnormals. Mismatched flags are exact power-of-two scaling for normal
  // renderable depths (extreme subnormal underflow is not asserted lossless).
  for (uint32_t bits : {0u, 0x80000000u, 1u, 7u, 0x007FFFFFu, 0x3E000001u,
                        0x3E000007u, 0x3E00000Fu, 0x3E7FFFFFu}) {
    const float depth = std::bit_cast<float>(bits);
    assert(std::bit_cast<uint32_t>(TransferSameDepth(depth, false, false)) == bits);
    assert(std::bit_cast<uint32_t>(TransferSameDepth(depth, true, true)) == bits);
  }
  const float precision_probe = std::bit_cast<float>(0x3E000007u);
  assert(DecodeDepthFloat20e4(ShaderA20e4(precision_probe, false)) != precision_probe);
  assert(TransferSameDepth(precision_probe, true, true) == precision_probe);

  for (uint32_t msaa = 0; msaa < 3; ++msaa) {
    const uint32_t mx = msaa >= 2, my = msaa >= 1;
    for (uint32_t word = 0; word < 80; ++word) {
      assert(SwapColumn(SwapColumn(word)) == word);
      // Shader swaps physical words first; SDK swaps tile pixels afterwards.
      const uint32_t source_pixel = word >> mx;
      const uint32_t half_tile = 40 >> mx;
      const uint32_t sdk_pixel = source_pixel < half_tile
          ? source_pixel + half_tile : source_pixel - half_tile;
      assert((SwapColumn(word) >> mx) == sdk_pixel);
    }
    for (uint32_t wide = 0; wide < 2; ++wide) {
      std::set<std::pair<uint32_t, uint32_t>> written;
      // Seven physically consecutive tiles, starting at odd tile index,
      // crossing rows of both 32bpp and 64bpp destination views.
      constexpr uint32_t first = 3, count = 7, pitch = 8;
      for (uint32_t qy = 0; qy < 16; ++qy) {
        for (uint32_t qx = 0; qx < count * 80; ++qx) {
          if ((qx & ((1u << (wide + mx)) - 1)) || (qy & ((1u << my) - 1))) continue;
          const uint32_t tile = first + qx / 80;
          const uint32_t local = qx % 80;
          if (wide) assert((local & 1) == 0 && local + 1 < 80);
          const uint32_t px = ((tile % pitch) * (80 >> wide) + (local >> wide)) >> mx;
          const uint32_t py = ((tile / pitch) * 16 + qy) >> my;
          assert(written.emplace(px, py).second);
        }
      }
      assert(written.size() == (count * 80 * 16) >> (wide + mx + my));
    }
  }

  // IEEE half interpretation of raw typeless depth words can yield specials.
  // Existing RGBA16F storage cannot promise to preserve NaN payload bits.
  const uint32_t cleared_word = (0xFFFFFFu << 8) | 255;
  assert((cleared_word & 0x7C00u) == 0x7C00u);
  assert((cleared_word & 0x03FFu) != 0);
  std::printf("Depth alias contracts passed (D24S8 exhaustive decode/pack; different-format depth reinterpretation; same-format host precision and half flags across all 9 MSAA grid pairs); legacy unhalved loss: %u float24 clear codes >1; legacy numeric FP16 conversion cannot preserve NaN payloads (MODE4 raw64 separately tested)\n",
              lost_clear_codes);
}

#include "me_depth.h"

#include <bit>
#include <cmath>
#include <cstdio>

namespace {
// Reference to rexglue-sdk/src/graphics/xenos.cpp Float20e4To32. Keep this
// unsigned-exponent implementation separate from the production signed one.
float SdkFloat20e4To32Reference(uint32_t f24) {
  f24 &= 0xFFFFFF;
  if (!f24) return 0.0f;
  uint32_t mantissa = f24 & 0xFFFFF;
  uint32_t exponent = f24 >> 20;
  if (!exponent) {
    uint32_t mantissa_lzcnt = std::countl_zero(mantissa) - (32 - 21);
    exponent = uint32_t(1 - int32_t(mantissa_lzcnt));
    mantissa = (mantissa << mantissa_lzcnt) & 0xFFFFF;
  }
  return std::bit_cast<float>(uint32_t(((exponent + 112) << 23) | (mantissa << 3)));
}
}

int main() {
  using namespace me::native;
  static_assert(DecodeDepthFloat20e4(0) == 0.0f);
  static_assert(DecodeDepthFloat20e4(0xE00000) == 0.5f);
  static_assert(DecodeDepthFloat20e4(0xF00000) == 1.0f);
  static_assert(DecodeDepthFloat20e4(0xFFFFFF) == 2.0f - 0x1p-20f);
  static_assert(DecodeDepthFloat20e4(1) == 0x1p-34f);
  static_assert(NativeDepthClearValue(0, 0xFFFFFFA5) == 1.0f);
  static_assert(NativeDepthClearValue(0, 0x000000A5) == 0.0f);
  static_assert(NativeDepthClearValue(1u << 16, 0xE00000A5) == 0.5f);
  static_assert(NativeDepthClearValue(1u << 16, 0xF00000A5) == 1.0f);
  static_assert(NativeDepthClearValue(1u << 16, 0xFFFFFFA5) == 1.0f);
  static_assert(NativeDepthClearValue(1u << 16, 0xFFFFFFA5, true) == 1.0f - 0x1p-21f);
  static_assert(NativeDepthClearValue(1u << 16, 0xF00000A5, true) == 0.5f);
  for (uint32_t bits = 0; bits <= 0xFFFFFF; ++bits) {
    const float decoded = DecodeDepthFloat20e4(bits);
    const float sdk = SdkFloat20e4To32Reference(bits);
    const uint32_t exponent = bits >> 20;
    const uint32_t mantissa = bits & 0xFFFFF;
    const float mathematical = exponent
        ? std::ldexp(float(0x100000u | mantissa), int(exponent) - 35)
        : std::ldexp(float(mantissa), -34);
    if (std::bit_cast<uint32_t>(decoded) != std::bit_cast<uint32_t>(sdk) ||
        decoded != mathematical ||
        NativeDepthToGuest(GuestDepthToNative(decoded, true), true) != decoded) {
      std::printf("depth decode mismatch at %06X\n", bits);
      return 1;
    }
    for (uint32_t stencil : {0u, 0xA5u, 0xFFu}) {
      const uint32_t clear = (bits << 8) | stencil;
      if (NativeDepthClearValue(0xA5A40001, clear) != float(bits) / 16777215.0f ||
          NativeDepthClearValue(0xA5A50001, clear) != std::min(decoded, 1.0f) ||
          NativeDepthClearValue(0xA5A50001, clear, true) != decoded * 0.5f) return 2;
    }
  }
  std::puts("depth: all 16777216 encodings match SDK reference and exact mathematical decode; D24S8/D24FS8 clear and stencil independence passed");
}

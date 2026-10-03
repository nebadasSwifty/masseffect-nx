// Isolated restore-bias contract. Optional actual witnesses:
// original.raw incoming.raw preceding.raw width height
// Files are D32 tight followed by S8 tight; no renderer/GPU is started.
#include "me_depth_quantize_spirv.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using me::native::DecodeDepthFloat20e4;
using me::native::QuantizeDepthHalfCpu;
constexpr uint32_t kActualBiasBits = 0x358637BDu; // Actual PS8580 c255.x, float32 1e-6.
static float Restore(float guest_texture) {
  // Material PS saturate(textureR - c255.x), then composed half export,
  // then final FLOAT24 nearest quantization. Keep actual float32 ordering.
  const float subtraction = guest_texture - std::bit_cast<float>(kActualBiasBits);
  const float saturated = std::clamp(subtraction, 0.0f, 1.0f);
  return QuantizeDepthHalfCpu(saturated * 0.5f, true);
}
static void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
static std::vector<float> Read(const char* path, size_t pixels) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  Require(bool(stream), "cannot open witness");
  Require(size_t(stream.tellg()) == pixels * 5, "witness D32/S8 byte count mismatch");
  stream.seekg(0);
  std::vector<float> depth(pixels);
  stream.read(reinterpret_cast<char*>(depth.data()), std::streamsize(pixels * 4));
  Require(bool(stream), "incomplete D32 payload");
  return depth;
}
struct Plane { double gx, gy, curvature; };
static Plane Derivative(const std::vector<float>& z, size_t at, size_t width) {
  const double center = z[at];
  return {(double(z[at + 1]) - z[at - 1]) * .5,
          (double(z[at + width]) - z[at - width]) * .5,
          std::max(std::abs(double(z[at + 1]) + z[at - 1] - 2 * center),
                   std::abs(double(z[at + width]) + z[at - width] - 2 * center))};
}
int main(int argc, char** argv) try {
  Require(std::endian::native == std::endian::little, "little-endian host required");
  Require(std::bit_cast<uint32_t>(1e-6f) == kActualBiasBits, "actual bias bits contract");
  const float delta = std::ldexp(1.0f, -21);
  size_t current_exponent = 0;
  // Every representable guest value strictly inside [.25,.5) has guest
  // FLOAT24 spacing 2^-22. Bias≈4.194304 such steps rounds to four steps;
  // the composed half range therefore differs by 2^-21. Avoid the exponent
  // transition at .25 itself, which deliberately has a different spacing.
  for (uint32_t code = 0xD00008u; code < 0xE00000u; ++code) {
    const float guest = DecodeDepthFloat20e4(code);
    const float incoming = QuantizeDepthHalfCpu(guest * .5f, true);
    Require(incoming - Restore(guest) == delta, "current-exponent restore delta mismatch");
    ++current_exponent;
  }
  // The same actual bias is NOT universally a constant delta: nearby
  // exponents and the clamp near zero must remain explicitly distinguished.
  const float small = DecodeDepthFloat20e4(0xB80000u);
  const float small_delta = QuantizeDepthHalfCpu(small * .5f, true) - Restore(small);
  Require(small_delta != delta && small_delta > 0, "bias incorrectly assumed exponent-independent");
  Require(Restore(0) == 0 && Restore(5e-7f) == 0, "near-zero saturate contract");
  Require(Restore(2.0f) == .5f, "guest PS saturate must precede half-range conversion");
  for (uint32_t boundary : {0xB00000u, 0xC00000u, 0xD00000u, 0xE00000u}) {
    for (int offset = -8; offset <= 8; ++offset) {
      const float guest = DecodeDepthFloat20e4(uint32_t(int64_t(boundary) + offset));
      Require(Restore(guest) <= QuantizeDepthHalfCpu(guest * .5f, true),
              "positive restore bias increased depth at exponent boundary");
      Require((std::bit_cast<uint32_t>(Restore(guest)) & 7u) == 0,
              "normal restored depth not FLOAT24-representable");
    }
  }
  if (argc != 1 && argc != 6) throw std::runtime_error("expected zero args or three raw paths width height");
  if (argc == 6) {
    const size_t width = std::stoul(argv[4]), height = std::stoul(argv[5]);
    Require(width >= 3 && height >= 3 && width <= 8192 && height <= 8192,
            "invalid witness extent");
    const auto original = Read(argv[1], width * height);
    const auto incoming = Read(argv[2], width * height);
    const auto preceding = Read(argv[3], width * height);
    size_t admitted = 0, mismatches = 0;
    constexpr double limit = 5e-8;
    for (size_t y = 1; y + 1 < height; ++y) for (size_t x = 1; x + 1 < width; ++x) {
      const size_t at = y * width + x;
      bool interior = true;
      for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
        const size_t pixel = size_t(int64_t(at) + dy * int64_t(width) + dx);
        interior &= incoming[pixel] != preceding[pixel];
      }
      if (!interior || !std::isfinite(original[at]) || !std::isfinite(incoming[at])) continue;
      const Plane old = Derivative(original, at, width), now = Derivative(incoming, at, width);
      if (!(old.curvature <= limit && now.curvature <= limit &&
            std::abs(old.gx - now.gx) <= limit && std::abs(old.gy - now.gy) <= limit)) continue;
      ++admitted;
      // Representable incoming value is the bounded candidate R32 guest
      // texture value. This checks the observed chain, not original raster
      // inputs equivalence outside these accepted surviving interiors.
      const float predicted = Restore(incoming[at] * 2.0f);
      mismatches += std::bit_cast<uint32_t>(predicted) != std::bit_cast<uint32_t>(original[at]);
    }
    std::cout << "actual admitted=" << admitted << " restore-bit-mismatches=" << mismatches << '\n';
    Require(admitted >= 16 && !mismatches, "actual bias chaining differs from accepted witnesses");
  }
  std::cout << "depth restore bias PASS current-exponent-codes=" << current_exponent
            << " actual-c255-bits=358637BD guest-bias=" << std::bit_cast<float>(kActualBiasBits)
            << " current-host-delta=" << delta << " other-exponent-delta=" << small_delta << '\n';
  return 0;
} catch (const std::exception& error) {
  std::cerr << "depth restore bias FAIL: " << error.what() << '\n';
  return 1;
}
